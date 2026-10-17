/* kernel/install/install.c -- SYS_INSTALL (NSI 105), DDR-1143 sec.3 / 10.6 / 10.9.
 *
 * Lays the sec.3 layout onto a whole physical disk:
 *
 *   LBA 0         stage1 + MBR table (P1 0xEF @4096, P2 0xDA @135168..end) + 'PRDI' @440
 *   LBA 1..16     stage2, zero-padded
 *   LBA 17..      the PRISTINE kernel (kimg_get), zero-padded through LBA 4095
 *   P1 (64 MiB)   FAT16: EFI/BOOT/BOOTX64.EFI and root KERNEL.BIN, both contiguous
 *   P2 LBA 0..7   volume header: PRDYVOL1, v1, flags=PLAINTEXT, sfs_off=8
 *   P2 LBA 8..    SFS, holding /INSTALLED.MARK = a 64-bit RNG nonce (16 hex)
 *
 * AUTHORITY (sec.10.6 finding 3): the caller must be THE BOOT CONSOLE SHELL,
 * i.e. current pid == the pid main.c recorded when it spawned PRISM. A fork of
 * PRISM has a new pid and is refused, as is every other process. Checked FIRST,
 * before any argument is even read.
 *
 * EVERY REFUSAL HAPPENS BEFORE THE FIRST WRITE: authority, confirmation
 * string, device class, size, the pristine-kernel copy, and the nonce. A
 * partially written disk is only possible from an I/O error, and that is
 * reported with the failing LBA.
 *
 * EVERY RAW WRITE IS READ BACK AND COMPARED (sec.4.5). blk_write takes
 * identity-mapped buffers (blk.h), and the embedded blobs live in the higher
 * half, so every byte is staged through a PMM page first.
 */
#include "syscall.h"
#include "sched.h"
#include "uaccess.h"
#include "errno.h"
#include "string.h"
#include "console.h"
#include "blk.h"
#include "pmm.h"
#include "kimg.h"
#include "crypto/rng.h"
#include "fs/vfs/vfs.h"
#include "fs/sfs/sfs.h"
#include "disk_info.h"
#include "ledger.h"
#include "crypto/sha256.h"

extern const unsigned char inst_stage1[], inst_stage1_end[];
extern const unsigned char inst_stage2[], inst_stage2_end[];
extern const unsigned char inst_efi[], inst_efi_end[];

static int32_t g_console_pid = -1;

void install_set_console_pid(uint32_t pid) { g_console_pid = (int32_t)pid; }

#define SEC     512u
#define CHUNK   8u                       /* sectors per staged write = one page */

/* ---- ESP (FAT16) geometry, sec.10.6 -------------------------------------- */
#define FAT_SPC      4u                  /* sectors per cluster (2 KiB)        */
#define FAT_RSVD     1u
#define FAT_SZ       128u                /* sectors per FAT                    */
#define FAT_ROOT_ENT 512u
#define FAT_ROOT_SEC (FAT_ROOT_ENT * 32u / SEC)                   /* 32 */
#define FAT_FAT1     (INST_ESP_LBA + FAT_RSVD)
#define FAT_FAT2     (FAT_FAT1 + FAT_SZ)
#define FAT_ROOT     (FAT_FAT2 + FAT_SZ)
#define FAT_DATA     (FAT_ROOT + FAT_ROOT_SEC)
#define FAT_CLUS_B   (FAT_SPC * SEC)                              /* 2048 */
#define FAT_NCLUS    ((INST_ESP_SECT - (FAT_DATA - INST_ESP_LBA)) / FAT_SPC)
_Static_assert(FAT_NCLUS >= 4085 && FAT_NCLUS < 65525, "ESP must count as FAT16");
_Static_assert((FAT_NCLUS + 2) * 2 <= FAT_SZ * SEC, "FAT16 table too small");

struct ictx {
    unsigned dev;
    uint8_t *w;                          /* staging page  (identity-mapped)    */
    uint8_t *r;                          /* readback page (identity-mapped)    */
};

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static void say_lba(const char *what, uint64_t lba) {
    kputs("[install] "); kputs(what); kputs(" lba="); kputdec(lba); kputs("\r\n");
}

/* Write `len` bytes from `src` (any kernel VA; NULL = zeros) at `lba`, zero
 * padded to whole sectors, one page at a time; read each page back and compare. */
static int wbytes(struct ictx *c, uint64_t lba, const uint8_t *src, uint64_t len) {
    uint64_t nsec = (len + SEC - 1) / SEC;
    for (uint64_t done = 0; done < nsec; done += CHUNK) {
        uint32_t n = (uint32_t)((nsec - done) < CHUNK ? (nsec - done) : CHUNK);
        uint64_t off = done * SEC, take = (uint64_t)n * SEC;
        memset(c->w, 0, take);
        if (src && off < len)
            memcpy(c->w, src + off, (len - off) < take ? (len - off) : take);
        if (blk_write(c->dev, lba + done, c->w, n) != 0) {
            say_lba("write FAILED", lba + done);
            return -EIO;
        }
        memset(c->r, 0xA5, take);
        if (blk_read(c->dev, lba + done, c->r, n) != 0 || memcmp(c->r, c->w, take) != 0) {
            say_lba("readback MISMATCH", lba + done);
            return -EIO;
        }
    }
    return 0;
}

/* 8.3 directory entry. */
static void dirent(uint8_t *e, const char name11[11], uint8_t attr,
                   uint32_t clus, uint32_t size) {
    memcpy(e, name11, 11);
    e[11] = attr;
    const uint32_t date = ((2026u - 1980u) << 9) | (1u << 5) | 1u;   /* fixed: reproducible */
    put16(e + 16, date);                 /* create date */
    put16(e + 18, date);                 /* access date */
    put16(e + 24, date);                 /* write date  */
    put16(e + 26, clus);
    put32(e + 28, size);
}

/* The FAT16 ESP. Layout: cluster 2 = /EFI, 3 = /EFI/BOOT, then BOOTX64.EFI,
 * then KERNEL.BIN, each contiguous. */
static int write_esp(struct ictx *c, const uint8_t *kern, uint64_t klen, uint32_t volid) {
    uint64_t elen = (uint64_t)(inst_efi_end - inst_efi);
    uint32_t e_cl = (uint32_t)((elen + FAT_CLUS_B - 1) / FAT_CLUS_B);
    uint32_t k_cl = (uint32_t)((klen + FAT_CLUS_B - 1) / FAT_CLUS_B);
    uint32_t e0 = 4, k0 = 4 + e_cl;
    if (k0 + k_cl > FAT_NCLUS + 2)
        return -ENOSPC;

    /* boot sector */
    static uint8_t bs[SEC];
    memset(bs, 0, sizeof bs);
    bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90;
    memcpy(bs + 3, "PRADYOS ", 8);
    put16(bs + 11, SEC);  bs[13] = FAT_SPC;  put16(bs + 14, FAT_RSVD);
    bs[16] = 2;           put16(bs + 17, FAT_ROOT_ENT);  put16(bs + 19, 0);
    bs[21] = 0xF8;        put16(bs + 22, FAT_SZ);
    put16(bs + 24, 63);   put16(bs + 26, 255);
    put32(bs + 28, INST_ESP_LBA);        put32(bs + 32, INST_ESP_SECT);
    bs[36] = 0x80;        bs[38] = 0x29; put32(bs + 39, volid);
    memcpy(bs + 43, "PRADYOSESP ", 11);  memcpy(bs + 54, "FAT16   ", 8);
    bs[510] = 0x55;       bs[511] = 0xAA;
    int rc = wbytes(c, INST_ESP_LBA, bs, SEC);
    if (rc) return rc;

    /* FAT: 64 KiB, built in a contiguous order-4 block, written twice. */
    uint64_t fp = pmm_alloc_pages(4);
    if (!fp) return -ENOMEM;
    uint8_t *fat = (uint8_t *)(uintptr_t)fp;
    memset(fat, 0, FAT_SZ * SEC);
    put16(fat + 0, 0xFFF8); put16(fat + 2, 0xFFFF);
    put16(fat + 2 * 2, 0xFFFF);          /* /EFI      one cluster */
    put16(fat + 3 * 2, 0xFFFF);          /* /EFI/BOOT one cluster */
    for (uint32_t i = 0; i < e_cl; i++)
        put16(fat + (e0 + i) * 2, (i + 1 == e_cl) ? 0xFFFF : e0 + i + 1);
    for (uint32_t i = 0; i < k_cl; i++)
        put16(fat + (k0 + i) * 2, (i + 1 == k_cl) ? 0xFFFF : k0 + i + 1);
    rc = wbytes(c, FAT_FAT1, fat, FAT_SZ * SEC);
    if (!rc) rc = wbytes(c, FAT_FAT2, fat, FAT_SZ * SEC);
    pmm_free_pages(fp, 4);
    if (rc) return rc;

    /* directories: root (32 sectors), /EFI and /EFI/BOOT (one cluster each) */
    static uint8_t d[FAT_ROOT_SEC * SEC];
    memset(d, 0, sizeof d);
    dirent(d,      "EFI        ", 0x10, 2, 0);
    dirent(d + 32, "KERNEL  BIN", 0x20, k0, (uint32_t)klen);
    /* The boot sector names a volume label, so the root must carry the
     * matching ATTR_VOLUME_ID entry; without it fsck.fat reports the pair
     * as inconsistent and exits 1 (measured on the first host readback). */
    dirent(d + 64, "PRADYOSESP ", 0x08, 0, 0);
    if ((rc = wbytes(c, FAT_ROOT, d, sizeof d))) return rc;

    memset(d, 0, FAT_CLUS_B);
    dirent(d,      ".          ", 0x10, 2, 0);
    dirent(d + 32, "..         ", 0x10, 0, 0);
    dirent(d + 64, "BOOT       ", 0x10, 3, 0);
    if ((rc = wbytes(c, FAT_DATA + 0 * FAT_SPC, d, FAT_CLUS_B))) return rc;

    memset(d, 0, FAT_CLUS_B);
    dirent(d,      ".          ", 0x10, 3, 0);
    dirent(d + 32, "..         ", 0x10, 2, 0);
    dirent(d + 64, "BOOTX64 EFI", 0x20, e0, (uint32_t)elen);
    if ((rc = wbytes(c, FAT_DATA + 1 * FAT_SPC, d, FAT_CLUS_B))) return rc;

    /* file bodies */
    if ((rc = wbytes(c, FAT_DATA + (uint64_t)(e0 - 2) * FAT_SPC, inst_efi, elen))) return rc;
    return wbytes(c, FAT_DATA + (uint64_t)(k0 - 2) * FAT_SPC, kern, klen);
}

static void hex16(char out[17], uint64_t v) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) { out[i] = hx[v & 15]; v >>= 4; }
    out[16] = 0;
}

/* DDR-1153: a fresh ledger seed for the TARGET, written to P2 sector 1 with
 * sha256(keygen(seed).pk) beside it. The running system's own key is not
 * touched. g_seed_pk keeps the pk so sys_install can print it once, after the
 * whole install has succeeded. */
static uint8_t g_seed_pk[LEDGER_PK_BYTES];
static uint8_t g_seed_fp[32];

static int write_seed(struct ictx *c) {
    static uint8_t sec[SEC];
    uint8_t seed[LEDGER_SEED_BYTES];
    if (ledger_new_seed(seed) != 0) {
        kputs("[install] refused: no entropy for the ledger seed\r\n");
        return -EIO;
    }
    int rc = ledger_derive_pk(seed, g_seed_pk);
    if (rc == 0) {
        sha256(g_seed_pk, LEDGER_PK_BYTES, g_seed_fp);
        memset(sec, 0, sizeof sec);
        memcpy(sec, INST_SEED_MAGIC, 8);
        put32(sec + 8, INST_SEED_VER);
        memcpy(sec + INST_SEED_OFF, seed, LEDGER_SEED_BYTES);
        memcpy(sec + INST_SEED_FP_OFF, g_seed_fp, 32);
        rc = wbytes(c, INST_P2_LBA + INST_SEED_LBA_OFF, sec, SEC);
    } else {
        kputs("[install] ledger keygen FAILED\r\n");
    }
    /* The seed exists only on the target disk from here on. */
    memset(seed, 0, sizeof seed);
    memset(sec, 0, sizeof sec);
    memset(c->w, 0, SEC);
    memset(c->r, 0, SEC);
    return rc;
}

/* The pk, once, in ledgertest's chunk format so ledger_verify.py reads it
 * unchanged: this is what the operator records off the machine (DDR-1150). */
static void print_seed_pk(void) {
    static const char hx[] = "0123456789ABCDEF";
    for (unsigned off = 0, i = 0; off < LEDGER_PK_BYTES; off += 64, i++) {
        kline k; kline_init(&k);
        kline_s(&k, "PRADYOS_LEDGER_PK n=0 i="); kline_d(&k, i); kline_c(&k, ' ');
        for (unsigned j = off; j < LEDGER_PK_BYTES && j < off + 64; j++) {
            kline_c(&k, hx[g_seed_pk[j] >> 4]); kline_c(&k, hx[g_seed_pk[j] & 15]);
        }
        kline_c(&k, '\r'); kline_c(&k, '\n');
        kline_emit(&k);
    }
    kline k; kline_init(&k);
    kline_s(&k, "[install] ledger fp=");
    static const char lx[] = "0123456789abcdef";    /* matches hashlib hexdigest() */
    for (unsigned j = 0; j < 8; j++) { kline_c(&k, lx[g_seed_fp[j] >> 4]); kline_c(&k, lx[g_seed_fp[j] & 15]); }
    kline_c(&k, '\r'); kline_c(&k, '\n');
    kline_emit(&k);
}

/* P2: header, then SFS on a partition sub-device, then the mark -- written,
 * unmounted, remounted and read back. */
static int write_p2(struct ictx *c, uint64_t p2len, uint64_t nonce, cap_t cap) {
    static uint8_t h[SEC];
    memset(h, 0, sizeof h);
    memcpy(h, INST_VOL_MAGIC, 8);
    put32(h + 8, 1);                          /* version */
    put32(h + 12, INST_VOL_PLAINTEXT);        /* flags: NOT encrypted, and says so */
    put32(h + 16, INST_P2_SFS_OFF);           /* SFS offset, sectors */
    int rc = wbytes(c, INST_P2_LBA, h, SEC);
    if (!rc) rc = write_seed(c);
    if (!rc) rc = wbytes(c, INST_P2_LBA + INST_SEED_LBA_OFF + 1, 0,
                         (INST_P2_SFS_OFF - INST_SEED_LBA_OFF - 1) * SEC);
    if (rc) return rc;

    int pi = blk_part_create(c->dev, INST_P2_LBA + INST_P2_SFS_OFF, p2len - INST_P2_SFS_OFF);
    if (pi < 0) { kputs("[install] P2 sub-device FAILED\r\n"); return pi; }
    struct blk_device *pbd = blk_get((unsigned)pi);
    if (!pbd || sfs_format(pbd) != 0) { kputs("[install] P2 sfs_format FAILED\r\n"); return -EIO; }

    char mark[17];
    hex16(mark, nonce);
    int mt = vfs_mount((unsigned)pi);
    if (mt < 0) { kputs("[install] P2 mount FAILED\r\n"); return -EIO; }
    struct vfs_file f;
    rc = (vfs_create(cap, mt, INST_MARK_PATH, &f) == 0 &&
          vfs_write(cap, &f, 0, mark, 16) == 16) ? 0 : -EIO;
    vfs_unmount(mt);
    if (rc) { kputs("[install] mark write FAILED\r\n"); return rc; }

    char back[17];
    memset(back, 0, sizeof back);
    mt = vfs_mount((unsigned)pi);
    if (mt < 0) { kputs("[install] P2 remount FAILED\r\n"); return -EIO; }
    rc = (vfs_open(cap, mt, INST_MARK_PATH, &f) == 0 &&
          vfs_read(cap, &f, 0, back, 16) == 16 && memcmp(back, mark, 16) == 0) ? 0 : -EIO;
    vfs_unmount(mt);
    if (rc) kputs("[install] mark readback MISMATCH\r\n");
    return rc;
}

/* (disk_idx, const char *confirm, uint64_t *nonce_out) -> 0 | -errno */
static long sys_install(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_thread;
    if (g_console_pid < 0 || t->pid != (uint32_t)g_console_pid)
        return -EPERM;                                   /* checked FIRST */

    char conf[48];
    size_t cl = 0;
    if (copyinstr(conf, (const void __user *)a2, sizeof conf, &cl) < 0)
        return -EFAULT;

    unsigned dev = (unsigned)a1;
    struct blk_device *bd = (a1 >= 0 && dev < blk_count()) ? blk_get(dev) : 0;
    if (!bd || !disk_name_phys(bd->name))
        return -EINVAL;                                  /* ramdisk / partition / virtual */

    char want[48];
    size_t nl = strlen(bd->name);
    if (nl + 8 > sizeof want)
        return -EINVAL;
    memcpy(want, "WIPE-", 5);
    memcpy(want + 5, bd->name, nl);
    size_t p = 5 + nl;
    if (dev >= 10) want[p++] = (char)('0' + dev / 10);
    want[p++] = (char)('0' + dev % 10);
    want[p] = 0;
    if (strcmp(conf, want) != 0)
        return -EINVAL;                                  /* before ANY write */

    uint64_t cap_s = bd->capacity_sectors;
    if (cap_s < INST_MIN_SECT)
        return -ENOSPC;
    if (cap_s > 0xFFFFFFFFull)
        return -EINVAL;                                  /* MBR's 32-bit LBAs */

    uint64_t kphys, klen;
    if (kimg_get(&kphys, &klen) != 0) {
        kputs("[install] refused: no pristine kernel copy\r\n");
        return -ENODEV;
    }
    uint64_t s1len = (uint64_t)(inst_stage1_end - inst_stage1);
    uint64_t s2len = (uint64_t)(inst_stage2_end - inst_stage2);
    if (s1len != SEC || s2len > 16u * SEC || 17u + (klen + SEC - 1) / SEC > INST_ESP_LBA)
        return -EINVAL;

    uint64_t nonce = 0;
    if (rng_bytes(&nonce, sizeof nonce) != 0) {          /* fail closed */
        kputs("[install] refused: no entropy source\r\n");
        return -EIO;
    }

    struct ictx c;
    c.dev = dev;
    uint64_t wp = pmm_alloc_page(), rp = pmm_alloc_page();
    if (!wp || !rp) {
        if (wp) pmm_free_page(wp);
        if (rp) pmm_free_page(rp);
        return -ENOMEM;
    }
    c.w = (uint8_t *)(uintptr_t)wp;
    c.r = (uint8_t *)(uintptr_t)rp;
    const uint8_t *kern = (const uint8_t *)(uintptr_t)kphys;
    uint64_t p2len = cap_s - INST_P2_LBA;

    kputs("[install] begin dev="); kputdec(dev);
    kputs(" sectors="); kputdec(cap_s); kputs("\r\n");

    /* LBA 0: stage1 + table + signature */
    static uint8_t mbr[SEC];
    memcpy(mbr, inst_stage1, SEC);
    memcpy(mbr + DISK_INSTALL_SIG_OFF, DISK_INSTALL_SIG, 4);
    mbr[444] = 0; mbr[445] = 0;
    blk_mbr_set(mbr, 0, 0xEF, 0x80, INST_ESP_LBA, INST_ESP_SECT);
    blk_mbr_set(mbr, 1, 0xDA, 0x00, INST_P2_LBA, (uint32_t)p2len);
    blk_mbr_set(mbr, 2, 0, 0, 0, 0);
    blk_mbr_set(mbr, 3, 0, 0, 0, 0);
    mbr[510] = 0x55; mbr[511] = 0xAA;

    /* 1..16: stage2 at its EXACT length, then zeros. Passing 16 sectors as the
     * length copied past inst_stage2_end into the next blob -- caught by the
     * host-side readback of the installed disk, not by the in-kernel one, which
     * only proves the disk holds what was sent. */
    int rc = wbytes(&c, 1, inst_stage2, s2len);
    uint64_t s2end = 1 + (s2len + SEC - 1) / SEC;
    if (!rc && s2end < 17) rc = wbytes(&c, s2end, 0, (17 - s2end) * SEC);
    if (!rc) rc = wbytes(&c, 17, kern, klen);                      /* kernel  */
    uint64_t kend = 17 + (klen + SEC - 1) / SEC;
    if (!rc) rc = wbytes(&c, kend, 0, (INST_ESP_LBA - kend) * SEC); /* gap    */
    if (!rc) rc = write_esp(&c, kern, klen, (uint32_t)nonce);
    if (!rc) rc = write_p2(&c, p2len, nonce, t->fs_cap);
    /* The MBR goes LAST: until it is written the disk carries no 'PRDI', so a
     * failure anywhere above leaves a disk root selection will not adopt. */
    if (!rc) rc = wbytes(&c, 0, mbr, SEC);
    if (!rc) {
        int f = blk_flush(dev);
        if (f != 0) { kputs("[install] flush FAILED rc="); kputdec((uint64_t)(-f)); kputs("\r\n"); rc = f; }
    }
    pmm_free_page(wp);
    pmm_free_page(rp);
    if (rc) {
        kputs("[install] FAILED\r\n");
        return rc;
    }
    char nh[17];
    hex16(nh, nonce);
    print_seed_pk();
    kputs("[install] ok nonce="); kputs(nh); kputs("\r\n");
    if (a3 && copyout((void __user *)a3, &nonce, sizeof nonce) < 0)
        return -EFAULT;
    return 0;
}

void sys_install_register(void) {
    syscall_register(SYS_INSTALL, sys_install);          /* NSI 105 */
}

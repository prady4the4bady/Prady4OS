/* kernel/acpi/acpi.c — RSDP/RSDT/XSDT discovery + FADT S5 poweroff (DDR-746). */
#include "acpi.h"
#include "console.h"
#include "string.h"
#include "io.h"

struct rsdp {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;          /* covers the first 20 bytes (ACPI 1.0) */
    char     oemid[6];
    uint8_t  revision;          /* 0 = ACPI 1.0, >=2 = 2.0+ (XSDT present) */
    uint32_t rsdt_address;
    uint32_t length;            /* ACPI 2.0+ */
    uint64_t xsdt_address;      /* ACPI 2.0+ */
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

static const struct acpi_sdt_header *g_root;   /* RSDT or XSDT */
static int      g_use_xsdt;
static uint32_t g_count;                        /* number of table pointers */

static uint8_t sum8(const void *p, uint32_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint8_t s = 0;
    for (uint32_t i = 0; i < n; i++)
        s += b[i];
    return s;
}

/* Scan the BIOS area for the RSDP (QEMU/SeaBIOS place it in 0xE0000..0xFFFFF). */
/* Signature + BOTH checksums. The ACPI 1.0 checksum covers only the first 20
 * bytes, which stop at rsdt_address — it says nothing about `xsdt_address`,
 * the very field acpi_init() dereferences for revision >= 2. ACPI 6.x §5.2.5.3
 * puts that field under the EXTENDED checksum, taken over the whole structure,
 * so a revision-2 RSDP is only trustworthy once that sums to zero too. The spec
 * fixes the revision-2 structure at 36 bytes (== sizeof(struct rsdp)), so a
 * length that disagrees is malformed rather than a future extension to tolerate.
 *
 * Shared by both discovery paths deliberately: validating only the loader hint
 * would let the legacy scan re-admit the same malformed table on fallback. */
static int rsdp_ok(const char *p) {
    if (memcmp(p, "RSD PTR ", 8) != 0 || sum8(p, 20) != 0)
        return 0;
    const struct rsdp *r = (const struct rsdp *)p;
    if (r->revision >= 2 &&
        (r->length != (uint32_t)sizeof(struct rsdp) ||
         sum8(p, sizeof(struct rsdp)) != 0))
        return 0;
    return 1;
}

static const struct rsdp *find_rsdp(void) {
    for (uint64_t a = 0xE0000; a < 0x100000; a += 16) {
        const char *p = (const char *)(uintptr_t)a;
        if (rsdp_ok(p))
            return (const struct rsdp *)p;
    }
    return 0;
}

/* DDR-978: validate a loader-supplied RSDP before trusting it, so a garbage,
 * truncated or stale value is rejected and we fall back rather than following a
 * wild pointer into whatever is mapped there. */
static const struct rsdp *validate_rsdp(uint64_t pa) {
    if (!pa)
        return 0;
    const char *p = (const char *)(uintptr_t)pa;
    return rsdp_ok(p) ? (const struct rsdp *)p : 0;
}

void acpi_init(uint64_t rsdp_hint) {
    /* DDR-978: prefer the address the boot loader handed us. UEFI firmware
     * publishes the RSDP through the EFI Configuration Table and is NOT obliged
     * to mirror it into 0xE0000..0xFFFFF -- OVMF does not -- so on the UEFI path
     * the legacy scan below finds nothing and the kernel ends up with no MCFG
     * (PCIe enumerates zero devices), no MADT and no FADT. The BIOS path passes
     * 0 here and is unaffected. */
    const struct rsdp *r = validate_rsdp(rsdp_hint);
    if (r) {
        kputs("ACPI: RSDP from loader\r\n");
    } else {
        if (rsdp_hint)
            kputs("ACPI: loader RSDP rejected (bad signature/checksum)\r\n");
        r = find_rsdp();
    }
    if (!r) {
        kputs("ACPI: RSDP not found\r\n");
        return;
    }
    if (r->revision >= 2 && r->xsdt_address) {
        g_root = (const struct acpi_sdt_header *)(uintptr_t)r->xsdt_address;
        g_use_xsdt = 1;
    } else {
        g_root = (const struct acpi_sdt_header *)(uintptr_t)(uint64_t)r->rsdt_address;
        g_use_xsdt = 0;
    }
    g_count = (g_root->length - (uint32_t)sizeof(struct acpi_sdt_header)) /
              (g_use_xsdt ? 8u : 4u);

    kputs("ACPI: ");
    kputs(g_use_xsdt ? "XSDT" : "RSDT");
    kputs(", ");
    kputdec(g_count);
    kputs(" tables (rev ");
    kputdec(r->revision);
    kputs(")\r\n");
}

const struct acpi_sdt_header *acpi_find_table(const char sig[4]) {
    if (!g_root)
        return 0;
    const uint8_t *ents = (const uint8_t *)g_root + sizeof(struct acpi_sdt_header);
    for (uint32_t i = 0; i < g_count; i++) {
        uint64_t addr = g_use_xsdt
            ? ((const uint64_t *)ents)[i]
            : (uint64_t)((const uint32_t *)ents)[i];
        const struct acpi_sdt_header *h = (const struct acpi_sdt_header *)(uintptr_t)addr;
        if (memcmp(h->signature, sig, 4) == 0)
            return h;
    }
    return 0;
}

/* ---- DDR-746: FADT (FACP) parse + ACPI S5 (soft-off) poweroff ------------- */

/* PM1x_CNT SLP_EN bit (ACPI 6.x §4.8.3.2.1). SLP_TYP occupies bits 10..12. */
#define ACPI_SLP_EN (1u << 13)

static uint16_t g_pm1a_cnt, g_pm1b_cnt;   /* PM1{a,b} control I/O ports        */
static uint8_t  g_slp_typa, g_slp_typb;   /* \_S5_ sleep-type values (3 bits)  */
static uint8_t  g_s3_typa,  g_s3_typb;    /* DDR-892: \_S3_ sleep-type values  */
static int      g_s3_ok;                  /* DDR-892: \_S3_ was found          */
static int      g_s5_ok;                  /* set once the S5 path is resolved  */
static uint16_t g_reset_port;             /* FADT RESET_REG I/O port (0 = none) */
static uint8_t  g_reset_value;            /* FADT RESET_VALUE                   */

/* Little-endian u32 at a byte offset into a table (FADT fields are unaligned). */
static uint32_t acpi_rd32(const void *base, uint32_t off) {
    const uint8_t *b = (const uint8_t *)base + off;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

void acpi_power_init(void) {
    const struct acpi_sdt_header *fadt = acpi_find_table("FACP");
    if (!fadt) { kputs("ACPI: no FADT (no S5)\r\n"); return; }

    /* FADT field offsets (ACPI 6.x §5.2.9): DSDT=40, PM1a_CNT_BLK=64,
     * PM1b_CNT_BLK=66. The 32-bit fields are always populated by QEMU/SeaBIOS
     * and the tables live in identity-mapped low RAM (see acpi_find_table). */
    uint32_t dsdt = acpi_rd32(fadt, 40);
    g_pm1a_cnt = (uint16_t)acpi_rd32(fadt, 64);
    g_pm1b_cnt = (uint16_t)acpi_rd32(fadt, 66);
    if (!g_pm1a_cnt || !dsdt) { kputs("ACPI: FADT lacks PM1a/DSDT (no S5)\r\n"); return; }

    /* Minimal \_S5_ scan (ACPI §7.4.2): find the "_S5_" NameOp and read the
     * first two integer elements of the following Package — SLP_TYPa/b. This is
     * NOT a full AML interpreter; if _S5_ is hidden behind AML control flow the
     * scan fails and poweroff becomes a no-op. */
    const struct acpi_sdt_header *d = (const struct acpi_sdt_header *)(uintptr_t)dsdt;
    const uint8_t *aml = (const uint8_t *)d + sizeof *d;
    uint32_t n = d->length - (uint32_t)sizeof *d;
    /* DDR-892 (item 27): the SAME scan finds \_S3_ as well as \_S5_. The two
     * objects have identical shape, so the digit is a parameter rather than a
     * second copy of the walker — a duplicated scanner is a second place for the
     * PkgLength arithmetic to be wrong. */
    for (uint32_t i = 0; i + 6 < n; i++) {
        char digit = 0;
        if (aml[i] == '_' && aml[i+1] == 'S' && aml[i+3] == '_' &&
            (aml[i+2] == '5' || aml[i+2] == '3'))
            digit = (char)aml[i+2];
        if (!digit)
            continue;
        /* Valid form: NameOp(0x08) ["\\"] "_S5_" PackageOp(0x12). */
        int name_ok = (i >= 1 && aml[i-1] == 0x08) ||
                      (i >= 2 && aml[i-2] == 0x08 && aml[i-1] == '\\');
        if (!name_ok || aml[i+4] != 0x12)
            continue;
        const uint8_t *p = aml + i + 5;         /* past "_Sx_" + PackageOp        */
        p += ((*p & 0xC0u) >> 6) + 2;           /* skip PkgLength(+extra) + NumElem */
        if (*p == 0x0A) p++;                    /* optional BytePrefix            */
        uint8_t ta = *p++;
        if (*p == 0x0A) p++;
        uint8_t tb = *p;
        if (digit == '5') {
            g_slp_typa = ta; g_slp_typb = tb; g_s5_ok = 1;
        } else {
            g_s3_typa  = ta; g_s3_typb  = tb; g_s3_ok = 1;
        }
        if (g_s5_ok && g_s3_ok)
            break;                              /* both found; nothing else to scan */
    }

    /* DDR-892: count RAW "_S3_" byte occurrences, independent of the NameOp
     * validation above. This separates "our scanner rejected it" from "the
     * firmware never published it" — two conclusions with opposite fixes, and
     * without this the S3 path reports the same line either way. */
    {
        uint32_t raw3 = 0;
        for (uint32_t i = 0; i + 4 <= n; i++)
            if (aml[i] == '_' && aml[i+1] == 'S' && aml[i+2] == '3' && aml[i+3] == '_')
                raw3++;
        { kline k; kline_init(&k);                   /* DDR-1055 */
          kline_s(&k, "[acpi] DSDT _S3_ occurrences=");
          kline_d(&k, raw3);
          kline_s(&k, " parsed=");
          kline_d(&k, (uint64_t)g_s3_ok);
          kline_s(&k, "\n"); kline_emit(&k); }
    }

    /* DDR-747: FADT reset register (ACPI §5.2.9). Flags@112 bit10 =
     * RESET_REG_SUPPORTED; RESET_REG is a GAS@116 (byte0 = address_space_id,
     * bytes4..11 = address); RESET_VALUE@128. Only a System-I/O (id==1) register
     * is used — memory/PCI-config reset spaces need a different access path and
     * the 0xCF9/8042 fallbacks in acpi_reboot() cover QEMU regardless. */
    uint32_t flags = acpi_rd32(fadt, 112);
    if ((flags & (1u << 10)) && ((const uint8_t *)fadt)[116] == 1) {
        uint64_t addr = 0;
        for (int k = 0; k < 8; k++)
            addr |= (uint64_t)((const uint8_t *)fadt)[116 + 4 + k] << (8 * k);
        if (addr && addr <= 0xFFFFu) {
            g_reset_port  = (uint16_t)addr;
            g_reset_value = ((const uint8_t *)fadt)[128];
        }
    }

    kputs("ACPI: FADT ok, S5 ");
    kputs(g_s5_ok ? "found" : "NOT found");
    kputs(g_reset_port ? ", reset-reg\r\n" : "\r\n");
}

int acpi_power_available(void) { return g_s5_ok; }

int acpi_s3_available(void) { return g_s3_ok; }

/* DDR-892 (item 27): S3 suspend-to-RAM. DISCOVERY is complete; ENTRY is closed.
 *
 * Entering S3 without a resume trampoline is not a bug that yields a wrong
 * answer — it powers the CPU down and never returns. In a gate that is
 * indistinguishable from a hung QEMU; on hardware it is a box that needs the
 * power button. A capability that would brick the run is not left enabled in the
 * hope the caller knows better.
 *
 * On resume the firmware re-enters in REAL MODE at FACS.firmware_waking_vector,
 * so long mode, CR3, GDT, IDT, TSS and every per-CPU MSR must be rebuilt before
 * any C runs. arch/x86_64/ap_boot.asm already does exactly that walk for SMP
 * bring-up; the resume path is that code with a different tail. See DDR-892 §3.
 */
int acpi_suspend_s3(void) {
    if (!g_s3_ok || !g_pm1a_cnt) {
        kputs("[acpi] S3 unavailable: no _S3_ in DSDT\r\n");
        return -1;
    }
    kputs("[acpi] S3 refused: no resume path (waking vector unset)\r\n");
    return -1;
}

__attribute__((noreturn)) void acpi_poweroff(void) {
    /* Sentinel BEFORE the port write — the machine may power off immediately. */
    kputs("PRADYOS_POWEROFF\r\n");
    if (g_s5_ok && g_pm1a_cnt) {
        outw(g_pm1a_cnt, (uint16_t)(((uint16_t)(g_slp_typa & 7u) << 10) | ACPI_SLP_EN));
        if (g_pm1b_cnt)
            outw(g_pm1b_cnt, (uint16_t)(((uint16_t)(g_slp_typb & 7u) << 10) | ACPI_SLP_EN));
    }
    /* Still running -> S5 unavailable or refused. Halt this CPU forever. */
    for (;;)
        __asm__ volatile("cli; hlt");
}

__attribute__((noreturn)) void acpi_reboot(void) {
    kputs("PRADYOS_REBOOT\r\n");
    /* 1. FADT reset register (if a System-I/O one was parsed). */
    if (g_reset_port)
        outb(g_reset_port, g_reset_value);
    /* 2. PCI reset-control port 0xCF9: SYS_RST|RST_CPU|FULL_RST. */
    outb(0xCF9, 0x0E);
    /* 3. 8042 keyboard controller: pulse the CPU reset line. */
    outb(0x64, 0xFE);
    /* All reset paths failed (or are disabled) — halt this CPU forever. */
    for (;;)
        __asm__ volatile("cli; hlt");
}

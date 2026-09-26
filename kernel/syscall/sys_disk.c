/* kernel/syscall/sys_disk.c -- SYS_DISK_LIST (NSI 104), DDR-1143 sec.10.6/10.8.
 *
 * READ-ONLY and open to any process: it reports what the registry holds and
 * what sector 0 of each device looks like, and changes nothing. The disk-wiping
 * call (NSI 105) is the one with an authority check; listing is not.
 *
 * Content flags come from ONE read of sector 0 per device. A read that fails
 * sets DISK_F_READERR and claims NO content flag -- a device we could not read
 * must never be reported as BLANK, because "blank" is what an installer reads
 * as "safe to overwrite".
 *
 * PHYS is a positive allowlist of real controller drivers, not "anything that
 * is not a ramdisk or partition": the partition self-test's trace wrapper and
 * any future virtual device must not become an install target by default. */
#include "syscall.h"
#include "sched.h"
#include "uaccess.h"
#include "errno.h"
#include "string.h"
#include "blk.h"
#include "pmm.h"
#include "../install/disk_info.h"

_Static_assert(sizeof(struct disk_info) == 32, "SYS_DISK_LIST wire format moved");

static int name_is(const char *a, const char *b) {
    return a && strcmp(a, b) == 0;
}

static uint32_t disk_flags(unsigned i, struct blk_device *bd, uint8_t *sec) {
    uint32_t f = 0;
    if (name_is(bd->name, "virtio-blk") || name_is(bd->name, "nvme0") ||
        name_is(bd->name, "ahci"))
        f |= DISK_F_PHYS;
    else if (name_is(bd->name, "ramdisk"))
        f |= DISK_F_RAMDISK;
    else if (name_is(bd->name, "part"))
        f |= DISK_F_PART;
    if (!sec || bd->capacity_sectors == 0 || blk_read(i, 0, sec, 1) != 0)
        return f | DISK_F_READERR;
    int zero = 1;
    for (unsigned k = 0; k < 512; k++)
        if (sec[k]) { zero = 0; break; }
    if (zero)
        f |= DISK_F_BLANK;
    else if (sec[510] == 0x55 && sec[511] == 0xAA &&
             memcmp(sec + DISK_INSTALL_SIG_OFF, DISK_INSTALL_SIG, 4) == 0)
        f |= DISK_F_INSTALLED;
    return f;
}

/* (struct disk_info *out, n) -> total device count; min(n, total) entries
 * copied. out == 0 with n == 0 is a pure count. -EFAULT on a bad buffer. */
static long sys_disk_list(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    unsigned total = blk_count();
    uint64_t want = (uint64_t)a2;
    if (want > total)
        want = total;
    if (want && !a1)
        return -EFAULT;
    uint64_t pg = want ? pmm_alloc_page() : 0;
    uint8_t *sec = (uint8_t *)(uintptr_t)pg;
    long rc = (long)total;
    for (unsigned i = 0; i < want; i++) {
        struct blk_device *bd = blk_get(i);
        struct disk_info di;
        memset(&di, 0, sizeof di);
        di.index = i;
        if (bd) {
            if (bd->name) {
                size_t nl = strlen(bd->name);
                if (nl > sizeof di.name - 1)
                    nl = sizeof di.name - 1;
                memcpy(di.name, bd->name, nl);
            }
            di.sectors = bd->capacity_sectors;
            di.flags = disk_flags(i, bd, sec);
        }
        if (copyout((void __user *)(a1 + (long)(i * sizeof di)), &di, sizeof di) < 0) {
            rc = -EFAULT;
            break;
        }
    }
    if (pg)
        pmm_free_page(pg);
    return rc;
}

void sys_disk_register(void) {
    syscall_register(SYS_DISK_LIST, sys_disk_list);   /* NSI 104 */
}

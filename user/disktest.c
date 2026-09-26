/* user/disktest.c -- DDR-1143 sec.10.8: SYS_DISK_LIST (NSI 104) probe.
 *
 * Spawned on smoke-part AFTER part_selftest has registered its ramdisk, two
 * partitions and the trace wrapper, so one call sees every device class.
 *
 * The probe REPORTS and the gate JUDGES (DDR-1020): every value is printed and
 * nothing fail()s before a print. Each line is ONE write(2) (DDR-1056).
 *
 * WHY THE ARMS DISCRIMINATE. The gate computes the expected sector counts from
 * the HOST's own image sizes (stat of each build image), so a kernel that invented a
 * capacity cannot match; and it requires distinct flag tokens on distinct
 * device classes (phys / ramdisk / part / none), so a kernel returning one
 * constant flag word passes at most one line. A count-only arm is also made
 * two-sided: n=0 must report the SAME total as the full call and copy nothing.
 * Freestanding, no writable globals (user.ld). */

#define SYS_WRITE          6
#define SYS_EXIT           4
#define SYS_DISK_LIST    104

/* Must match kernel/install/disk_info.h (pinned by _Static_assert there). */
#define DISK_F_PHYS       0x01u
#define DISK_F_RAMDISK    0x02u
#define DISK_F_PART       0x04u
#define DISK_F_BLANK      0x08u
#define DISK_F_INSTALLED  0x10u
#define DISK_F_READERR    0x20u

struct disk_info {
    char          name[16];
    unsigned long sectors;
    unsigned      flags;
    unsigned      index;
};

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

struct line { char b[160]; long n; };
static void ls(struct line *l, const char *s) {
    while (*s && l->n < (long)sizeof l->b - 1) l->b[l->n++] = *s++;
}
static void ld(struct line *l, long v) {
    char t[24]; int k = 0;
    unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
    if (v < 0) ls(l, "-");
    do { t[k++] = (char)('0' + (u % 10)); u /= 10; } while (u && k < 23);
    while (k > 0 && l->n < (long)sizeof l->b - 1) l->b[l->n++] = t[--k];
}
static void lflush(struct line *l) {
    ls(l, "\n");
    nsi(SYS_WRITE, 1, (long)l->b, l->n);
    l->n = 0;
}
static void lflags(struct line *l, unsigned f) {
    int any = 0;
    static const struct { unsigned bit; const char *nm; } tk[] = {
        { DISK_F_PHYS, "phys" }, { DISK_F_RAMDISK, "ramdisk" },
        { DISK_F_PART, "part" }, { DISK_F_BLANK, "blank" },
        { DISK_F_INSTALLED, "installed" }, { DISK_F_READERR, "readerr" },
    };
    for (unsigned i = 0; i < sizeof tk / sizeof tk[0]; i++)
        if (f & tk[i].bit) { if (any) ls(l, ","); ls(l, tk[i].nm); any = 1; }
    if (f & ~0x3Fu) { if (any) ls(l, ","); ls(l, "unknown"); any = 1; }
    if (!any) ls(l, "none");
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    struct line l; l.n = 0;
    struct disk_info d[8];
    for (unsigned i = 0; i < 8; i++) {           /* poison: a skipped copy shows */
        for (unsigned k = 0; k < 16; k++) d[i].name[k] = '?';
        d[i].name[15] = 0;
        d[i].sectors = 0xDEADul; d[i].flags = 0x80000000u; d[i].index = 99;
    }
    long count = nsi(SYS_DISK_LIST, 0, 0, 0);
    long n = nsi(SYS_DISK_LIST, (long)d, 8, 0);
    long bad = nsi(SYS_DISK_LIST, 0x10, 8, 0);   /* not a user address */
    long show = (n > 8) ? 8 : n;
    for (long i = 0; i < show; i++) {
        ls(&l, "[disk] i="); ld(&l, (long)d[i].index);
        ls(&l, " name="); ls(&l, d[i].name);
        ls(&l, " sectors="); ld(&l, (long)d[i].sectors);
        ls(&l, " flags="); lflags(&l, d[i].flags);
        lflush(&l);
    }
    ls(&l, "[disk] n="); ld(&l, n); ls(&l, " count="); ld(&l, count);
    ls(&l, " efault="); ld(&l, bad); lflush(&l);
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) {}
}

/* user/rbtest.c — DDR-1171 rollback-on-rejection probe (smoke-rollback).
 *
 * Runs SOVEREIGN (SYS_SPAWN_TASK and SYS_REJECT_ACTION are sovereign-only).
 *   1. creates /RBOLD.TXT with 10 bytes -- the fixture the rollback task must
 *      NOT be able to damage;
 *   2. spawns R = "rb-work" WITH TASK_FLAG_ROLLBACK and C = "rb-ctl" without;
 *      each creates /RBN<pid>.TXT and parks a force-pending DELETE_FILE;
 *   3. reads each task's action id out of the KERNEL audit log (AR_SUBMIT) and
 *      rejects both;
 *   4. reads AR_ROLLBACK records back from the audit log, then checks the files.
 *
 * Every verdict comes from the kernel (an audit record, an open() rc, an fstat
 * size), never from what a task says about itself (DDR-1066). The probe REPORTS
 * one line; the gate JUDGES it (DDR-1020). C is the control that separates
 * "rejection undoes a rollback task's effects" from "rejection deletes files".
 *
 * Freestanding: raw syscalls, no writable globals (user.ld is one R+X segment).
 */

#define SYS_YIELD          3
#define SYS_EXIT           4
#define SYS_WRITE          6
#define SYS_OPEN           7
#define SYS_CLOSE          8
#define SYS_FSTAT          9
#define SYS_REJECT_ACTION 34
#define SYS_READ_AUDIT    37
#define SYS_CLOCK         57
#define SYS_SPAWN_TASK   107

#define AR_SUBMIT          1
#define AR_ROLLBACK       30     /* _Static_assert-pinned in kernel/aether/aether.h */
#define ACTION_DELETE_FILE 6
#define TASK_FLAG_ROLLBACK 1
#define O_CREAT         0x40

struct audit_entry {
    unsigned long timestamp;
    unsigned pid, action_type;
    unsigned long action_id;
    unsigned result, _pad;
};
struct audit_cursor { unsigned long from, first; };
struct task_budget { unsigned long max_ticks, max_bytes; unsigned max_actions, flags; };
/* Must be the kernel's FULL struct stat (kernel/include/stat.h, 144 B with
 * __unused[3]): fstat copies out sizeof the kernel struct. A 120-byte copy of
 * the layout overran this frame and zeroed a neighbouring local -- measured in
 * DDR-1171 §7 as a control file reading "kept" after the kernel deleted it. */
struct kstat { unsigned long dev, ino, nlink; unsigned mode, uid, gid, pad0;
               unsigned long rdev; long size, blksize, blocks, t[6], unused[3]; };
_Static_assert(sizeof(struct kstat) == 144, "kernel struct stat is 144 bytes");

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *putd(char *p, unsigned long v) {
    char t[24]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}
static char *putsd(char *p, long v) {
    if (v < 0) { *p++ = '-'; return putd(p, (unsigned long)-v); }
    return putd(p, (unsigned long)v);
}
static void rbn(char *p, long pid) {         /* "/RBN<pid>.TXT" */
    p = put(p, "/RBN"); p = putd(p, (unsigned long)pid); p = put(p, ".TXT"); *p = 0;
}
static long elapsed(long t0) {
    long el = nsi(SYS_CLOCK, 0, 0, 0) - t0;
    return el < 0 ? el + 86400 : el;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    long ofd = nsi(SYS_OPEN, (long)"/RBOLD.TXT", O_CREAT | 1, 0);
    if (ofd >= 0) {
        nsi(SYS_WRITE, ofd, (long)"0123456789", 10);
        nsi(SYS_CLOSE, ofd, 0, 0);
    }

    struct task_budget br = { 3000, 0, 0, TASK_FLAG_ROLLBACK };
    struct task_budget bc = { 1500, 0, 0, 0 };
    long pid[2], aid[2] = { 0, 0 }, rj[2] = { 1, 1 };
    unsigned long rb[2] = { 0, 0 };            /* 1 + (undone << 32 | failed) */
    pid[0] = nsi(SYS_SPAWN_TASK, (long)"rb-work", -1, (long)&br);
    pid[1] = nsi(SYS_SPAWN_TASK, (long)"rb-ctl",  -1, (long)&bc);

    struct audit_cursor cur = { 0, 0 };
    struct audit_entry buf[64];
    long t0 = nsi(SYS_CLOCK, 0, 0, 0), trj = -1;
    for (long it = 0; it < 4000000; it++) {
        long n = nsi(SYS_READ_AUDIT, (long)buf, 64, (long)&cur);
        for (long i = 0; i < n; i++)
            for (int k = 0; k < 2; k++) {
                if (pid[k] <= 0 || (long)buf[i].pid != pid[k]) continue;
                if (buf[i].result == AR_SUBMIT && buf[i].action_type == ACTION_DELETE_FILE
                    && !aid[k])
                    aid[k] = (long)buf[i].action_id;
                if (buf[i].result == AR_ROLLBACK && !rb[k])
                    rb[k] = 1 + buf[i].action_id;
            }
        if (n > 0) cur.from = cur.first + (unsigned long)n;
        if (trj < 0 && aid[0] && aid[1]) {
            rj[0] = nsi(SYS_REJECT_ACTION, aid[0], 0, 0);
            rj[1] = nsi(SYS_REJECT_ACTION, aid[1], 0, 0);
            trj = elapsed(t0);
        }
        long el = elapsed(t0);
        /* R's record, plus 3 s so the control has every chance to be (wrongly)
         * rolled back before it is judged. Bounded two ways (DDR-1068). */
        if (trj >= 0 && rb[0] && el >= trj + 3) break;
        if (el >= 40) break;
        nsi(SYS_YIELD, 0, 0, 0);
    }

    char path[32];
    rbn(path, pid[0]);
    long rnew = nsi(SYS_OPEN, (long)path, 0, 0);
    if (rnew >= 0) nsi(SYS_CLOSE, rnew, 0, 0);
    rbn(path, pid[1]);
    long cnew = nsi(SYS_OPEN, (long)path, 0, 0);
    if (cnew >= 0) nsi(SYS_CLOSE, cnew, 0, 0);
    long old = -1;
    long ofd2 = nsi(SYS_OPEN, (long)"/RBOLD.TXT", 0, 0);
    if (ofd2 >= 0) {
        struct kstat st;
        if (nsi(SYS_FSTAT, ofd2, (long)&st, 0) == 0) old = st.size;
        nsi(SYS_CLOSE, ofd2, 0, 0);
    }

    char line[256];
    char *p = put(line, "PRADYOS_ROLLBACK");
    if (pid[0] <= 0 || pid[1] <= 0) p = put(p, " spawnfail");
    p = put(p, " rj="); p = putsd(p, rj[0]); p = put(p, "/"); p = putsd(p, rj[1]);
    p = put(p, " R=");
    if (!rb[0]) p = put(p, "none");
    else { p = putd(p, (rb[0] - 1) >> 32); p = put(p, "/"); p = putd(p, (rb[0] - 1) & 0xFFFFFFFFul); }
    p = put(p, " rnew="); p = put(p, rnew >= 0 ? "kept" : "gone");
    p = put(p, " C=");
    if (!rb[1]) p = put(p, "none");
    else { p = putd(p, (rb[1] - 1) >> 32); p = put(p, "/"); p = putd(p, (rb[1] - 1) & 0xFFFFFFFFul); }
    p = put(p, " cnew="); p = put(p, cnew >= 0 ? "kept" : "gone");
    p = put(p, " old="); p = putsd(p, old);
    p = put(p, " end\n");
    nsi(SYS_WRITE, 1, (long)line, p - line);
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}

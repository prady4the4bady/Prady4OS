/* user/budgettest.c — DDR-1170 per-task budget probe (smoke-taskbudget).
 *
 * Runs SOVEREIGN (SYS_SPAWN_TASK is sovereign-only) and spawns four agent tasks:
 *   T  bud-time     max_ticks   = 50     -> must be killed, kind 1
 *   B  bud-bytes    max_bytes   = 4096   -> must be killed, kind 2, used > 4096
 *   A  bud-actions  max_actions = 3      -> must be killed, kind 3, used = 4
 *   C  bud-bytes    max_bytes   = 1 MiB  -> CONTROL: completes, no kill record
 *
 * Every verdict is read from the KERNEL's audit log (AR_BUDGET_KILL records
 * keyed by pid), never from the probe's own observation: a probe can fake what
 * it saw, it cannot fake a kernel-written record (DDR-1066). The probe REPORTS
 * one line and the gate JUDGES it (DDR-1020).
 *
 * Freestanding: raw syscalls, no writable globals (user.ld is one R+X segment).
 */

#define SYS_YIELD          3
#define SYS_WRITE          6
#define SYS_EXIT           4
#define SYS_READ_AUDIT    37
#define SYS_CLOCK         57
#define SYS_SPAWN_TASK   107

#define AR_BUDGET_KILL    29     /* _Static_assert-pinned in kernel/aether/aether.h */

struct audit_entry {
    unsigned long timestamp;
    unsigned pid, action_type;
    unsigned long action_id;
    unsigned result, _pad;
};
struct audit_cursor { unsigned long from, first; };
struct task_budget { unsigned long max_ticks, max_bytes; unsigned max_actions, _pad; };

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

/* Per-arm verdict: kind and used from the kill record, or 0/0 if none seen. */
struct verdict { long pid; unsigned long kind, used; };

static char *arm(char *p, const char *name, const struct verdict *v) {
    p = put(p, " "); p = put(p, name); p = put(p, "=");
    if (v->pid < 0) return put(p, "spawnfail");
    if (!v->kind)   return put(p, "none");
    p = putd(p, v->kind); p = put(p, "/");
    return putd(p, v->used);
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    struct task_budget bt = { 50, 0, 0, 0 };
    struct task_budget bb = { 0, 4096, 0, 0 };
    struct task_budget ba = { 0, 0, 3, 0 };
    struct task_budget bc = { 0, 1u << 20, 0, 0 };
    struct verdict v[4];
    v[0].pid = nsi(SYS_SPAWN_TASK, (long)"bud-time",    -1, (long)&bt);
    v[1].pid = nsi(SYS_SPAWN_TASK, (long)"bud-bytes",   -1, (long)&bb);
    v[2].pid = nsi(SYS_SPAWN_TASK, (long)"bud-actions", -1, (long)&ba);
    v[3].pid = nsi(SYS_SPAWN_TASK, (long)"bud-bytes",   -1, (long)&bc);
    for (int i = 0; i < 4; i++) { v[i].kind = 0; v[i].used = 0; }

    /* Poll the audit log from the oldest retained record. Bounded two ways:
     * a wall-clock window (SYS_CLOCK wraps at midnight, so it is also bounded
     * by an iteration count) -- DDR-1068's precedent. */
    struct audit_cursor cur = { 0, 0 };
    struct audit_entry buf[64];
    long t0 = nsi(SYS_CLOCK, 0, 0, 0);
    for (long it = 0; it < 2000000; it++) {
        long n = nsi(SYS_READ_AUDIT, (long)buf, 64, (long)&cur);
        for (long i = 0; i < n; i++) {
            if (buf[i].result != AR_BUDGET_KILL) continue;
            for (int k = 0; k < 4; k++)
                if (v[k].pid > 0 && (long)buf[i].pid == v[k].pid && !v[k].kind) {
                    v[k].kind = buf[i].action_id >> 56;
                    v[k].used = buf[i].action_id & 0x00FFFFFFFFFFFFFFul;
                }
        }
        if (n > 0) cur.from = cur.first + (unsigned long)n;
        long el = nsi(SYS_CLOCK, 0, 0, 0) - t0;
        if (el < 0) el += 86400;
        /* The three must-kill arms done, plus 3 s more so the control has had
         * every chance to be (wrongly) killed before it is judged. */
        if (v[0].kind && v[1].kind && v[2].kind && el >= 3) break;
        if (el >= 30) break;
        nsi(SYS_YIELD, 0, 0, 0);
    }

    char line[256];
    char *p = put(line, "PRADYOS_TASKBUDGET");
    p = arm(p, "T", &v[0]);
    p = arm(p, "B", &v[1]);
    p = arm(p, "A", &v[2]);
    p = arm(p, "C", &v[3]);
    p = put(p, " cpid="); p = putsd(p, v[3].pid);
    p = put(p, "\n");
    nsi(SYS_WRITE, 1, (long)line, p - line);
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}

/* kernel/aether/aether_mem.c — AETHER per-process memory cap + syscall rate limit.
 *
 * Both enforce a hard bound on an agent and, when crossed, signal the *caller* to
 * cleanly kill the offending process (zombie + status, reaped by its parent) —
 * never a kernel panic. A kernel-thread/init/PRISM process (is_agent==0) is
 * exempt so existing gates are unaffected (ADR-026 D5/D7).
 *
 * The kill DECISION (log + audit + return <0) is separated from the kill ACTION
 * (sched_exit, done by the caller) so the policy is unit-testable in-kernel
 * without terminating the running thread (see aether_sectest).
 */
#include "aether.h"
#include "sched.h"
#include "mm.h"           /* DDR-1181 */
#include "console.h"
#include "irq.h"      /* g_ticks */
#include "pmm.h"      /* DDR-1171: rollback log page */
#include "fd.h"
#include "vfs.h"
#include "string.h"
#include "errno.h"

static void log_pid(const char *tag, uint32_t pid) {
    kputs(tag);
    kputdec(pid);
    kputs("\r\n");
}

static uint64_t cap_of(const struct tcb *t) {
    return t->mm->mem_limit ? t->mm->mem_limit : AETHER_MEM_DEFAULT;
}

/* Returns 0 (allowed, charged) or -1 (over cap: logged + audited; caller kills). */
int aether_mem_charge(struct tcb *t, uint64_t bytes) {
    if (!t->is_agent)
        return 0;                               /* non-agents are uncapped */
    if (t->mm->mem_used + bytes > cap_of(t)) {
        aether_drop_pid(t->pid);
        aether_audit(t->pid, 0, 0, AR_OOM_KILL);
        log_pid("AGENT_OOM_KILLED PID=", t->pid);
        return -1;
    }
    t->mm->mem_used += bytes;
    return 0;
}

void aether_mem_uncharge(struct tcb *t, uint64_t bytes) {
    if (!t->is_agent)
        return;
    t->mm->mem_used = (t->mm->mem_used > bytes) ? t->mm->mem_used - bytes : 0;
}

/* SYS_SET_MEM_LIMIT: a process may only *lower* its own cap (no self-escalation).
 * Raising, or a zero limit, is rejected (-EPERM at the syscall boundary). */
long aether_set_mem_limit(uint32_t pid, uint64_t bytes) {
    (void)pid;
    struct tcb *t = current_thread;
    /* DDR-1181: the cap lives in the mm, and a kernel thread has none (the
     * boot thread runs aether_sectest's escalation arm). Reading t->mm->
     * there dereferenced NULL through the low identity map and returned
     * whatever was at that offset, so the escalation was ACCEPTED
     * (smoke-aether-sec lost AETHER_SEC_CAP_DENIED, run 39502463241). With
     * no address space there is no cap to change. */
    if (!t->mm)
        return -1;
    /* DDR-1181 §4.4 R3: the cap is shared by every thread of the mm; check
     * and store as one step, or two lowering siblings can raise it. */
    mm_lock(t);
    uint64_t cur = cap_of(t);
    if (bytes == 0 || bytes > cur) {
        mm_unlock(t);
        return -1;
    }
    t->mm->mem_limit = bytes;
    mm_unlock(t);
    return 0;
}

/* Sliding 1 s window, AETHER_RATE_MAX syscalls. Returns 0 (proceed) or -1 (budget
 * blown: logged + audited; caller kills). */
int aether_rate_check(struct tcb *t) {
    if (!t->is_agent)
        return 0;
    if (g_ticks - t->sc_window_start >= AETHER_RATE_WINDOW) {
        t->sc_window_start = g_ticks;
        t->sc_count = 0;
    }
    if (++t->sc_count > AETHER_RATE_MAX) {
        aether_drop_pid(t->pid);
        aether_audit(t->pid, 0, 0, AR_RATE_KILL);
        log_pid("AGENT_RATE_LIMITED PID=", t->pid);
        return -1;
    }
    return 0;
}

/* --- DDR-1170: per-task budgets ------------------------------------------- *
 * A budget runs OUT, unlike the rate window above. Exhaustion kills the task:
 * a budget that only refused would leave the agent free to ignore the error
 * and retry, i.e. a rate limiter again (DDR-1170 §4). The kill is audited
 * exactly once, with the axis and the amount charged in action_id. */
void aether_budget_arm(struct tcb *t, const struct task_budget *b) {
    t->bud_max_ticks   = b->max_ticks;
    t->bud_max_bytes   = b->max_bytes;
    t->bud_max_actions = b->max_actions;
    t->bud_start   = g_ticks;
    t->bud_bytes   = 0;
    t->bud_actions = 0;
    t->bud_tripped = 0;
}

static void budget_trip(struct tcb *t, unsigned kind, uint64_t used) {
    if (t->bud_tripped)
        return;
    t->bud_tripped = 1;
    aether_drop_pid(t->pid);
    aether_audit(t->pid, 0, ((uint64_t)kind << 56) | (used & 0x00FFFFFFFFFFFFFFull),
                 AR_BUDGET_KILL);
    log_pid("AGENT_BUDGET_KILLED PID=", t->pid);
}

int aether_budget_charge(struct tcb *t, unsigned kind, uint64_t amount) {
    if (t->bud_tripped)
        return -1;
    if (kind == TASK_BUDGET_BYTES && t->bud_max_bytes) {
        t->bud_bytes += amount;
        if (t->bud_bytes > t->bud_max_bytes) {
            budget_trip(t, kind, t->bud_bytes);
            return -1;
        }
    } else if (kind == TASK_BUDGET_ACTIONS && t->bud_max_actions) {
        t->bud_actions += (uint32_t)amount;
        if (t->bud_actions > t->bud_max_actions) {
            budget_trip(t, kind, t->bud_actions);
            return -1;
        }
    }
    return 0;
}

int aether_budget_time(struct tcb *t) {
    if (t->bud_tripped)
        return -1;
    if (t->bud_max_ticks && g_ticks - t->bud_start > t->bud_max_ticks) {
        budget_trip(t, TASK_BUDGET_TIME, g_ticks - t->bud_start);
        return -1;
    }
    return 0;
}

/* --- DDR-1171: rollback on rejection --------------------------------------- *
 * One PMM page of RB_MAX entries. Low physical memory is identity-mapped, so
 * the page is addressed directly (sys_io.c's staging buffer does the same).
 * Only CREATES are logged: every other effect a rollback task could have on a
 * file is refused at its syscall, so creates are the whole of what can need
 * undoing (DDR-1171 §2). */
struct rb_entry { char path[RB_PATH]; uint64_t _r; };
_Static_assert(sizeof(struct rb_entry) * RB_MAX <= 4096, "rollback log fits one page");

void *aether_rb_page(void) {
    uint64_t pg = pmm_alloc_page();
    if (!pg)
        return 0;
    memset((void *)(uintptr_t)pg, 0, 4096);
    return (void *)(uintptr_t)pg;
}

void aether_rb_free(struct tcb *t) {
    pmm_free_page((uint64_t)(uintptr_t)t->rb_log);
    t->rb_log = 0;
    t->rb_n = 0;
}

static unsigned rb_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

int aether_rb_room(struct tcb *t, const char *path) {
    if (rb_len(path) >= RB_PATH)
        return -ENAMETOOLONG;
    if (t->rb_n >= RB_MAX)
        return -ENOSPC;              /* an untracked create is worse than a refused one */
    return 0;
}

void aether_rb_created(struct tcb *t, const char *path) {
    struct rb_entry *e = (struct rb_entry *)t->rb_log + t->rb_n;
    unsigned n = rb_len(path);
    memcpy(e->path, path, n + 1);    /* aether_rb_room bounded n < RB_PATH */
    t->rb_n++;
}

int aether_rb_is_created(struct tcb *t, const char *path) {
    const struct rb_entry *e = (const struct rb_entry *)t->rb_log;
    for (uint32_t i = 0; i < t->rb_n; i++)
        if (strcmp(e[i].path, path) == 0)
            return 1;
    return 0;
}

void aether_rb_replay(struct tcb *t) {
    /* Close the task's files first so no open handle outlives its unlink. */
    for (int i = 0; i < FD_MAX; i++)
        if (fd_get(t, i))
            fd_free(t, i);
    const struct rb_entry *e = (const struct rb_entry *)t->rb_log;
    uint64_t undone = 0, failed = 0;
    for (uint32_t i = t->rb_n; i-- > 0; ) {      /* reverse order */
        if (vfs_unlink(t->fs_cap, t->root_mnt, e[i].path) == 0)
            undone++;
        else
            failed++;
    }
    t->rb_n = 0;
    t->rb_pending = 0;
    aether_audit(t->pid, 0, (undone << 32) | (failed & 0xFFFFFFFFull), AR_ROLLBACK);
    kputs("AGENT_ROLLED_BACK PID="); kputdec(t->pid);
    kputs(" undone="); kputdec(undone); kputs(" failed="); kputdec(failed); kputs("\r\n");
}

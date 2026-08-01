/* kernel/syscall/sys_aether.c — AETHER NSI handlers (ADR-026 / DDR §1, §4).
 *
 * The 10 agent-layer syscalls (29..38). Every user pointer crosses copyin/copyout
 * (ADR-022); authority is the kernel-set per-process flag (is_agent / is_sovereign),
 * never a user-forgeable token. A denied authority returns -EPERM and audits the
 * attempt but lets the caller survive (only resource abuse is fatal).
 */
#include "syscall.h"
#include "metric_page.h"   /* DDR-812: lockbox read */
#include "sched.h"
#include "uaccess.h"
#include "errno.h"
#include "console.h"
#include "aether.h"

#define SIGKILL 9

/* Named-agent roster (DDR-707/730): one UI slot per named agent (KRYOS..SOLIN).
 * `used` + `pid` are recorded at spawn; `actions` counts what that agent has
 * submitted. Liveness is NOT a stored bit — a slot is "active" iff its pid still
 * resolves to a live agent tcb (see roster_active), so a card self-corrects to
 * dim when its agent dies (root-fixes the DDR-707 never-cleared-bit leak). The
 * names live in userspace; the kernel tracks slots by index. */
#define AGENT_ROSTER_N 8
struct agent_slot {
    uint8_t used; uint32_t pid; uint64_t actions;
    /* DDR-735: last-known CPU accounting, refreshed from the live tcb on every
     * metrics read and RETAINED after the agent exits — so a short-lived agent's
     * dispatch/run counts stay observable (its tcb is gone once it dies). */
    uint64_t run_ticks, dispatches;
};
static struct agent_slot g_agent[AGENT_ROSTER_N];

/* The shape SYS_AGENT_METRICS returns (mirrored in user/agentmetricstest.c).
 * DDR-735 appends run_ticks (100 Hz ticks while current — sampled CPU time) and
 * dispatches (scheduler switch-ins; >= 1 for any thread that ever ran). */
struct agent_metric { uint32_t pid, state; uint64_t mem_used, actions, run_ticks, dispatches; };

/* Walk the ready ring for a live user thread with this pid. */
static struct tcb *tcb_by_pid(uint32_t pid) {
    struct tcb *t = current_thread;
    do {
        if (t->is_user && t->pid == pid &&
            t->state != THREAD_ZOMBIE && t->state != THREAD_DONE)
            return t;
        t = t->next;
    } while (t != current_thread);
    return 0;
}

/* DDR-730: a slot is active iff it was spawned AND its agent is still alive. */
static int roster_active(int i) {
    return g_agent[i].used && tcb_by_pid(g_agent[i].pid) != 0;
}

/* DDR-730: the roster slot a submitting agent occupies, or -1. */
static int slot_of_pid(uint32_t pid) {
    for (int i = 0; i < AGENT_ROSTER_N; i++)
        if (g_agent[i].used && g_agent[i].pid == pid)
            return i;
    return -1;
}

/* The kernel-side agent spawner is provided by kmain (it closes over the SFS
 * mount + cap + embedded agent ELF). Returns the new pid, or <0. */
typedef long (*aether_spawn_fn)(const char *task);
static aether_spawn_fn g_spawn_hook;
void aether_set_spawn_hook(aether_spawn_fn fn) { g_spawn_hook = fn; }

static long sys_get_mode(long a1, long a2, long a3, long a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    return (long)aether_get_mode();
}

/* DDR-812 — read the metric lockbox.
 *
 * Sovereign-gated: the lockbox is the owner's ground truth, so a non-sovereign
 * reader gets -EPERM and an audit record, matching every other authority check
 * in this file.
 *
 * The verification happens in metric_lockbox_read() BEFORE any bytes are copied,
 * so a tampered record returns -ETAMPER and nothing else. Returning the data
 * "but flagged" would let a careless caller use it, which defeats the point of
 * having a hash at all. */
static long sys_metric_read(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    if (!current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    metric_lockbox_t rec;
    long r = metric_lockbox_read(&rec);
    if (r < 0)
        return r;                       /* -ENOENT (never committed) | -ETAMPER */
    if (copyout((void __user *)a1, &rec, sizeof rec) < 0)
        return -EFAULT;
    return 0;
}

static long sys_set_mode(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    if (!current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;                          /* no self-escalation (D6) */
    }
    return aether_set_mode((unsigned)a1);
}

static long sys_submit_action(long a1, long a2, long a3, long a4) {
    (void)a4;
    if (!current_thread->is_agent) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    uint32_t type = (uint32_t)a1;
    uint32_t len  = (uint32_t)a3;
    if (len > AETHER_PAYLOAD_MAX) len = AETHER_PAYLOAD_MAX;
    uint8_t kbuf[AETHER_PAYLOAD_MAX];
    if (len && copyin(kbuf, (const void __user *)a2, len) < 0)
        return -EFAULT;
    long r = aether_submit(current_thread->pid, type, kbuf, len);
    if (r >= 0) {                                   /* DDR-730: live action count */
        int slot = slot_of_pid(current_thread->pid);
        if (slot >= 0) g_agent[slot].actions++;
    }
    return r;
}

static long sys_poll_result(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    return aether_poll(current_thread->pid, (uint64_t)a1);
}

static long sys_approve_action(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    if (!current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    return aether_approve((uint64_t)a1, 1);
}

static long sys_reject_action(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    if (!current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    return aether_approve((uint64_t)a1, 0);
}

static long sys_spawn_agent(long a1, long a2, long a3, long a4) {
    (void)a1; (void)a4;
    /* The daemon (sovereign) or an existing agent may spawn agents (D6/D8). */
    if (!current_thread->is_sovereign && !current_thread->is_agent) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    if (!g_spawn_hook)
        return -ENOSYS;
    char task[64];
    if (a2 && copyinstr(task, (const void __user *)a2, sizeof task, 0) < 0)
        return -EFAULT;
    if (!a2) task[0] = 0;
    long pid = g_spawn_hook(task);
    if (pid >= 0 && a3 >= 0 && a3 < AGENT_ROSTER_N) {  /* DDR-707/730: claim the slot */
        g_agent[a3].used = 1;
        g_agent[a3].pid = (uint32_t)pid;
        g_agent[a3].actions = 0;
        g_agent[a3].run_ticks = 0;      /* DDR-735: reset accounting for the new agent */
        g_agent[a3].dispatches = 0;
    }
    return pid;
}

static long sys_agent_roster(long a1, long a2, long a3, long a4) {
    (void)a3; (void)a4;
    int max = (int)a2;
    if (max <= 0) return 0;
    if (max > AGENT_ROSTER_N) max = AGENT_ROSTER_N;
    uint8_t bits[AGENT_ROSTER_N];                    /* DDR-730: derived, self-correcting */
    for (int i = 0; i < max; i++) bits[i] = (uint8_t)roster_active(i);
    if (copyout((void __user *)a1, bits, (size_t)max) < 0)
        return -EFAULT;
    return max;
}

/* DDR-730: per-agent live metrics. For each slot, if its agent is alive, report
 * its pid + scheduler state + charged memory + cumulative action count; inactive
 * slots report all-zero. Read-only observability, no authority gate. */
static long sys_agent_metrics(long a1, long a2, long a3, long a4) {
    (void)a3; (void)a4;
    int max = (int)a2;
    if (max <= 0) return 0;
    if (max > AGENT_ROSTER_N) max = AGENT_ROSTER_N;
    struct agent_metric buf[AGENT_ROSTER_N];
    for (int i = 0; i < max; i++) {
        struct tcb *t = g_agent[i].used ? tcb_by_pid(g_agent[i].pid) : 0;
        if (t) {
            g_agent[i].run_ticks  = t->run_ticks;    /* DDR-735: refresh + RETAIN so the */
            g_agent[i].dispatches = t->dispatches;   /* counts survive the agent's exit  */
            buf[i].pid        = g_agent[i].pid;
            buf[i].state      = (t->state == THREAD_BLOCKED) ? 2u : 1u;   /* 1=run/ready 2=blocked */
            buf[i].mem_used   = t->mem_used;
            buf[i].actions    = g_agent[i].actions;
        } else {
            /* dead (or never-spawned) slot: state 0, but a spawned slot's IDENTITY
             * and CPU counts stay readable POST-MORTEM (pid + counts retained; the
             * counts are captured authoritatively at exit by agent_metrics_reap, so
             * they never depend on a read having landed during the agent's life —
             * on slow TCG hosts an agent's whole life can fit between two reads). */
            buf[i].state = 0;
            buf[i].pid = g_agent[i].used ? g_agent[i].pid : 0;
            buf[i].mem_used = 0;
            buf[i].actions  = g_agent[i].used ? g_agent[i].actions : 0;
        }
        buf[i].run_ticks  = g_agent[i].run_ticks;
        buf[i].dispatches = g_agent[i].dispatches;
    }
    if (copyout((void __user *)a1, buf, (size_t)max * sizeof buf[0]) < 0)
        return -EFAULT;
    return max;
}

/* DDR-735 (exit capture): called from sched_exit with the dying thread's final
 * counters. Makes the roster's retained CPU accounting authoritative — with
 * refresh-on-read alone, an agent whose whole life fits between two metrics
 * reads (seconds-long quanta on TCG CI runners) would retain zeros forever. */
void agent_metrics_reap(uint32_t pid, uint64_t run_ticks, uint64_t dispatches) {
    int slot = slot_of_pid(pid);
    if (slot >= 0) {
        g_agent[slot].run_ticks  = run_ticks;
        g_agent[slot].dispatches = dispatches;
    }
}

static long sys_kill_agent(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    if (!current_thread->is_sovereign && !current_thread->is_agent)
        return -EPERM;
    struct tcb *t = tcb_by_pid((uint32_t)a1);
    if (!t)
        return -ESRCH;
    t->sig_pending |= (1ull << SIGKILL);        /* terminated on its next IRQ return */
    return 0;
}

static long sys_read_audit(long a1, long a2, long a3, long a4) {
    (void)a3; (void)a4;
    int max = (int)a2;
    if (max <= 0) return 0;
    if (max > 64) max = 64;                      /* bounded kernel staging buffer */
    struct aether_audit_entry_pub buf[64];
    int n = aether_audit_read(buf, max);
    if (n > 0 && copyout((void __user *)a1, buf,
                         (size_t)n * sizeof buf[0]) < 0)
        return -EFAULT;
    return n;
}

static long sys_set_mem_limit(long a1, long a2, long a3, long a4) {
    (void)a3; (void)a4;
    long r = aether_set_mem_limit((uint32_t)a1, (uint64_t)a2);
    return r < 0 ? -EPERM : 0;                   /* lower-only; no self-escalation */
}

void sys_aether_register(void) {
    syscall_register(SYS_GET_MODE,       sys_get_mode);
    syscall_register(SYS_SET_MODE,       sys_set_mode);
    syscall_register(SYS_METRIC_READ,    sys_metric_read);  /* DDR-812 */
    syscall_register(SYS_SUBMIT_ACTION,  sys_submit_action);
    syscall_register(SYS_POLL_RESULT,    sys_poll_result);
    syscall_register(SYS_APPROVE_ACTION, sys_approve_action);
    syscall_register(SYS_REJECT_ACTION,  sys_reject_action);
    syscall_register(SYS_SPAWN_AGENT,    sys_spawn_agent);
    syscall_register(SYS_KILL_AGENT,     sys_kill_agent);
    syscall_register(SYS_READ_AUDIT,     sys_read_audit);
    syscall_register(SYS_SET_MEM_LIMIT,  sys_set_mem_limit);
    syscall_register(SYS_AGENT_ROSTER,   sys_agent_roster);   /* DDR-707 */
    syscall_register(SYS_AGENT_METRICS,  sys_agent_metrics);  /* DDR-730 */
}

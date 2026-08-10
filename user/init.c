/* user/init.c — pradyos-init, PID 1 (Layer 5 slice 5d).
 *
 * The first long-lived ring-3 process: it prints a liveness banner, then becomes
 * the system reaper — collecting any child that exits so exited processes never
 * leak their address space + TCB (the teardown leak noted in SESSION_HANDOFF §2;
 * the kernel reparents orphans to init on exit, so init reaps the whole tree).
 *
 * Output uses musl printf (readability for a reference PID 1). The process-control
 * calls go straight to the PRADYOS NSI by number: musl's fork/waitpid/sched_yield
 * wrappers pull in cancellation-point + clone plumbing this minimal libc does not
 * vendor, and issuing the syscalls directly keeps the kernel contract explicit.
 * stdout is fully buffered on the (non-tty) console and init never exits, so every
 * line is fflush'd or it would sit in the buffer forever. */
#include <stdio.h>

/* NSI numbers — keep in sync with kernel/syscall/syscall.h. */
#define SYS_EXIT   4
#define SYS_YIELD  3
#define SYS_FORK   15
#define SYS_WAIT4  16
#define SYS_EXECVE 14   /* DDR-891: services are fork+execve */
#define WNOHANG    1

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* ---- DDR-891 (item 15): the service table and its supervision ------------
 *
 * Bounded and static. A dynamic table in PID 1 would mean PID 1 allocating, and
 * an allocation failure in the process that supervises everything else has no
 * good recovery.
 */
#define SVC_MAX          4
#define SVC_RESTART_MAX  3          /* budget; see the giveup path below */

#define RESTART_NEVER       0
#define RESTART_ON_FAILURE  1

/* Capabilities a service may request. init holds none of the privileged ones, so
 * anything but CAP_NONE is refused before the fork. init never grants; it only
 * refuses. */
#define CAP_NONE   0u
#define CAP_AGENT  (1u << 4)
#define INIT_CAPS  CAP_NONE

struct service {
    const char *name;
    const char *path;
    int         policy;
    unsigned    caps_required;
    long        pid;
    int         restarts;
    int         gave_up;
};

static struct service g_svc[SVC_MAX] = {
    /* Exists and exits cleanly — proves start and exit attribution. */
    { "exectest", "/EXECTEST.ELF", RESTART_NEVER,      CAP_NONE,  0, 0, 0 },
    /* Binary absent: execve fails, the child exits 127, and the restart budget
     * is what stops PID 1 spinning on it forever. */
    { "missing",  "/NOSUCH.ELF",   RESTART_ON_FAILURE, CAP_NONE,  0, 0, 0 },
    /* Demands a capability init does not hold — refused BEFORE the fork. */
    { "agentsvc", "/AGENT.ELF",    RESTART_NEVER,      CAP_AGENT, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 },
};

static struct service *svc_by_pid(long pid) {
    for (int i = 0; i < SVC_MAX; i++)
        if (g_svc[i].name && g_svc[i].pid == pid)
            return &g_svc[i];
    return 0;
}

static void svc_spawn(struct service *s) {
    /* Capability check BEFORE the fork. Leaving it to the kernel would let the
     * process start and fail later at first privileged use — by which time a
     * process is running that should never have existed, and the failure shows
     * up somewhere unrelated to the misconfigured service. */
    if (s->caps_required & ~(unsigned)INIT_CAPS) {
        printf("[svc] refuse %s: requires caps init does not hold\n", s->name);
        fflush(stdout);
        s->gave_up = 1;
        return;
    }
    long kid = nsi(SYS_FORK, 0, 0, 0);
    if (kid == 0) {
        nsi(SYS_EXECVE, (long)s->path, 0, 0);
        nsi(SYS_EXIT, 127, 0, 0);          /* execve failed: 127, as a shell does */
    }
    if (kid < 0) {
        printf("[svc] fork failed for %s\n", s->name);
        fflush(stdout);
        return;
    }
    s->pid = kid;
    printf("[svc] start %s pid=%ld\n", s->name, kid);
    fflush(stdout);
}

static void svc_start_all(void) {
    for (int i = 0; i < SVC_MAX; i++)
        if (g_svc[i].name)
            svc_spawn(&g_svc[i]);
}

static void svc_handle_exit(struct service *s, int st) {
    if (s->policy != RESTART_ON_FAILURE || st == 0)
        return;                            /* clean exit, or not ours to restart */
    if (s->restarts >= SVC_RESTART_MAX) {
        /* Give up LOUDLY. A supervisor that silently stops retrying is
         * indistinguishable from one that never noticed; one that retries
         * forever turns a broken config into a system-wide livelock, because a
         * missing binary fails execve instantly and PID 1 then spins at 100%. */
        if (!s->gave_up) {
            printf("[svc] giveup %s after %d restarts\n", s->name, s->restarts);
            fflush(stdout);
            s->gave_up = 1;
        }
        return;
    }
    s->restarts++;
    printf("[svc] restart %s %d/%d\n", s->name, s->restarts, SVC_RESTART_MAX);
    fflush(stdout);
    svc_spawn(s);
}

int main(void) {
    printf("PRADYOS_INIT_OK v1.0.0 2026-06-27\n");
    fflush(stdout);

    /* Startup self-check (5d): fork a child that exits 42 and reap it, proving the
     * reap path end to end. The child runs no libc — just _exit(42). */
    long dummy = nsi(SYS_FORK, 0, 0, 0);
    if (dummy == 0)
        nsi(SYS_EXIT, 42, 0, 0);          /* child: terminate; never returns */
    if (dummy > 0) {
        int st = 0;
        long r = nsi(SYS_WAIT4, dummy, (long)&st, 0);   /* block until it exits */
        if (r > 0) {
            printf("init: reaped PID=%ld exit=%d\n", r, st);
            fflush(stdout);
        }
    }

    /* PID 1 reap loop: the kernel launches the PRISM shell as init's child
     * (ADR-024 §D5); init collects it (and any orphan reparented here) and never
     * exits. Poll with yield so we never busy-spin. */
    /* ---- DDR-891 (item 15): start the supervised services ------------------
     * The loop below was a REAPER: it collected children and printed them. It
     * did not start anything, did not know what a service was, and did not react
     * to one dying. */
    svc_start_all();

    for (;;) {
        int st = 0;
        long r = nsi(SYS_WAIT4, -1, (long)&st, WNOHANG);
        if (r > 0) {
            struct service *s = svc_by_pid(r);
            if (s) {
                printf("[svc] exit %s pid=%ld st=%d\n", s->name, r, st);
                fflush(stdout);
                s->pid = 0;
                svc_handle_exit(s, st);
            } else {
                /* Still the reaper for orphans reparented here — a pid matching
                 * no service is collected exactly as before. */
                printf("init: reaped PID=%ld exit=%d\n", r, st);
                fflush(stdout);
            }
        } else {
            nsi(SYS_YIELD, 0, 0, 0);
        }
    }
    return 0;                              /* unreachable; kernel panics if reached */
}

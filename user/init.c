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
#define WNOHANG    1

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
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
    for (;;) {
        int st = 0;
        long r = nsi(SYS_WAIT4, -1, (long)&st, WNOHANG);
        if (r > 0) {
            printf("init: reaped PID=%ld exit=%d\n", r, st);
            fflush(stdout);
        } else {
            nsi(SYS_YIELD, 0, 0, 0);
        }
    }
    return 0;                              /* unreachable; kernel panics if reached */
}

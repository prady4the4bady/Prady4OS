/* user/aether_daemon.c — AETHER daemon, ring 3 (Layer 6, ADR-026 §D11).
 *
 * Spawned by the kernel as init's child and granted CAP_SOVEREIGN. It is the
 * operator's proxy: it owns the global mode and the approve/reject authority, and
 * it spawns agents. In this reference (test-mode) build it auto-spawns one agent
 * to exercise the full pipeline, then becomes a reaper for its children.
 *
 * The NIA IPC command surface (AETHER_SPAWN/STATUS/KILL/MODE) maps 1:1 onto the
 * NSI calls below; a richer console front-end is a later refinement. SFS config
 * (/etc/aether/config) reading is deferred — the reference build defaults to test
 * mode (see docs/build_status.md).
 */
#include <stdio.h>

/* NSI numbers — keep in sync with kernel/syscall/syscall.h. */
#define SYS_EXIT        4
#define SYS_YIELD       3
#define SYS_WAIT4       16
#define SYS_GET_MODE    29
#define SYS_SET_MODE    30
#define SYS_SPAWN_AGENT 35
#define WNOHANG         1

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

int main(void) {
    long mode = nsi(SYS_GET_MODE, 0, 0, 0);
    printf("PRADYOS_AETHER_DAEMON_OK mode=%s\n", mode ? "sovereign" : "manual");
    fflush(stdout);

    /* L7 (DDR-701): Sovereign/Manual toggle binding self-check. As the
     * CAP_SOVEREIGN authority, exercise SYS_SET_MODE both ways and confirm via
     * SYS_GET_MODE, then restore SOVEREIGN so the agent pipeline auto-approves.
     * This proves the toggle's control path end-to-end (the future graphical
     * Super+M toggle is a renderer over exactly this binding). */
    {
        long ok = 1;
        printf("PRADYOS_MODE_%s\n", nsi(SYS_GET_MODE, 0, 0, 0) ? "SOVEREIGN" : "MANUAL");
        ok &= (nsi(SYS_SET_MODE, 0, 0, 0) == 0);                 /* -> MANUAL */
        ok &= (nsi(SYS_GET_MODE, 0, 0, 0) == 0);
        printf("PRADYOS_MODE_%s\n", nsi(SYS_GET_MODE, 0, 0, 0) ? "SOVEREIGN" : "MANUAL");
        ok &= (nsi(SYS_SET_MODE, 1, 0, 0) == 0);                 /* -> SOVEREIGN */
        ok &= (nsi(SYS_GET_MODE, 0, 0, 0) == 1);
        printf("PRADYOS_MODE_%s\n", nsi(SYS_GET_MODE, 0, 0, 0) ? "SOVEREIGN" : "MANUAL");
        printf("PRADYOS_MODE_TOGGLE_%s\n", ok ? "OK" : "FAIL");
        fflush(stdout);
    }

    /* Test-mode bring-up: spawn one agent with task "test". The kernel loads the
     * embedded agent image and marks it CAP_AGENT; sovereign mode auto-approves
     * its action, so the agent completes and prints PRADYOS_AGENT_DONE. */
    /* Spawn the test agent into roster slot 0 (KRYOS) so the agent panel lights
     * it (DDR-707): SYS_SPAWN_AGENT(path, task, slot). */
    long apid = nsi(SYS_SPAWN_AGENT, 0, (long)"test", 0 /* slot 0 = KRYOS */);
    if (apid > 0) {
        printf("aetherd: spawned agent PID=%ld\n", apid);
        fflush(stdout);
    } else {
        printf("aetherd: spawn_agent failed rc=%ld\n", apid);
        fflush(stdout);
    }

    /* Reaper loop: collect the agent (and any other child) so nothing leaks; the
     * daemon never exits. Yield instead of busy-spinning. */
    for (;;) {
        int st = 0;
        long r = nsi(SYS_WAIT4, -1, (long)&st, WNOHANG);
        if (r > 0) {
            printf("aetherd: reaped PID=%ld exit=%d\n", r, st);
            fflush(stdout);
        } else {
            nsi(SYS_YIELD, 0, 0, 0);
        }
    }
    return 0;
}

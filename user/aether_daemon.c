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
#define SYS_READ        5
#define SYS_OPEN        7
#define SYS_CLOSE       8
#define SYS_WAIT4       16
#define SYS_GET_MODE    29
#define SYS_SET_MODE    30
#define SYS_SPAWN_AGENT 35
#define SYS_NET_ALLOW   65
#define WNOHANG         1

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* DDR-732 — boot policy from /AETHER.CFG on the boot volume (key=value lines:
 * mode=sovereign|manual, task=<str>, slot=<0..7>; unknown keys ignored). Filled
 * with the compiled defaults first, so a missing/garbled file changes nothing —
 * the daemon must never fail to boot over config. Returns 1 iff the file was
 * read and at least one key parsed (the CFG_OK/CFG_DEFAULT distinction). */
struct aether_cfg {
    int mode; long slot; char task[64];
    /* DDR-734: net=<a.b.c.d>:<port> egress rules (up to 8, matching the kernel
     * list bound); installed via SYS_NET_ALLOW before any agent spawns. */
    int nnet;
    unsigned net_host_be[8];
    unsigned net_port[8];
};

/* Parse "a.b.c.d:port" -> big-endian host + port; 1 on success. */
static int parse_hostport(const char *s, unsigned *host_be, unsigned *port) {
    unsigned oct[4] = {0, 0, 0, 0};
    int i = 0;
    for (int o = 0; o < 4; o++) {
        int d = 0;
        while (s[i] >= '0' && s[i] <= '9') { oct[o] = oct[o] * 10 + (unsigned)(s[i] - '0'); i++; d++; }
        if (!d || oct[o] > 255) return 0;
        if (o < 3) { if (s[i] != '.') return 0; i++; }
    }
    if (s[i] != ':') return 0;
    i++;
    unsigned p = 0; int d = 0;
    while (s[i] >= '0' && s[i] <= '9') { p = p * 10 + (unsigned)(s[i] - '0'); i++; d++; }
    if (!d || s[i] || p > 65535) return 0;
    *host_be = (oct[0] << 24) | (oct[1] << 16) | (oct[2] << 8) | oct[3];
    *port = p;
    return 1;
}

static int cfg_load(struct aether_cfg *c) {
    c->mode = 1;                              /* defaults: sovereign/test/0 */
    c->slot = 0;
    c->task[0] = 't'; c->task[1] = 'e'; c->task[2] = 's'; c->task[3] = 't'; c->task[4] = 0;
    c->nnet = 0;                              /* DDR-734: no rules = deny-all */
    long fd = nsi(SYS_OPEN, (long)"/etc/aether/config", 0, 0);  /* DDR-761: SFS root */
    if (fd < 0)
        return 0;
    char buf[257];
    long n = nsi(SYS_READ, fd, (long)buf, 256);
    nsi(SYS_CLOSE, fd, 0, 0);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    int parsed = 0;
    for (long i = 0; i < n; ) {
        long e = i;                            /* line = [i, e) */
        while (e < n && buf[e] != '\n') e++;
        buf[e] = 0;
        const char *ln = &buf[i];
        if (ln[0] == 'm' && ln[1] == 'o' && ln[2] == 'd' && ln[3] == 'e' && ln[4] == '=') {
            if (ln[5] == 's') { c->mode = 1; parsed = 1; }
            else if (ln[5] == 'm') { c->mode = 0; parsed = 1; }
        } else if (ln[0] == 't' && ln[1] == 'a' && ln[2] == 's' && ln[3] == 'k' && ln[4] == '=') {
            int k = 0;
            while (ln[5 + k] && k < 63) { c->task[k] = ln[5 + k]; k++; }
            if (k > 0) { c->task[k] = 0; parsed = 1; }
        } else if (ln[0] == 's' && ln[1] == 'l' && ln[2] == 'o' && ln[3] == 't' && ln[4] == '=') {
            if (ln[5] >= '0' && ln[5] <= '7' && !ln[6]) { c->slot = ln[5] - '0'; parsed = 1; }
        } else if (ln[0] == 'n' && ln[1] == 'e' && ln[2] == 't' && ln[3] == '=') {
            unsigned h, p;                     /* DDR-734: net=<a.b.c.d>:<port> */
            if (c->nnet < 8 && parse_hostport(&ln[4], &h, &p)) {
                c->net_host_be[c->nnet] = h;
                c->net_port[c->nnet] = p;
                c->nnet++;
                parsed = 1;
            }
        }
        i = e + 1;
    }
    return parsed;
}

int main(void) {
    long mode = nsi(SYS_GET_MODE, 0, 0, 0);
    printf("PRADYOS_AETHER_DAEMON_OK mode=%s\n", mode ? "sovereign" : "manual");
    fflush(stdout);

    /* DDR-732: load the boot policy from /AETHER.CFG (falls back to the compiled
     * defaults; the mode is applied AFTER the DDR-701 toggle self-check below so
     * the check's fixed sequence stays deterministic). */
    struct aether_cfg cfg;
    if (cfg_load(&cfg))
        printf("PRADYOS_AETHER_CFG_OK mode=%s task=%s slot=%ld\n",
               cfg.mode ? "sovereign" : "manual", cfg.task, cfg.slot);
    else
        printf("PRADYOS_AETHER_CFG_DEFAULT\n");
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

    /* DDR-732: apply the configured mode (the toggle self-check above ended in
     * SOVEREIGN; the config may say otherwise), then spawn the bring-up agent
     * with the configured task into the configured roster slot (DDR-707: slot 0
     * = KRYOS by default). The kernel loads the embedded agent image and marks
     * it CAP_AGENT (+CAP_NET, DDR-731); sovereign mode auto-approves its action,
     * so the default agent completes and prints PRADYOS_AGENT_DONE. */
    /* DDR-734: install the configured egress allowlist BEFORE any agent exists
     * (deny-by-default: with no rules, a CAP_NET agent's connect is -EPERM). */
    {
        int installed = 0;
        for (int i = 0; i < cfg.nnet; i++)
            if (nsi(SYS_NET_ALLOW, (long)cfg.net_host_be[i], (long)cfg.net_port[i], 0) == 0)
                installed++;
        printf("PRADYOS_NET_ALLOW_OK n=%d\n", installed);
        fflush(stdout);
    }
    nsi(SYS_SET_MODE, cfg.mode, 0, 0);
    long apid = nsi(SYS_SPAWN_AGENT, 0, (long)cfg.task, cfg.slot);
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

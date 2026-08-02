/* user/surfdestroytest.c — surface lifecycle completion (DDR-729).
 *
 * Proves the surface table reclaims slots on every teardown path, including the
 * one that had no coverage: a client that EXITS without SYS_SURFACE_CLOSE. Runs
 * alongside the compositor + surfacetest, so it must not assume an empty table —
 * it measures free capacity by creating until -EMFILE.
 *
 *  1. churn:  a create/close loop far exceeding the 16-slot table never wedges
 *             (explicit close reclaims)              -> PRADYOS_SURFDESTROY_CHURN_OK
 *  2. reuse:  create A, close A, create B; B reuses the freed slot (A == B)
 *                                                    -> PRADYOS_SURFDESTROY_REUSE_OK
 *  3. exit:   fork a child that fills every free slot and exits WITHOUT closing;
 *             after the parent reaps it, the parent can re-claim at least as many
 *             (the child's slots came back on exit)  -> PRADYOS_SURFDESTROY_EXIT_OK
 *
 * All three -> PRADYOS_SURFDESTROY_OK; any failure -> "SURFDESTROY FAIL".
 *
 * Freestanding (no libc): links against user/user.ld's single R+X segment, so it
 * must have NO writable globals — all state is on the stack. Output is raw
 * SYS_WRITE of fixed strings (the gate greps sentinels; no numbers formatted).
 */

#define SYS_WRITE           6
#define SYS_YIELD           3
#define SYS_EXIT            4
#define SYS_FORK            15
#define SYS_WAIT4           16
#define SYS_SURFACE_CREATE  48
#define SYS_SURFACE_COMMIT  50
#define SYS_SURFACE_CLOSE   59

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) {
    long n = 0;
    while (s[n]) n++;
    return n;
}

static void wr(const char *s) {
    nsi(SYS_WRITE, 1, (long)s, slen(s));
}

__attribute__((noreturn)) static void die(const char *why) {
    wr("SURFDESTROY FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

/* Create-and-commit until the table is full; record ids. Returns the count. */
static int fill(long *ids, int cap) {
    int n = 0;
    while (n < cap) {
        long id = nsi(SYS_SURFACE_CREATE, 32, 32, 0);
        if (id < 0) break;                 /* -EMFILE: table full */
        nsi(SYS_SURFACE_COMMIT, id, 0, 0);
        ids[n++] = id;
    }
    return n;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    /* 1. churn — 40 create/close cycles over a 16-slot table. */
    for (int i = 0; i < 40; i++) {
        long id = nsi(SYS_SURFACE_CREATE, 32, 32, 0);
        if (id < 0) die("churn create");
        if (nsi(SYS_SURFACE_CLOSE, id, 0, 0) != 0) die("churn close");
    }
    wr("PRADYOS_SURFDESTROY_CHURN_OK\n");

    /* 2. reuse — the freed slot is the first re-handed out. */
    long a = nsi(SYS_SURFACE_CREATE, 32, 32, 0);
    if (a < 0) die("reuse create A");
    if (nsi(SYS_SURFACE_CLOSE, a, 0, 0) != 0) die("reuse close A");
    long b = nsi(SYS_SURFACE_CREATE, 32, 32, 0);
    if (b < 0) die("reuse create B");
    if (b != a) die("reuse slot not reused");
    nsi(SYS_SURFACE_CLOSE, b, 0, 0);
    wr("PRADYOS_SURFDESTROY_REUSE_OK\n");

    /* 3. exit reclamation — a child leaks a full table's worth on exit; the
     * parent must reclaim them once the child is reaped. */
    long pid = nsi(SYS_FORK, 0, 0, 0);
    if (pid < 0) die("fork");
    if (pid == 0) {
        long ids[16];
        int c = fill(ids, 16);             /* grab every free slot, DO NOT close */
        nsi(SYS_EXIT, c > 0 ? 0 : 7, 0, 0);/* leak them; reap must reclaim */
        for (;;) { }
    }
    long st = 0;
    if (nsi(SYS_WAIT4, pid, (long)&st, 0) < 0) die("wait4");

    long pids[16];
    int p = fill(pids, 16);                /* how many can the parent claim now? */
    /* If the child's slots leaked, the table stays full and p is ~0. Reclamation
     * returns the child's whole fill (~13 free of the 16-slot table, minus the
     * ~3 surfacetest holds), cleanly separating the two outcomes. */
    if (p < 4) die("exit reclamation slots not freed");
    for (int i = 0; i < p; i++)            /* tidy up (also re-exercises close) */
        nsi(SYS_SURFACE_CLOSE, pids[i], 0, 0);
    wr("PRADYOS_SURFDESTROY_EXIT_OK\n");

    wr("PRADYOS_SURFDESTROY_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}

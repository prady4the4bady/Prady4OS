/* user/actionquerytest.c — Section 3C ACTION_QUERY_MEMORY (DDR-1018).
 *
 * DDR-1017 §1 listed this type as "unchecked" for the same gap that blocks
 * ACTION_SEND_IPC. IT IS CHECKED NOW AND IT IS NOT BLOCKED: agent memory has a
 * real ring-3 surface, `SYS_MEMORY_WRITE` (NSI 82) and `SYS_MEMORY_READ`
 * (NSI 83), gated on CAP_MEMORY (DDR-836, kernel/syscall/sys_agentmem.c), and
 * `user/agentmemtest.c` already exercises both. So an approved QUERY_MEMORY has
 * an executor, and this follows DDR-1015's shape rather than DDR-1017 §1's.
 *
 * QUERY_MEMORY is NOT in aether_action_forces_pending(), so in sovereign mode it
 * auto-approves and the poll breaks on its first iteration -- which is what
 * makes DDR-1015's loop shape safe here and would NOT be safe for a
 * force-pending type (DDR-1016 §4: AETHER_RATE_MAX kills a busy-polling agent).
 *
 *   1. seed a fact with our own authority: MEMORY_WRITE(key, val)
 *   2. submit ACTION_QUERY_MEMORY with the key as the payload -> action_id
 *   3. poll for the verdict                                   -> AE_APPROVED
 *   4. ONLY THEN read it back: MEMORY_READ(key, out, &outlen)
 *   5. verify the BYTES
 *
 * Step 5 is what keeps the gate non-vacuous. A probe that skipped step 4 still
 * prints the sentinel; it cannot print the value it never read. The gate asserts
 * n and the first byte, exactly as DDR-1015's does, and DDR-1015's M1 showed
 * that the byte-count arm is the one that has to carry it.
 *
 * The seed in step 1 doubles as the control: a read that returns the seeded
 * bytes proves the store works in THIS boot, so n>0 is a measurement rather than
 * an artefact of a store that silently accepts and loses everything.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_WRITE          6
#define SYS_EXIT           4
#define SYS_YIELD          3
#define SYS_SUBMIT_ACTION 31
#define SYS_POLL_RESULT   32
#define SYS_MEMORY_WRITE  82
#define SYS_MEMORY_READ   83

/* Wire format, pinned by _Static_assert in aether.h in this same commit. */
#define ACTION_QUERY_MEMORY 8
#define AE_PENDING          1
#define AE_APPROVED         2

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

static void wrdec(long v) {
    char b[24]; int i = 0;
    if (v < 0) { wr("-"); v = -v; }
    if (!v) { wr("0"); return; }
    while (v && i < 23) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i--) { char c[2]; c[0] = b[i]; c[1] = 0; wr(c); }
}

__attribute__((noreturn)) static void fail(const char *why, long v) {
    wr("ACTIONQUERY FAIL: "); wr(why); wr(" rc="); wrdec(v); wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    static const char key[] = "QFACT";
    static const char val[] = "Quorum reached at tick 7";   /* 24 bytes */

    /* 1. SEED, with our own authority. Not part of the action -- this is the
     * fact the action will ask about, and the control for step 4. */
    long w = nsi(SYS_MEMORY_WRITE, (long)key, (long)val, slen(val));
    if (w != 0)
        fail("seed write", w);

    /* 2. PROPOSE. The payload is the key the action concerns. */
    long id = nsi(SYS_SUBMIT_ACTION, ACTION_QUERY_MEMORY, (long)key, slen(key));
    if (id < 0)
        fail("submit", id);

    /* 3. WAIT FOR THE VERDICT -- two polls around a ring-3 spin, NOT DDR-1015's
     * 20000-iteration loop, and the verdict is REPORTED rather than asserted.
     *
     * Two reasons, both learned the hard way. (a) DDR-1015's loop is safe only
     * while the action auto-approves and breaks it on iteration 1; if it ever
     * stayed PENDING the loop would spend 20000 syscalls and
     * AETHER_RATE_MAX = 60/s would kill the agent before it printed anything
     * (DDR-1016 §4, measured). (b) An earlier draft here called fail() on any
     * non-APPROVED verdict, which meant the printed st could ONLY ever be 2 and
     * the gate's st check could never fire -- the dead-arm class named in
     * DDR-1016 §5 and DDR-1017 §4. Reporting it makes M2 land on that arm. */
    long st = nsi(SYS_POLL_RESULT, id, 0, 0);
    if (st < 0)
        fail("poll1", st);
    if (st == AE_PENDING) {
        for (volatile long i = 0; i < 4000000L; i++) { }   /* 0 syscalls */
        st = nsi(SYS_POLL_RESULT, id, 0, 0);
        if (st < 0)
            fail("poll2", st);
    }

    /* 4. EXECUTE, AND ONLY IF APPROVED. Reading regardless would make the two
     * orders indistinguishable again -- it is the whole property under test.
     * '?' marks "not read" so the reported first byte is never uninitialised. */
    unsigned char out[256];
    unsigned int outlen = 0;
    out[0] = '?';
    if (st == AE_APPROVED) {
        long r = nsi(SYS_MEMORY_READ, (long)key, (long)out, (long)&outlen);
        if (r != 0)
            fail("read", r);
    }

    /* 5. VERIFY THE BYTES. */
    wr("PRADYOS_ACTIONQUERY_OK id=");
    wrdec(id);
    wr(" st=");
    wrdec(st);
    wr(" n=");
    wrdec((long)outlen);
    wr(" first=");
    { char c[2]; c[0] = (char)out[0]; c[1] = 0; wr(c); }
    wr("\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}

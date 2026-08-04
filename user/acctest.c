/* user/acctest.c — DDR-813 ACC gate probe.
 *
 * Exercises the envelope through the SAME kernel/crypto/acc.c the kernel links,
 * compiled a second time with the user code model. A probe that reimplemented
 * the composition would test the probe.
 *
 * Four arms, all previously proven on the host, now asserted in QEMU:
 *
 *   1. seal + open round-trip           -> ACC_OK, plaintext matches
 *   2. tampered ciphertext              -> ACC_ERR_AUTH
 *      tampered signature               -> ACC_ERR_AUTH  (a separate arm: the
 *                                         first is caught by the AEAD tag, the
 *                                         second by Ed25519, and a bug in one
 *                                         does not imply a bug in the other)
 *   3. replay (seq <= last)             -> ACC_ERR_REPLAY
 *   4. owner read AFTER REBOOT          -> ACC_OK
 *
 * Arm 4 is the one ACC exists for and the reason BUG-1 was fixed: it opens the
 * envelope with last_seq=NULL, i.e. no session state at all, which is exactly
 * the owner's situation after a restart — the ephemeral key is gone and the
 * only thing binding the envelope to an agent is agent_sign_pub travelling
 * in-band. An implementation that kept the verify key outside the envelope
 * passes arms 1-3 and fails only here.
 *
 * NO WRITABLE GLOBALS (DDR-826): user.ld gives this ELF one R+X PT_LOAD, so a
 * `static` cache would link fine and fault on first store. Everything is on the
 * stack, and ci-probe-rodata-check enforces it.
 */
#include "acc.h"
#include "x25519.h"

#define SYS_WRITE 6
#define SYS_EXIT  4

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}
static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }
static void wrint(int v) {
    char b[12]; int i = 11; b[i--] = 0;
    int neg = v < 0; unsigned u = (unsigned)(neg ? -v : v);
    if (!u) b[i--] = '0';
    while (u && i >= 0) { b[i--] = (char)('0' + (u % 10u)); u /= 10u; }
    if (neg && i >= 0) b[i--] = '-';
    wr(&b[i + 1]);
}
/* Explicit byte copy. `acc_envelope_t t = env;` compiles to a memcpy CALL,
 * which does not exist in the freestanding user model — the same trap that cost
 * ed25519.c a link failure (DDR-826 session). */
static void env_copy(acc_envelope_t *d, const acc_envelope_t *s) {
    const uint8_t *sp = (const uint8_t *)s;
    uint8_t *dp = (uint8_t *)d;
    for (unsigned i = 0; i < sizeof(acc_envelope_t); i++) dp[i] = sp[i];
}

static void fail(const char *arm, int got, int want) {
    wr("PRADYOS_ACC_STUB arm="); wr(arm);
    wr(" got="); wrint(got);
    wr(" want="); wrint(want); wr("\n");
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    acc_envelope_t env;
    uint8_t owner_priv[32], owner_pub[32], seed[32], out[ACC_MAX_PT];
    uint32_t n = 0;
    int ok = 1, r;

    for (unsigned i = 0; i < 32u; i++) {
        owner_priv[i] = (uint8_t)(i * 3u + 1u);
        seed[i]       = (uint8_t)(i * 5u + 9u);
    }
    if (x25519_scalarmult_base(owner_pub, owner_priv) != 0) {
        wr("ACC FAIL: owner pubkey derivation failed\n");
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }

    static const char MSG[] = "sovereign objective vector v1";
    const uint32_t MLEN = sizeof(MSG) - 1u;
    uint64_t last = 0;

    /* --- arm 1: seal + open round-trip --- */
    r = acc_seal(&env, MSG, MLEN, owner_pub, seed, 1);
    if (r != ACC_OK) { fail("seal", r, ACC_OK); ok = 0; }

    r = acc_open(out, &n, &env, owner_priv, &last);
    if (r != ACC_OK) { fail("open", r, ACC_OK); ok = 0; }
    else if (n != MLEN) { fail("open_len", (int)n, (int)MLEN); ok = 0; }
    else {
        for (uint32_t i = 0; i < MLEN; i++) {
            if (out[i] != (uint8_t)MSG[i]) { fail("plaintext", i, -1); ok = 0; break; }
        }
    }

    /* BUG-1 present: the verify key travels in-band. */
    {
        uint8_t acc_or = 0;
        for (unsigned i = 0; i < ACC_PUB_LEN; i++) acc_or |= env.agent_sign_pub[i];
        if (!acc_or) { fail("agent_sign_pub_absent", 0, 1); ok = 0; }
    }

    /* --- arm 2a: tampered ciphertext (caught by the AEAD tag) --- */
    {
        acc_envelope_t t; env_copy(&t, &env);
        uint64_t l = 0;
        t.ct[0] ^= 0x01u;
        r = acc_open(out, &n, &t, owner_priv, &l);
        if (r != ACC_ERR_AUTH) { fail("tamper_ct", r, ACC_ERR_AUTH); ok = 0; }
    }

    /* --- arm 2b: tampered signature (caught by Ed25519) --- */
    {
        acc_envelope_t t; env_copy(&t, &env);
        uint64_t l = 0;
        t.sig[5] ^= 0x01u;
        r = acc_open(out, &n, &t, owner_priv, &l);
        if (r != ACC_ERR_AUTH) { fail("tamper_sig", r, ACC_ERR_AUTH); ok = 0; }
    }

    /* --- arm 3: replay. `last` was advanced to env.seq by arm 1. --- */
    r = acc_open(out, &n, &env, owner_priv, &last);
    if (r != ACC_ERR_REPLAY) { fail("replay", r, ACC_ERR_REPLAY); ok = 0; }

    /* --- arm 4: owner read after reboot (no session state) --- */
    r = acc_open(out, &n, &env, owner_priv, 0);
    if (r != ACC_OK) { fail("owner_after_reboot", r, ACC_OK); ok = 0; }

    /* A strictly greater sequence must still be accepted, or the replay check
     * would be indistinguishable from "accept nothing". */
    {
        acc_envelope_t e2;
        uint64_t l = 1;
        if (acc_seal(&e2, MSG, MLEN, owner_pub, seed, 2) != ACC_OK ||
            acc_open(out, &n, &e2, owner_priv, &l) != ACC_OK) {
            fail("next_seq", 0, ACC_OK); ok = 0;
        }
    }

    if (!ok) {
        wr("ACC FAIL\n");
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }
    wr("PRADYOS_ACC_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}

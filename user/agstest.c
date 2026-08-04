/* user/agstest.c — DDR-814 AGS gate probe (NSI 79/80).
 *
 * Exercises goal signing through the SAME kernel/aether/ags.c the kernel links,
 * compiled a second time with the user code model. A probe that reimplemented
 * the composition would test the probe.
 *
 * Three arms, each covering something the others cannot:
 *
 *   1. sign + verify round-trip    -> AGS_OK
 *   2. TAMPERED GOAL, same sig     -> AGS_ERR_VERIFY
 *      The security property is that a goal an agent gave itself is
 *      indistinguishable from one the owner set UNLESS the signature separates
 *      them. Arm 1 alone is satisfied by a verify() that returns 0 always.
 *   3. WRONG KEY, untampered goal  -> AGS_ERR_VERIFY
 *      Distinct from arm 2 on purpose: arm 2 fails because the hash changed,
 *      arm 3 because the key did. A verify() that only hashed the goal and
 *      ignored the public key passes arms 1-2 and fails only here — and that
 *      bug would let ANY keyholder authorise goals for this owner.
 *
 * NO WRITABLE GLOBALS (DDR-826): user.ld gives this ELF one R+X PT_LOAD, so a
 * `static` cache would link fine and fault on the first store. Everything is on
 * the stack, and ci-probe-rodata-check enforces it.
 */
#include "ags.h"
#include "ed25519.h"

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

static void fail(const char *why) {
    wr("AGS FAIL: ");
    wr(why);
    wr("\r\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    /* Derived, not recalled: the seed is built here and the public key comes
     * from ed25519_pubkey over it, so the two cannot silently disagree the way
     * a pasted vector pair can. */
    uint8_t seed[32], pub[32], other_seed[32], other_pub[32];
    for (int i = 0; i < 32; i++) {
        seed[i]       = (uint8_t)(0x40 + i);
        other_seed[i] = (uint8_t)(0x90 + i);
    }
    ed25519_pubkey(pub, seed);
    ed25519_pubkey(other_pub, other_seed);

    char goal[64];
    const char *g = "raise throughput without weakening W^X";
    int glen = 0;
    while (g[glen] && glen < 63) { goal[glen] = g[glen]; glen++; }

    uint8_t sig[AGS_SIG_LEN];

    /* Arm 1 — round trip. */
    if (ags_sign(sig, goal, (uint32_t)glen, seed) != AGS_OK)
        fail("sign");
    if (ags_verify(sig, goal, (uint32_t)glen, pub) != AGS_OK)
        fail("verify round-trip");

    /* Arm 2 — tampered goal must be rejected. */
    goal[3] ^= 0x01;
    if (ags_verify(sig, goal, (uint32_t)glen, pub) == AGS_OK)
        fail("tampered goal ACCEPTED");
    goal[3] ^= 0x01;                     /* restore for arm 3 */

    /* Arm 3 — right goal, wrong key, must be rejected. */
    if (ags_verify(sig, goal, (uint32_t)glen, other_pub) == AGS_OK)
        fail("wrong key ACCEPTED");

    wr("PRADYOS_AGS_VECTORS_OK\r\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}

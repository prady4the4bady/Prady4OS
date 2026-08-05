/* user/accrottest.c — DDR-815 ACC session rotation gate (NSI 77/78/81).
 *
 * Unlike acctest.c (which links acc.c and tests the CRYPTO in user space), this
 * probe drives the real SYSCALLS, because the thing under test — the per-agent
 * channel table and its replay floor — lives in the kernel, not in the library.
 *
 * ONE ELF, SPAWNED TWICE at different privilege. It discovers which instance it
 * is from the first call rather than being told:
 *
 *   SYS_ACC_ROTATE on an unknown key returns -ENOENT to a sovereign caller and
 *   -EPERM to a non-sovereign one. Those are distinguishable, so the same binary
 *   covers both sides of the capability split without a second ELF or an
 *   argument channel.
 *
 * Sovereign arms:
 *   1. seal -> open round-trip                      -> 0
 *   2. open the SAME envelope again                 -> -EAGAIN (replay floor)
 *   3. rotate, then re-present a pre-rotation
 *      envelope                                     -> rejected
 *      Arms 1-2 pass with no rotation implemented at all; arm 3 is the one the
 *      feature exists for.
 *
 * Non-sovereign arms:
 *   4. rotate                                       -> -EPERM
 *      open                                         -> -EPERM
 *      Without this the capability check is untested, and rotation would be a
 *      replay tool wearing a safety label (DDR-815).
 *
 * NO WRITABLE GLOBALS (DDR-826): one R+X PT_LOAD, everything on the stack.
 */
#include <stdint.h>
#include "x25519.h"     /* derive the owner box keypair rather than recall it */

#define SYS_WRITE       6
#define SYS_EXIT        4
#define SYS_ACC_SEAL    77
#define SYS_ACC_OPEN    78
#define SYS_ACC_ROTATE  81

#define EPERM   1
#define ENOENT  2
#define EAGAIN  11

/* Mirrors acc_envelope_t. Size must match the kernel's struct exactly — the
 * envelope crosses the ring boundary as a fixed-layout blob. */
#define ACC_PUB_LEN 32u
#define ACC_TAG_LEN 16u
#define ACC_SIG_LEN 64u
#define ACC_MAX_PT  512u

typedef struct {
    uint8_t  version;
    uint8_t  _pad[3];
    uint32_t ctlen;
    uint64_t seq;
    uint8_t  eph_pub[ACC_PUB_LEN];
    uint8_t  agent_sign_pub[ACC_PUB_LEN];
    uint8_t  tag[ACC_TAG_LEN];
    uint8_t  sig[ACC_SIG_LEN];
    uint8_t  ct[ACC_MAX_PT];
} acc_env_t;

struct seal_args {
    uint8_t owner_box_pub[ACC_PUB_LEN];
    uint8_t agent_sign_seed[32];
};

static inline long nsi4(long n, long a1, long a2, long a3, long a4) {
    long r;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}
static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi4(SYS_WRITE, 1, (long)s, slen(s), 0); }

static void wrint(long v) {
    char b[24];
    int i = 23;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    b[i--] = 0;
    if (!u) b[i--] = '0';
    while (u) { b[i--] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) b[i--] = '-';
    wr(&b[i + 1]);
}

/* The code is printed, not just the label: "seal failed" sends you reading
 * acc_seal, while "seal failed rc=-22" says which branch rejected it. */
static void fail(const char *why, long rc) {
    wr("ACCROT FAIL: ");
    wr(why);
    wr(" rc=");
    wrint(rc);
    wr("\r\n");
    nsi4(SYS_EXIT, 1, 0, 0, 0);
    for (;;) { }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    uint8_t probe_key[ACC_PUB_LEN];
    for (unsigned i = 0; i < ACC_PUB_LEN; i++)
        probe_key[i] = (uint8_t)(0xC0 + i);   /* a key no channel can hold yet */

    long who = nsi4(SYS_ACC_ROTATE, (long)probe_key, 0, 0, 0);

    if (who == -EPERM) {
        /* --- non-sovereign instance: arm 4 --- */
        acc_env_t env;
        uint8_t pt[ACC_MAX_PT];
        uint32_t ptlen = 0;
        uint8_t priv[32];
        for (unsigned i = 0; i < 32; i++) priv[i] = (uint8_t)i;
        for (unsigned i = 0; i < sizeof env; i++) ((uint8_t *)&env)[i] = 0;

        long o = nsi4(SYS_ACC_OPEN, (long)pt, (long)&env, (long)priv, (long)&ptlen);
        if (o != -EPERM)
            fail("non-sovereign SYS_ACC_OPEN was not denied", o);

        wr("PRADYOS_ACCROT_DENY_OK\r\n");
        nsi4(SYS_EXIT, 0, 0, 0, 0);
        for (;;) { }
    }

    if (who != -ENOENT)
        fail("sovereign rotate of unknown channel not -ENOENT", who);

    /* --- sovereign instance: arms 1-3 --- */
    struct seal_args args;
    for (unsigned i = 0; i < 32; i++) args.agent_sign_seed[i] = (uint8_t)(0x20 + i);

    /* DERIVED, NOT RECALLED. My first version passed the X25519 base point as
     * owner_box_pub next to an unrelated owner_priv; they were not a keypair, so
     * the two sides computed different shared secrets and open() rejected the
     * envelope as a forgery (-EACCES). Deriving the public key from the private
     * one makes that mismatch unrepresentable — the same reason agstest.c builds
     * its Ed25519 public key with ed25519_pubkey instead of pasting a vector. */
    uint8_t owner_priv[32];
    for (unsigned i = 0; i < 32; i++) owner_priv[i] = (uint8_t)(0x50 + i);
    if (x25519_scalarmult_base(args.owner_box_pub, owner_priv) != 0)
        fail("deriving owner_box_pub", 0);

    acc_env_t env;
    const char *msg = "rotation gate payload";
    long sl = nsi4(SYS_ACC_SEAL, (long)&env, (long)msg, slen(msg), (long)&args);
    if (sl != 0)
        fail("seal", sl);

    uint8_t pt[ACC_MAX_PT];
    uint32_t ptlen = 0;
    long o1 = nsi4(SYS_ACC_OPEN, (long)pt, (long)&env, (long)owner_priv, (long)&ptlen);
    if (o1 != 0)
        fail("round-trip open", o1);

    /* Arm 2: the same envelope again must trip the replay floor. */
    long o2 = nsi4(SYS_ACC_OPEN, (long)pt, (long)&env, (long)owner_priv, (long)&ptlen);
    if (o2 != -EAGAIN)
        fail("replay NOT rejected before rotation", o2);

    /* Arm 3: rotate this agent's channel, then re-present the old envelope. */
    long rr = nsi4(SYS_ACC_ROTATE, (long)env.agent_sign_pub, 0, 0, 0);
    if (rr != 0)
        fail("rotate of a live channel", rr);

    long o3 = nsi4(SYS_ACC_OPEN, (long)pt, (long)&env, (long)owner_priv, (long)&ptlen);
    if (o3 == 0)
        fail("pre-rotation envelope ACCEPTED after rotation", o3);

    wr("PRADYOS_ACCROT_OK\r\n");
    nsi4(SYS_EXIT, 0, 0, 0, 0);
    for (;;) { }
}

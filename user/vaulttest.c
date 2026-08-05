/* user/vaulttest.c — DDR-834 credential vault gate (NSI 87/91).
 *
 * ONE ELF SPAWNED TWICE at different privilege, the same shape as accrottest.c:
 * the capability split is part of what is under test, and one privilege level
 * can only ever test half of it. The instances tell themselves apart from the
 * return of a GET on a name nothing has stored — -ENOENT to a sovereign caller,
 * -EPERM to an agent.
 *
 * Sovereign arms:
 *   1. put -> get round-trip, byte-for-byte
 *   2. get an unknown name -> -ENOENT, NOT an empty success. A vault that
 *      returns zero bytes for a missing key hands back a "credential" that is
 *      silently wrong, and the caller finds out somewhere far away.
 *   3. TAMPER /VAULT.BIN through the ordinary file syscalls, then get ->
 *      REJECTED. This is the arm the feature exists for: arms 1-2 pass on a
 *      vault that stores plaintext. Tampering through the FS is how an attacker
 *      with disk access would do it.
 *
 * Agent arm:
 *   4. put and get both -> -EPERM.
 *
 * NO WRITABLE GLOBALS (DDR-826): one R+X PT_LOAD, everything on the stack.
 */
#include <stdint.h>

#define SYS_READ        5
#define SYS_WRITE       6
#define SYS_OPEN        7
#define SYS_CLOSE       8
#define SYS_EXIT        4
#define SYS_VAULT_PUT   87
#define SYS_VAULT_GET   91

#define O_WRONLY 1

#define EPERM   1
#define ENOENT  2
#define EACCES  13

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}
static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

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

static void fail(const char *why, long rc) {
    wr("VAULT FAIL: ");
    wr(why);
    wr(" rc=");
    wrint(rc);
    wr("\r\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    char out[256];
    uint32_t outlen = 0;

    long who = nsi(SYS_VAULT_GET, (long)"NO_SUCH_KEY", (long)out, (long)&outlen);

    if (who == -EPERM) {
        /* --- agent instance: arm 4 --- */
        long p = nsi(SYS_VAULT_PUT, (long)"AGENT_TRY", (long)"x", 1);
        if (p != -EPERM)
            fail("non-sovereign SYS_VAULT_PUT was not denied", p);
        wr("PRADYOS_VAULT_DENY_OK\r\n");
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }

    /* --- sovereign instance --- */
    /* Arm 2 first: the probe already made this call to identify itself, so
     * assert on it rather than repeating it. */
    if (who != -ENOENT)
        fail("get of an unknown name was not -ENOENT", who);

    const char *secret = "cloud-bridge-token-abc123";
    long pr = nsi(SYS_VAULT_PUT, (long)"BRIDGE_TOKEN", (long)secret, slen(secret));
    if (pr != 0)
        fail("put", pr);

    /* Arm 1: round-trip. */
    long gr = nsi(SYS_VAULT_GET, (long)"BRIDGE_TOKEN", (long)out, (long)&outlen);
    if (gr != 0)
        fail("get round-trip", gr);
    if (outlen != (uint32_t)slen(secret))
        fail("round-trip length mismatch", (long)outlen);
    for (uint32_t i = 0; i < outlen; i++) {
        if (out[i] != secret[i])
            fail("round-trip byte mismatch", (long)i);
    }

    /* Arm 3: corrupt the stored ciphertext on disk, then read it back. The
     * offset lands inside the first record's ct field, past name+nonce+tag+len
     * (16+12+16+4 = 48). */
    long fd = nsi(SYS_OPEN, (long)"/VAULT.BIN", O_WRONLY, 0);
    if (fd < 0)
        fail("open /VAULT.BIN for tamper", fd);
    /* SYS_WRITE appends at the file offset, so a fresh WRONLY handle writes at
     * 0; overwrite the head of the record, which includes the name field and
     * therefore also breaks the AAD binding. Either way the tag must reject. */
    const char *junk = "TAMPERTAMPERTAMPERTAMPERTAMPERTAMPERTAMPERTAMPER";
    long wrote = nsi(SYS_WRITE, fd, (long)junk, slen(junk));
    nsi(SYS_CLOSE, fd, 0, 0);
    if (wrote <= 0)
        fail("tamper write", wrote);

    long tr = nsi(SYS_VAULT_GET, (long)"BRIDGE_TOKEN", (long)out, (long)&outlen);
    if (tr == 0)
        fail("TAMPERED vault record was ACCEPTED", tr);

    wr("PRADYOS_VAULT_OK\r\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}

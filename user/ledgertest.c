/* user/ledgertest.c -- DDR-1150: SYS_LEDGER (NSI 106) probe, standing in for the
 * installer (DDR-1143 piece 5, not yet built) that will perform the KEYGEN.
 *
 * Two roles, one per process, named in argv[1]:
 *   SOV    sovereign. SIGN before any key (-ENOKEY), KEYGEN, KEYGEN again
 *          (-EEXIST), PUBKEY, SIGN, one audited event, SIGN again.
 *   PLAIN  NOT sovereign. KEYGEN / PUBKEY / SIGN must all be -EPERM.
 *
 * The probe REPORTS and the gate JUDGES (DDR-1020). The signatures are NOT
 * verified here: tools/ci/ledger_verify.py does that off-box with an independent
 * implementation, because a kernel verifying its own signature passes on any
 * self-consistent wrong implementation (the dead-arm class).
 *
 * The seed KEYGEN returns is deliberately NOT printed: it is the secret the
 * installer would write to the target disk. Hex output is chunked at 64 bytes
 * per line, each line ONE write(2) (DDR-1056). Freestanding, no writable
 * globals (user.ld). */
#include "uline.h"

#define SYS_EXIT        4
#define SYS_WRITE       6
#define SYS_GET_MODE   29
#define SYS_SET_MODE   30
#define SYS_LEDGER    106

#define LEDGER_KEYGEN   1
#define LEDGER_PUBKEY   3
#define LEDGER_SIGN     4

#define PK_BYTES     1312
#define SIG_BYTES    2420
#define MSG_BYTES      58

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static inline unsigned long tsc(void) {
    unsigned lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long)hi << 32) | lo;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void emit(uline *u) {
    const char *l = ul_end(u);
    nsi(SYS_WRITE, 1, (long)l, slen(l));
}
static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* "<tag> n=<k> i=<chunk> <hex>" per 64 bytes. */
static void hexdump(const char *tag, long k, const unsigned char *p, long len) {
    static const char hx[] = "0123456789ABCDEF";
    for (long off = 0, i = 0; off < len; off += 64, i++) {
        uline u; ul_init(&u);
        ul_s(&u, tag); ul_s(&u, " n="); ul_d(&u, k);
        ul_s(&u, " i="); ul_d(&u, i); ul_c(&u, ' ');
        for (long j = off; j < len && j < off + 64; j++) {
            ul_c(&u, hx[p[j] >> 4]); ul_c(&u, hx[p[j] & 15]);
        }
        ul_c(&u, '\n');
        emit(&u);
    }
}

static long sign_and_dump(long k, unsigned char *buf) {
    long r = nsi(SYS_LEDGER, LEDGER_SIGN, (long)buf, MSG_BYTES + SIG_BYTES);
    if (r == MSG_BYTES) {
        hexdump("PRADYOS_LEDGER_MSG", k, buf, MSG_BYTES);
        hexdump("PRADYOS_LEDGER_SIG", k, buf + MSG_BYTES, SIG_BYTES);
    }
    return r;
}

static void role_sov(void) {
    unsigned char seed[32];
    unsigned char pk[PK_BYTES];
    unsigned char buf[MSG_BYTES + SIG_BYTES];

    long nokey = nsi(SYS_LEDGER, LEDGER_SIGN, (long)buf, sizeof buf);
    long kg    = nsi(SYS_LEDGER, LEDGER_KEYGEN, (long)seed, sizeof seed);
    long again = nsi(SYS_LEDGER, LEDGER_KEYGEN, (long)seed, sizeof seed);
    long pkr   = nsi(SYS_LEDGER, LEDGER_PUBKEY, (long)pk, sizeof pk);
    if (pkr == PK_BYTES)
        hexdump("PRADYOS_LEDGER_PK", 0, pk, PK_BYTES);
    /* DDR-1150 sec.5: how long SIGN holds IF masked. rdtsc, not the vDSO
     * clock -- that one is advanced by the PIT interrupt, i.e. it is blind to
     * exactly the window being measured. Emulated TSC under TCG (DDR-870):
     * a comparison figure, printed beside a one-syscall baseline as its
     * denominator, never a hardware claim. NOT asserted by the gate. */
    unsigned long t0 = tsc();
    long mode = nsi(SYS_GET_MODE, 0, 0, 0);
    unsigned long t1 = tsc();
    long s1r = nsi(SYS_LEDGER, LEDGER_SIGN, (long)buf, sizeof buf);
    unsigned long t2 = tsc();
    (void)s1r;
    long s1 = sign_and_dump(1, buf);
    /* One audited event between the two signatures: a successful SET_MODE to
     * the CURRENT mode changes nothing and appends AR_MODE_SET (DDR-1147). */
    long ev = nsi(SYS_SET_MODE, mode, 0, 0);
    long s2 = sign_and_dump(2, buf);

    uline u; ul_init(&u);
    ul_s(&u, "PRADYOS_LEDGER_SOV nokey="); ul_d(&u, nokey);
    ul_s(&u, " keygen="); ul_d(&u, kg);
    ul_s(&u, " again=");  ul_d(&u, again);
    ul_s(&u, " pk=");     ul_d(&u, pkr);
    ul_s(&u, " s1=");     ul_d(&u, s1);
    ul_s(&u, " ev=");     ul_d(&u, ev);
    ul_s(&u, " s2=");     ul_d(&u, s2);
    ul_s(&u, " sign_tsc="); ul_d(&u, (long)(t2 - t1));
    ul_s(&u, " base_tsc="); ul_d(&u, (long)(t1 - t0));
    ul_c(&u, '\n');
    emit(&u);
}

static void role_plain(void) {
    unsigned char seed[32];
    unsigned char pk[PK_BYTES];
    unsigned char buf[MSG_BYTES + SIG_BYTES];
    long kg = nsi(SYS_LEDGER, LEDGER_KEYGEN, (long)seed, sizeof seed);
    long p  = nsi(SYS_LEDGER, LEDGER_PUBKEY, (long)pk, sizeof pk);
    long s  = nsi(SYS_LEDGER, LEDGER_SIGN, (long)buf, sizeof buf);
    uline u; ul_init(&u);
    ul_s(&u, "PRADYOS_LEDGER_PLAIN keygen="); ul_d(&u, kg);
    ul_s(&u, " pk="); ul_d(&u, p);
    ul_s(&u, " sign="); ul_d(&u, s);
    ul_c(&u, '\n');
    emit(&u);
}

__asm__(".globl _start\n"
        "_start:\n"
        "  mov %rsp, %rdi\n"
        "  and $-16, %rsp\n"
        "  call ledger_main\n"
        "  ud2\n");

__attribute__((noreturn, force_align_arg_pointer, used))
void ledger_main(long *sp) {
    long argc = sp[0];
    const char *role = (argc >= 2) ? (const char *)sp[2] : "NOARGS";
    if (streq(role, "SOV"))
        role_sov();
    else
        role_plain();
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}

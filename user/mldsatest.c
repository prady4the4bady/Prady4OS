/* user/mldsatest.c — DDR-1054 FIPS 204 ML-DSA-44 keyGen known-answer probe.
 *
 * Links kernel/crypto/mldsa.c and kernel/crypto/keccak.c — the SAME SOURCES,
 * compiled a second time for ring 3, exactly as shaketest.c does for Keccak.
 * A separate userspace copy could drift and the gate would still pass.
 *
 * The vectors are NIST's own ACVP FIPS 204 keyGen vectors (DDR-1053), fetched
 * by tools/ci/fetch_mldsa_kat.py from usnistgov/ACVP-Server and pinned
 * byte-exact. That is what makes this a KNOWN-ANSWER test: a sign-then-verify
 * round trip passes on ANY self-consistent wrong implementation, which is the
 * dead-arm class this project has hit repeatedly.
 *
 * keyGen is chosen because it is DETERMINISTIC — one 32-byte seed maps to
 * exactly one (pk, sk), with no signing randomness — so a mismatch is
 * unambiguous rather than a probabilistic argument.
 *
 * The ~21 KiB scratch is a STACK local, not a global: user/user.ld gives this
 * probe a single R+X PT_LOAD, so any writable allocated section would link
 * cleanly and fault on its first store (DDR-826). It fits inside the 32 KiB
 * ADR-038 eagerly maps, so no stack growth is needed either.
 */
#include "mldsa.h"
#include "uline.h"

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

__attribute__((force_align_arg_pointer))
void _start(void) {
    mldsa44_scratch sc;
    int bad;

    sc.tables_ready = 0;              /* no BSS here, so init explicitly */
    bad = mldsa44_selftest(&sc);

    if (bad != 0) {
        /* DDR-1056: ONE write. Also note the SIGN — a negative index is a
         * Power2Round boundary case, which the ACVP vectors do not reach
         * (measured: 0 hits in 2048 coefficients), so the two arms must stay
         * distinguishable in the log. */
        uline u; ul_init(&u);
        ul_s(&u, "PRADYOS_MLDSA_STUB first_bad=");
        ul_d(&u, bad);
        ul_s(&u, bad < 0 ? " arm=power2round\n" : " arm=acvp_kat\n");
        wr(ul_end(&u));
        wr("MLDSA FAIL\n");
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }

    {   uline u; ul_init(&u);
        ul_s(&u, "PRADYOS_MLDSA44_KEYGEN_OK acvp=");
        ul_d(&u, (long)mldsa44_kat_count());
        ul_s(&u, " p2r=");
        ul_d(&u, (long)mldsa44_p2r_count());
        ul_c(&u, '\n');
        wr(ul_end(&u)); }

    /* DDR-1057: signing. A separate scope so the two scratches need not be live
     * at once -- 21 KiB + 54 KiB would still fit the 8 MiB demand-paged stack,
     * but there is no reason to ask for both. */
    {
        mldsa44_sign_scratch ss;
        int sbad;
        ss.tables_ready = 0;
        sbad = mldsa44_sign_selftest(&ss);
        if (sbad != 0) {
            uline u; ul_init(&u);
            ul_s(&u, "PRADYOS_MLDSA_SIGN_STUB first_bad=");
            ul_d(&u, sbad);
            ul_s(&u, sbad < 0 ? " arm=decompose\n" : " arm=acvp_sig\n");
            wr(ul_end(&u));
            wr("MLDSA FAIL\n");
            nsi(SYS_EXIT, 1, 0, 0);
            for (;;) { }
        }
        {   uline u; ul_init(&u);
            ul_s(&u, "PRADYOS_MLDSA44_SIGN_OK acvp=");
            ul_d(&u, (long)mldsa44_sig_kat_count());
            ul_s(&u, " dec=");
            ul_d(&u, (long)mldsa44_decomp_count());
            ul_c(&u, '\n');
            wr(ul_end(&u)); }
    }

    /* DDR-1058: verification. Its own scope and its own scratch (~27 KiB). */
    {
        mldsa44_verify_scratch vs;
        int vbad;
        vs.tables_ready = 0;
        vbad = mldsa44_verify_selftest(&vs);
        if (vbad != 0) {
            uline u; ul_init(&u);
            ul_s(&u, "PRADYOS_MLDSA_VER_STUB first_bad=");
            ul_d(&u, vbad);
            ul_s(&u, vbad < 0 ? " arm=usehint\n" : " arm=acvp_ver\n");
            wr(ul_end(&u));
            wr("MLDSA FAIL\n");
            nsi(SYS_EXIT, 1, 0, 0);
            for (;;) { }
        }
        {   uline u; ul_init(&u);
            ul_s(&u, "PRADYOS_MLDSA44_VERIFY_OK acvp=");
            ul_d(&u, (long)mldsa44_ver_kat_count());
            ul_s(&u, " uh=");
            ul_d(&u, (long)mldsa44_usehint_count());
            ul_c(&u, '\n');
            wr(ul_end(&u)); }
    }

    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}

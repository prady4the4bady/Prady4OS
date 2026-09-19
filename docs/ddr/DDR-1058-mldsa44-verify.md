# DDR-1058 — ML-DSA-44 Verify_internal (FIPS 204), implemented + gated + V1/V5

**Status:** IMPLEMENTED + gated + mutation-proven on the shipped kernel
**Date:** 2026-09-03
**Branch:** `dev/phase1-seyp3n`
**Completes:** the ML-DSA-44 primitive set — DDR-1054 keyGen, DDR-1057 sign,
this verify. All three byte-exact (or verdict-exact) against NIST's own vectors.

---

## 1. The vector set is mostly NEGATIVE, and that is the point

ACVP's ML-DSA-44 `Verify_internal` group is **3 signatures that must verify and
12 that must not**. That asymmetry is what makes a verify gate mean anything: an
implementation that always answers "valid" passes every positive test, and
sign-then-verify — the shape this project keeps rejecting — is exactly such an
implementation in disguise.

The pinned set therefore keeps **both verdicts**: tcIds 116 and 114 (ACCEPT),
120, 107 and 117 (REJECT), with the expected verdict recorded per vector. The
generator refuses to emit a one-sided set.

Proven, not asserted:

| mutant | result |
|---|---|
| **V1** always ACCEPT | fails at vector **3** — the first REJECT |
| **V2** always REJECT | fails at vector **1** — the first ACCEPT |

Neither trivial answer survives, which is the property a one-sided set could not
give.

## 2. What the negative vectors actually catch — measured per guard

Rather than assume the twelve rejects exercise the parsing checks, each was
decoded and classified:

- **Hint-encoding validation** catches tcIds **107, 113, 119** — malformed hint
  encodings. Mutant **V4** (drop the validation) fails at vector 4, so this is
  real coverage of the checks that stop a verifier being fed an arbitrary byte
  string.
- **The `||z||inf < gamma1 - beta` bound catches NONE of them.** Not one of the
  twelve is an out-of-range-`z` rejection.

## 3. THE FINDING: that bound cannot be covered by a synthetic vector either

Mutant **V3** (remove the `z` bound entirely) **passes all five pinned vectors**,
and the obvious repair — forge a signature with an out-of-range `z` and assert it
is rejected — was **measured before being written, and does not work**:

```
forged out-of-range z: verify WITH bound check = False, WITHOUT = False
```

Altering `z` changes `w'approx`, hence `w1'`, hence `c~'`, so the **hash
comparison rejects the forgery either way**. A test asserting "this is rejected"
would therefore pass on an implementation with **no bound check at all** — the
dead-arm class, and this is the second time it has been caught in *design* rather
than after shipping (DDR-1039 was the first).

So the bound is recorded as **measured-uncovered**, with the reason, rather than
given a decorative test. Isolating it needs a signature that is simultaneously
out-of-range in `z` and hash-consistent, which is a forgery of the scheme itself.
NIST does not supply one; neither can I.

## 4. UseHint's boundary — the third function in a row

`r0 == 0` with the hint bit set is not reached by any pinned vector: mutant
**V5** (`r0 > 0` -> `r0 >= 0`) passes all five. Same shape as DDR-1054's
`Power2Round` and DDR-1057's `Decompose`, now in a third function — which is why
these direct boundary arms have become routine rather than a one-off. Thirteen
cases generated from FIPS 204 Alg. 40, including `r = 190464` (`r0 == 0`
exactly, where the mutant yields 2 instead of 0) and `r = q-1`, where `Decompose`
takes its own special branch. Reported with a NEGATIVE index.

## 5. Proof

**Python oracle first**, as with signing: `verify_internal` in
`tools/ci/mldsa_sign_ref.py` matches **15 of 15** ACVP verdicts before any C.

**On the running OS**, `smoke-mldsa` now asserts three sentinels, read back out
of the capture:

```
PRADYOS_MLDSA44_KEYGEN_OK acvp=2 p2r=10
PRADYOS_MLDSA44_SIGN_OK   acvp=2 dec=12
PRADYOS_MLDSA44_VERIFY_OK acvp=5 uh=13
```

**Mutants on the shipped kernel:**

| | kernel | gate | capture |
|---|---|---|---|
| clean | `46016bc8c7c7fa3b` | rc=0 | all three sentinels |
| V1 (always accept) | `6d026ed03832b65c` | **rc=1** | `first_bad=3 arm=acvp_ver` |
| V5 (`r0 > 0` -> `>=`) | `336448d366164153` | **rc=1** | `first_bad=-3 arm=usehint` |

Revert returns `46016bc8c7c7fa3b` bit-for-bit.

## 6. Its own scratch, deliberately

`mldsa44_verify_scratch` (27,400 B) is separate from signing's rather than
reusing it. Verify's polynomials mean different things — `t1`, `z-hat` — and
aliasing them onto `s1h`/`yh` would make the code read as something it is not.
In crypto that is how a subtly wrong answer ships.

## 7. Measurements

| | value |
|---|---|
| kernel | `46016bc8c7c7fa3b` |
| `kernel.bin` | 1,249,674 -> **1,278,346 B** against the 1,572,864 B gate (294,518 B headroom) |
| `mldsatest.elf` | 43,016 -> **72,536 B** |
| `sizeof(mldsa44_verify_scratch)` | 27,400 B |
| build | rc=0, zero warnings at `-Werror` |
| `hygiene_check.sh` | ALL SIX |

## 8. What is NOT claimed

- **No application.** The audit ledger still uses SHA-256. Nothing in this OS
  calls keyGen, sign or verify in anger; `mldsa.c` compiles into the ring-3 probe
  only. The primitive set is complete; the *use* of it is not built.
- **The `||z||inf` bound is untested** (§3), and it is a real FIPS 204 step.
- **Nothing about constant-time behaviour.** Verification handles no secrets, so
  it matters far less here than for signing — but it is still not claimed.
- ML-DSA-44 only; `internal` interface only, so the message-encoding wrapper and
  pre-hash variants are not exercised.

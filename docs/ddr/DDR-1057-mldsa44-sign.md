# DDR-1057 — ML-DSA-44 Sign_internal (FIPS 204), implemented + gated + S1/S4

**Status:** IMPLEMENTED + gated + mutation-proven on the shipped kernel
**Date:** 2026-09-03
**Branch:** `dev/phase1-seyp3n`
**Follows:** DDR-1052 (Keccak), DDR-1053 (keyGen vectors), DDR-1054 (keyGen).

DDR-1054 shipped key generation and closed with "no `sigGen`/`sigVer`, so nothing
in this OS is post-quantum *authenticated* yet". Signing exists now.

---

## 1. The blocker that did not exist, again

DDR-1054 §8 named the next step's risk. It does not materialise, for the same
reason DDR-1053's didn't: **ACVP publishes DETERMINISTIC sigGen groups.**

FIPS 204 signs with a random `rnd` by default, and a randomized signature can
only be checked by *verifying* it — which passes on any self-consistent wrong
implementation, the trap this project keeps meeting. But NIST's vector set
carries `deterministic: true` groups (`rnd` = 32 zero bytes), and among them
`signatureInterface: internal, externalMu: false` — **`ML-DSA.Sign_internal`
itself**, with no message-encoding wrapper and no pre-hash. So one
`(sk, message)` maps to exactly one signature, the answer is an ACVP constant,
and a failure localises to the signing algorithm rather than to the envelope
around it.

`tools/ci/fetch_mldsa_kat.py` now pins those too. Re-running it regenerated the
**keyGen header bit-identically**, which is the property a provenance tool is
supposed to have and is worth checking rather than assuming.

## 2. Vector selection is a design choice, not the first two rows

The two pinned vectors are tcId **110 (message 1 byte)** and **118 (273 bytes)**.
Shortest-first is deliberate — the headers are large — but not *only* shortest:
a set whose messages all fit inside one SHAKE block never exercises multi-block
absorption in `mu = H(tr || M)`. 273 bytes crosses SHAKE256's 136-byte rate
twice.

## 3. Proof

**Python first, C second** — the DDR-1052 discipline, and the reason keyGen
worked on the first attempt. `tools/ci/mldsa_sign_ref.py` implements
Sign_internal and reproduces the ACVP vectors byte-exactly; only then was the C
written, so every stage had a ground-truth value instead of a 2420-byte
all-or-nothing answer.

**On the running OS:** `smoke-mldsa` now asserts two sentinels, read back out of
the capture rather than inferred from rc=0 (DDR-1041):

```
PRADYOS_MLDSA44_KEYGEN_OK acvp=2 p2r=10
PRADYOS_MLDSA44_SIGN_OK acvp=2 dec=12
```

Counts are reported by the probe, never literals.

**Mutants on the SHIPPED KERNEL:**

| | kernel | gate | capture |
|---|---|---|---|
| clean | `9d3a813c3910ac1f` | rc=0 | both sentinels |
| S1 (`rnd` not all-zero) | `ab06e1593d5ea1c5` | **rc=1** | `first_bad=1 arm=acvp_sig` |
| S4 (`lo > GAMMA2` -> `>=`) | `0d3f5e7782071cef` | **rc=1** | `first_bad=-4 arm=decompose` |

Reverting returns `9d3a813c3910ac1f` **bit-for-bit**. Two mutants, two different
arms, distinguished by the sign of `first_bad`.

Host-build mutants that also failed as intended: `TAU` 39 -> 38 (ball weight),
dropping the `c[i] = c[j]` swap in SampleInBall, and dropping `tr` from the `mu`
hash.

## 4. TWO branches the KATs do not cover — measured, and only one is fixable here

**S4: `Decompose`'s boundary.** `lo == GAMMA2` exactly occurs in **0 of the
28,672 decompose calls** across both pinned vectors (~0.15 expected). A mutant
flipping `>` to `>=` therefore **passed the KAT arm outright** — exactly the
shape DDR-1054 found in `Power2Round`, in a different function. Fixed the same
way: twelve direct cases generated from FIPS 204 Alg. 36, bracketing the
boundary from both sides, plus `r = q-1` for the `r+ - r0 == q-1` special
branch. Reported with a NEGATIVE index so the arms cannot be confused. S4 now
fails at `-4`, which is `r = 95232`, the boundary itself.

**S5: the `hint_total > OMEGA` rejection.** Defeating it entirely still passes,
because for well-formed keys the hint weight never approaches 80. This one is
**NOT fixed by a unit arm** and is recorded as uncovered rather than papered
over: reaching it needs a crafted secret key, which is not a known-answer test.

What *was* done about it is a memory-safety guard, because that check is the
only thing keeping `HintBitPack` inside the 2420-byte signature: past OMEGA the
index run would overrun into the per-row counts and then past the buffer. The
encoder now re-checks the bound at the point of the write and returns -1, so the
overflow is impossible even if the earlier test were ever wrong. **An untested
guard on a fixed-size buffer write is worth not relying on.**

## 5. Bounded, deliberately

FIPS 204 states no iteration cap on the rejection loop; the expected count for
ML-DSA-44 is about 4.25. An unbounded loop is an S2 violation in this codebase
(DDR-961, DDR-994), so `SIGN_MAX_ITERS` is 1000 and exceeding it returns -1
rather than spinning. The cap is far beyond any plausible run and is still a
hard ceiling.

## 6. Measurements

| | value |
|---|---|
| kernel | `9d3a813c3910ac1f` |
| `kernel.bin` | 1,229,194 -> **1,249,674 B** against the 1,572,864 B gate |
| `mldsatest.elf` | 24,408 -> **43,016 B** |
| `sizeof(mldsa44_sign_scratch)` | 54,396 B (stack local; the user stack is 8 MiB, demand-paged) |
| build | rc=0, zero warnings at `-Werror`; host build clean at `-Wall -Wextra -Werror` |
| `hygiene_check.sh` | ALL SIX, incl. `ci-probe-rodata-check` |
| gate | `smoke-mldsa`, shard 0, strict, window 120 -> 180 s |

## 7. What is NOT claimed

- **No `sigVer`.** Verification is not implemented, so this OS can produce a
  post-quantum signature and cannot yet check one. The vectors are at the same
  reachable ACVP source.
- **No application.** The audit ledger still uses SHA-256; nothing calls this.
  The kernel does not contain ML-DSA — `mldsa.c` compiles into the ring-3 probe
  only, as `keccak.c` also does for `shaketest`.
- **Deterministic signing only.** The hedged (random-`rnd`) mode is not
  exercised; it is the same code with a different `rnd`, but that is an argument,
  not a measurement.
- **Nothing about constant-time behaviour**, and signing is the operation where
  that matters most: the reduction is a 64-bit `%`, whose timing is
  data-dependent on some hardware, and the rejection loop's iteration count is
  itself a function of secret-dependent values. **This implementation should not
  be used against an adversary who can measure it** until that is addressed.
  Stated plainly rather than left for a reader to infer.
- ML-DSA-44 only.

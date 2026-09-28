# DDR-1054 — ML-DSA-44 (FIPS 204) key generation, implemented + gated + M1/M3

**Status:** IMPLEMENTED + gated + mutation-proven on the shipped kernel
**Date:** 2026-09-03
**Branch:** `dev/phase1-seyp3n`
**Follows:** DDR-1052 (Keccak/SHAKE — the prerequisite), DDR-1053 (the pinned
ACVP vectors and the verified Python oracle).

**This is step 3 of Post-Quantum Security, which CLAUDE.md §PHASE 3 makes
MANDATORY v1 scope, landing before the ISO.** DDR-1053 closed with "NO ML-DSA
IMPLEMENTATION SHIPS IN THIS CHANGE". One does now.

---

## 1. What is claimed, and what is not

**Claimed:** ML-DSA-44 **keyGen** produces, for a given 32-byte seed, the exact
`(pk, sk)` NIST's own ACVP vectors specify — 1312 and 2560 bytes, byte-for-byte,
verified on the running OS.

**Not claimed:**
- **No signing and no verification.** `sigGen`/`sigVer` are not implemented.
  Nothing in this OS is post-quantum *authenticated* yet; the audit ledger still
  uses SHA-256.
- **ML-DSA-44 only.** The other two parameter sets are not built.
- **Nothing about constant-time behaviour.** The modular reduction here is a
  64-bit `%`, whose timing is data-dependent on some hardware, and QEMU under TCG
  could not measure it anyway. keyGen from a fixed seed is not a secret-dependent
  online operation, but this is stated rather than glossed.
- **The kernel does not contain ML-DSA.** `kernel/crypto/mldsa.c` is compiled
  into the ring-3 probe only, exactly as `kernel/crypto/keccak.c` is *also*
  compiled for `shaketest`. Nothing in ring 0 calls it, because nothing needs it
  until the ledger does. It is real, tested code that is not yet a dependency.

## 2. Why keyGen, and why byte-exact vectors

keyGen is **deterministic** — one seed maps to exactly one key pair, with no
signing randomness — so a mismatch is unambiguous.

The alternative shape, sign-then-verify, is the trap this project has hit nine
or more times under the name **the dead-arm class**: it passes on *any*
self-consistent wrong implementation. A wrong-but-consistent ML-DSA would sign
and verify its own signatures happily forever. Only an answer computed by
someone else can catch that, which is what DDR-1053 went and fetched from
`usnistgov/ACVP-Server`.

## 3. No magic constants — everything derived

DDR-1052 learned this expensively: its first Keccak round-constant generator
produced `RC[0] = 0x03` instead of `0x01`, a silent total break of exactly the
kind that hand-copying 24 magic 64-bit values invites.

So nothing here is transcribed:

- the 256 twiddle factors are computed as `zeta^brv8(i) mod q` from
  `zeta = 1753`;
- the inverse-NTT scale factor is computed as `256^(q-2) mod q` by Fermat —
  **which evaluates to 8347681, the literal the reference implementations
  carry.** That equality was checked rather than assumed.

The only pinned numbers in the whole change are NIST's own vectors.

## 4. All mutable state is caller-owned, and that is load-bearing twice

`mldsa44_scratch` holds the derived tables, the working polynomials **and** the
selftest's `pk`/`sk` buffers. There is not one `static` mutable object in
`mldsa.c`.

1. **Reentrancy.** `mldsa.h` claimed this from its first draft; a lazily-filled
   file-scope table would have quietly broken it. Putting the tables in the
   struct is what actually delivers the property the header advertises.
2. **The gate could not exist otherwise.** `user/user.ld` links each probe as a
   SINGLE R+X PT_LOAD, so a probe with any writable allocated section links
   cleanly and then **faults on its first store** — DDR-826, which is exactly
   how `smoke-ed25519` once failed with "sentinel not found", a message that
   reads as "the crypto is wrong" and was nothing of the kind.
   `ci-probe-rodata-check` passes on `mldsatest.elf`.

The scratch is **21,288 bytes**, and the header's original "~10 KiB" was wrong
even for the fields it then listed (`s1hat`/`t1`/`t0` are 4 KiB each). It is a
stack local in the probe, inside the 32 KiB ADR-038 maps eagerly, so no stack
growth is involved either.

## 5. THE KATs DO NOT COVER Power2Round's BOUNDARY — measured, not assumed

Five mutants were run against the host build first. Three failed as intended.
**Two passed, and both are findings.**

**M3** — `power2round`'s `r0 > 2^(D-1)` flipped to `>=` — **passed the ACVP
arm outright.** The reason is not subtle once measured: `r0 == 2^(D-1)` exactly
occurs in **0 of the 2048 coefficients** across both pinned vectors, and the
expected rate is ~0.125 hits per key. Two vectors will essentially never reach
it. A gate resting on the KATs alone would have shipped that branch untested.

So the selftest gained a **direct Power2Round arm**: ten cases generated from
the FIPS 204 definition, bracketing the boundary from both sides. M3 now fails
it. The arm reports a **negative** index so a boundary failure can never be
mistaken for a vector failure in a log.

**M4** — the `s1`/`s2` domain index base `MLDSA_L + r` changed to `MLDSA_K + r`
— also passed, and it is an **equivalent mutant, not a coverage gap**: at
ML-DSA-44, `k == l == 4`, so the two expressions are the same number. That index
is undiscriminable at this parameter set and would need ML-DSA-65/87 to test.
Recorded rather than papered over.

## 6. Proof

**Against the oracle, before any gate.** The host harness links the *shipped*
`mldsa.c` + `keccak.c` and reproduces `tools/ci/mldsa_ref.py` **byte-exactly**
on a seed that is in no vector file (`00 01 02 ... 1f`), pk and sk both. The
oracle in turn reproduces ACVP tcId 1-5 byte-exactly (DDR-1053). Two independent
confirmations, neither of them the implementation checking itself.

**On the running OS.** `smoke-mldsa` (shard 0, strict) — and per DDR-1041, rc=0
alone is worthless, so the capture was read back: a 431-line boot log carrying

```
[user] ELF loaded (embedded); FIPS 204 ML-DSA-44 keyGen probe spawned
PRADYOS_MLDSA44_KEYGEN_OK acvp=2 p2r=10
```

The counts are **reported by the probe**, via `mldsa44_kat_count()` /
`mldsa44_p2r_count()`, not printed as literals — a gate asserting a hard-coded
`acvp=2` would keep passing after the vector table shrank. `mldsa.c` also
carries a `_Static_assert` on the table size, so a regenerated `mldsa_kat.h`
with a different vector count fails the **build** rather than silently reducing
coverage.

**Mutants on the SHIPPED KERNEL, not merely the host build** (DDR-1052's
standard — proving the gate, not just the code):

| | kernel | gate | capture |
|---|---|---|---|
| clean | `bd921648b60ae930` | rc=0 | sentinel present |
| M1 (`zeta` 1753 -> 1754) | `8e7a4ac795c9e71f` | **rc=1** | `PRADYOS_MLDSA_STUB first_bad=1 arm=acvp_kat` |
| M3 (`>` -> `>=`) | `4923b1e5d7af82de` | **rc=1** | `PRADYOS_MLDSA_STUB first_bad=-4 arm=power2round` |

Reverting returns `bd921648b60ae930` **bit-for-bit**. The two mutants land on
**different arms**, and the sign of `first_bad` says which — which is the whole
point of §5.

Two further host-build mutants also failed as intended: dropping the `(k, l)`
domain-separation bytes from the seed hash (the change FIPS 204 made in its
final version — it yields a self-consistent WRONG key that only a real vector
catches), and swapping the `s, r` byte order in the matrix expansion seed.

## 7. Measurements

| | value |
|---|---|
| kernel | `bd921648b60ae930` |
| `kernel.bin` | 1,204,618 -> **1,229,194 B** against the 1,572,864 B gate |
| `mldsatest.elf` | 24,408 B |
| `sizeof(mldsa44_scratch)` | 21,288 B |
| build | `make image` rc=0, **zero warnings at `-Werror`** |
| host build | also clean at `-Wall -Wextra -Werror` |
| `hygiene_check.sh` | ALL SIX, incl. `ci-probe-rodata-check` on the new probe |
| gate | `smoke-mldsa`, shard 0, **strict** |

## 8. Next

`sigGen`/`sigVer` vectors are reachable from the same ACVP source (5.0 MB and
3.1 MB, not yet fetched). The application named in CLAUDE.md §PHASE 3 is an
ML-DSA-signed tamper-evident ledger on top of F#76's audit chain — which needs
signing, so it is blocked on that and on nothing else.

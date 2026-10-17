# DDR-1059 — the ML-DSA-signed audit ledger: ASSESSED and NOT BUILT, blocker named

**Status:** ASSESSED, deliberately NOT BUILT. The DDR-1038 shape.
**Date:** 2026-09-03
**Branch:** `dev/phase1-seyp3n`

CLAUDE.md §PHASE 3 names three candidate applications for post-quantum
cryptography, first among them an **ML-DSA-signed tamper-evident ledger** on
F#76's audit chain. The primitives are now complete and gated (DDR-1054 keyGen,
DDR-1057 sign, DDR-1058 verify). This is the assessment of the application, and
§PHASE 3's own instruction is what it follows: *"If it genuinely cannot be built
to that bar in the time available, say so explicitly and name the exact
blocker."*

---

## 1. Everything that is usually the blocker, is not

Each of these was measured, not assumed, so that none of them can be offered as
the reason later:

| candidate blocker | measured | verdict |
|---|---|---|
| the crypto isn't built | keyGen/sign/verify all gated, byte-exact vs ACVP | **not a blocker** |
| too slow | keygen **0.26 ms**, sign **0.39 ms**, verify **0.27 ms** (host) | **not a blocker** |
| kernel too big | `mldsa.o` ~60 KB text; `kernel.bin` has **294,518 B** headroom | **not a blocker** |
| wrong shape | a hash chain needs ONE signature over the head, not 4096 | **not a blocker** |

That last row is worth stating plainly because it removes the obvious objection:
signing every entry would be absurd (2420 bytes of signature per 64-byte entry),
but nobody would. `chain[i] = SHA-256(chain[i-1] || fields)` already binds the
whole log into one value, so signing **the head** costs a single signature and is
the standard construction.

## 2. The actual blocker: there is no key custody

**A signature is worth more than a hash only if the private key is unavailable to
the adversary and the verifier learns the public key independently of the
artefact being verified.** This system satisfies neither.

- **No hardware root of trust.** `grep` over `kernel/` and `boot/` for
  TPM / PCR / secure-boot finds nothing. There is no measured boot to anchor a
  key to.
- **The one signing key in the tree is a compile-time constant.**
  `kernel/syscall/sys_vault.c:22` holds `g_owner_seed` as 32 literal bytes, and
  `ags_sign` — the existing goal-signing path — signs with it. **Anyone holding
  the ISO holds the private key.** The comment there is careful and correct
  about why the seed must not be a ring-3 parameter; it does not, and does not
  claim to, make the seed secret from someone with the image.
- **No protected persistent store.** The vault (DDR-834) encrypts credentials at
  rest, but its `K_vault` is HKDF'd from that same in-image seed, so it inherits
  the property rather than fixing it.

So an ML-DSA-signed ledger built today would be signed by a key the adversary
already has. They could edit the log, recompute the SHA-256 chain, re-sign the
new head with the key from the image, and the result would verify. **The
signature would add no assurance over the existing hash chain, while looking
considerably stronger.**

That is security theatre, and it is the exact failure mode this project has a
name for: a control that cannot see the case it exists for (DDR-1046), which
reads exactly like a control that works.

### 2.1 And this also settles a smaller idea

"Swap AGS's Ed25519 for ML-DSA so the existing signing path is post-quantum" is
appealing and equally empty **for the same reason**: if the key ships in the
image, an adversary does not need to break Ed25519, quantum computer or not.
Post-quantum resistance of the *scheme* is irrelevant when the *key* is public.
Recorded so it is not proposed as a consolation prize.

## 3. What would unblock it — three routes, cheapest last

1. **A hardware root of trust.** TPM + measured boot, PCR-sealed key. Correct,
   and an entire subsystem this OS does not have.
2. **First-boot key generation into a protected store.** Needs a store the
   adversary model excludes, which is exactly what §2 says does not exist. This
   is circular until (1) or a platform equivalent lands.
3. **Out-of-band publication of the public key — a DEPLOYMENT story, not a
   kernel feature.** Generate the keypair at install time, publish `pk` once to
   somewhere off the machine, keep `sk` in the image. The ledger is then
   verifiable by anyone holding that `pk` **against an adversary who can edit the
   disk but did not have the image at install time.** That is a narrower claim
   than "tamper-evident ledger", and it is honest, and it is buildable in an
   afternoon once someone decides the threat model.

**Route 3 is a decision, not a task**, which is why it is not being taken here:
it is a security-posture change, the same class as DDR-793's cloud bridge, and
this project defers those to the operator rather than inventing them on the last
day of a release.

## 4. What IS true, and should be said in the release notes

- PRADYOS implements **FIPS 204 ML-DSA-44** — keyGen, sign and verify — and each
  is checked byte-exactly (or verdict-exactly) against **NIST's own ACVP
  vectors**, gated on every CI run.
- The audit chain is **tamper-evident** via a recomputable SHA-256 chain
  (DDR-842), gated by `smoke-auditchain` and `smoke-auditchain-tamper`.
- The two are **not connected**, and this DDR is why.

Claiming "post-quantum signed audit ledger" would be false. Claiming
"post-quantum signature primitives, NIST-vector-verified, and a tamper-evident
audit chain" is true and is what the notes should say.

## 5. What is NOT claimed here

- Not that the application is unimportant — it is §PHASE 3's first-named one.
- Not that it is hard. §1 shows every mechanical cost is small.
- Not that `g_owner_seed` is a defect. It is appropriate for what AGS does
  under a trust model where the image is trusted; it is simply not a foundation
  for a ledger whose adversary is someone who can edit that image.

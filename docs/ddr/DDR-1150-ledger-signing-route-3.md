# DDR-1150: signing the audit-chain head with a per-install ML-DSA key (DDR-1059 Route 3)

**Status:** DESIGN, committed before any code (§NON-NEGOTIABLE 5).
**Decision:** operator decision 4 in PR #17 comment 5845610518 (OWNER):
*"build Route 3 — publish the ML-DSA public key out-of-band at install time,
keeping the private key in the image as today … Do not build routes 1 or 2."*
**Parents:** DDR-1059 (the assessment and the three routes), DDR-842 (the
SHA-256 chain), DDR-1054/1057/1058 (the ML-DSA-44 primitives, ACVP-gated), and
DDR-1143 (the installer, pieces 4–6 not yet built).

## §1 The one reading that matters: "the image" is the installed image, not the ISO

The operator's sentence has two readings. They differ in the only property
Route 3 exists to provide.

- **Reading A: a build-time key in the distributed ISO.** This is DDR-1059 §2
  exactly. Every copy of the ISO carries the same `sk`, so everyone who
  downloaded it can re-sign an edited log. The Route 3 claim ("an adversary …
  who did not have the image at install time") is then true only of someone
  who never obtained a public download. That is no one, so the claim is
  **false**.
- **Reading B: a key generated at install time from real entropy and stored
  in the installed system.** Every install has its own `sk`. The public ISO
  holds none. The installer prints `pk` exactly once, and the operator records
  it off the machine. **This is what DDR-1059 §3 route 3 said in its own
  words:** *"Generate the keypair at install time, publish `pk` once to
  somewhere off the machine, keep `sk` in the image."*

**Reading B is built.** Operator decision 4 also says *"always choose whatever
is structurally best for THIS operating system's own real architecture."*
Reading A is the security theatre DDR-1059 §2 named, so it is not a tenable
reading. This is reported back rather than decided silently.

## §2 The claim, stated at its real size (it goes into the release notes verbatim)

**Defends against:** an adversary who edits the installed disk **after**
signatures have been exported, **and does not hold this install's signing
key.** The attacker can alter log entries, recompute the SHA-256 chain, and
replace the whole key file with one of their own. Any signature checked
against the **out-of-band `pk`** then fails.

**Does NOT defend against:** an adversary who can **read** the installed
disk's key file. The signing key lives on that disk (§3.3). Anyone who reads it
can sign whatever they like. The signature adds nothing against someone with
root access to the installed disk. Of DDR-1059's routes, only Route 1 (a TPM)
closes that gap, and it is not built.

**What it adds over the bare SHA-256 chain:** the chain alone has **no
anchor**. An editor recomputes it end to end and `SYS_VERIFY_AUDIT` returns
0. A signed head is tied to a key that was published before the edit, so a
recomputed chain cannot produce a signature that the published `pk` accepts
**unless the editor also holds `sk`.** That is the whole gain, and it is real
only because `pk` lives off the machine.

## §3 Design

### §3.1 Kernel: `kernel/aether/ledger.c`

- The keypair is held in **kernel memory only**. `sk` is **never** copied out
  to ring 3, by any path.
- Key material is the **32-byte seed**, not the 2,560-byte `sk`: `sk` is
  re-derived by `mldsa44_keygen(seed)`. That makes the stored secret 32 bytes,
  which fits comfortably under every SFS write ceiling (DDR-1100).
- The sign scratch (~54 KiB) and keygen scratch come from the PMM, allocated
  once on the first key operation. `.bss` would add them to every boot's
  footprint for a feature most boots do not use.
- `mldsa.c` joins the **kernel** build. Today it is compiled only into the
  ring-3 probe. DDR-1059 §1 measured `mldsa.o` at about 60 KB against what is
  now 192,118 B of headroom. The size/headroom pair is recomputed in the same
  commit that lands it.

### §3.2 One syscall, NSI 106 `SYS_LEDGER`, sovereign-only

NSI 105 stays reserved for `SYS_INSTALL` (DDR-1143 §10.6). Every subcommand
checks `is_sovereign` first and returns `-EPERM` otherwise. An agent must
never sign or re-key the ledger that records it.

| op | Effect | Refusals |
|---|---|---|
| `LEDGER_KEYGEN` | 32 seed bytes from `rng_bytes` (fails closed, DDR-816), keygen, key held. Returns 0. Copies the **seed** out to a sovereign caller-supplied buffer, so the installer can persist it (§3.3). | `-EEXIST` if a key is already held; `-EIO` if `rng_bytes` fails. **There is no fallback to `g_owner_seed` or to any constant.** That fallback *is* Reading A. |
| `LEDGER_LOAD` | Load a 32-byte seed (from the installed key file at boot). | `-EEXIST`; `-EINVAL` on a bad length. |
| `LEDGER_PUBKEY` | Copy out the 1,312-byte `pk`. | `-ENOKEY` if no key is held. |
| `LEDGER_SIGN` | Snapshot `(g_written, chain head)` under `g_audit_lock`, run `aether_audit_verify()`, then sign. Copies out the message and the 2,420-byte signature. | `-ENOKEY`; **`-EBADMSG` if the chain fails verification.** Signing a chain already known to be tampered would put this install's signature on the tampering. |

**The signed message is fixed-format and self-describing:**
`"PRADYOS-LEDGER-v1\0"` (18 bytes), then `written` (u64 LE), then the chain
head (32 bytes), then `head_seq` (u64 LE), for 66 bytes in all. The domain
string keeps these signatures from being reinterpreted as anything else signed
with this key.

**The seed copy-out is the one deliberate exception to "never leaves the
kernel"** and it is stated here: the installer must write the key to the
target disk. It is a sovereign-only return from the call that created the
seed, and no other op returns it.

### §3.3 Persistence rides on the installer (DDR-1143 piece 5, not yet built)

The installer calls `LEDGER_KEYGEN` and writes the returned seed to
`/etc/aether/ledger.seed` on the **target** SFS. It then prints `pk` as hex
plus its SHA-256 fingerprint and tells the operator to record it off the
machine. At boot on an installed root, the daemon reads the seed and calls
`LEDGER_LOAD`.

Until the installer lands (task #11), a live/ISO boot holds **no key**, and
`LEDGER_SIGN` returns `-ENOKEY`. **That is correct behaviour, not a gap:** a
live medium has no install to anchor.

### §3.4 Export and verification are off-box by construction

`tools/ci/ledger_verify.py <pk-hex-file> <capture>` parses
`PRADYOS_LEDGER_MSG`/`PRADYOS_LEDGER_SIG` lines and verifies them with
**`tools/ci/mldsa_sign_ref.py`'s `verify_internal`**. That is an independent
Python implementation (DDR-1057/1058), not the kernel's code. The kernel
signing and the kernel then verifying its own signature would be the dead-arm
class, since any self-consistent wrong implementation passes. The verifier
also **re-checks the message format** (domain string, length) before it
trusts any field.

## §4 Gate `smoke-ledger`, and why each arm is not vacuous

A probe `user/ledgertest.c` stands in for the installer (it is sovereign and
performs the KEYGEN the installer will perform). Two boots:

- **A: host verify PASSES with the printed `pk`.** The Python verifier, not
  the kernel, accepts both signatures.
- **B: the signature binds the chain, not a constant.** The probe signs, then
  causes an audited event (a refused `SYS_SET_MODE` from a non-sovereign fork
  writes `AR_CAP_DENIED`), then signs again. The host requires `written2 >
  written1` **and** `head2 != head1`, and both must verify. A kernel that signs
  a constant or zero head passes A and fails B.
- **C: the host rejects a tampered message.** Flip one head byte and verify
  must fail. This proves the **verifier** can say no. Without it, A passes on
  a verifier that returns True.
- **D: refusals, exact values.** A second `KEYGEN` returns `-EEXIST`; a
  non-sovereign child gets `-EPERM` on `PUBKEY` and on `SIGN`; `SIGN` before
  `KEYGEN` returns `-ENOKEY` (the probe signs first).
- **E: the key is per-keygen, not image-baked.** A **second boot** must print
  a **different** `pk` fingerprint. **This is the arm that catches Reading A**:
  a kernel seeding from `g_owner_seed` or any constant passes A–D and
  produces the identical `pk` on both boots. It costs a second boot, and that
  is the price of the one property Route 3 exists for.

**Mutants, each aimed at a different arm:**

| Mutant | Change | Expected to fail |
|---|---|---|
| M1 | Seed from `g_owner_seed` instead of `rng_bytes` | E alone |
| M2 | Sign a zeroed head | B |
| M3 | Python verifier with a stub that always returns True | C |
| M4 | Drop the `is_sovereign` check on `SIGN` | D |

## §5 Cost, recorded before measurement

- **Signing runs with IF clear** (`SYSCALL` masks it, `syscall.c:229`), the
  same shape as DDR-1034's bounded interpreter. The rejection loop is bounded
  at 1,000 iterations, with an expected count of about 4.25 (DDR-1057). The
  host measured 0.39 ms; the TCG cost is measured in the gate rather than
  guessed.
- If the cost is in the tens of milliseconds, that is a timer-starvation
  window on a kernel whose open defect (OPEN-2) is timing-sensitive. The
  number is recorded, and **this commit is not exonerated in advance**
  (DDR-1042).
- `SIGN` is sovereign-only and on-demand, not per-append.

## §6 Not claimed

- **Not "a tamper-proof ledger."** §2 states the boundary. A reader of the
  installed disk can forge.
- **Not Route 1 or Route 2.** Both are not built, per the decision.
- **No per-append signing.** The head is signed on demand, and export
  frequency is operational policy.
- **Until DDR-1143 piece 5 lands, no install produces a key.** The gate's
  probe stands in, and it says so.
- **`g_owner_seed` is not touched.** The vault and AGS keep their current
  trust model (DDR-1059 §5).
- **GLOBAL_FORBIDDEN stays at 77.**

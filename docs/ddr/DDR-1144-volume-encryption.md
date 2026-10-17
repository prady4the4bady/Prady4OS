# DDR-1144 — Volume encryption: crypt block device, keyslots, unlock

**Status: DESIGN. Committed before code (NON-NEGOTIABLE 5).** Part of the track
DDR-1143 opens (PR #17 comment 5839562349). The size, risk and operator
decisions live in DDR-1143 §6 and §8.

## §1 Reuse, and the one thing reuse cannot give

The operator asked to reuse the in-tree ciphers and not re-implement
primitives. Available (DDR-1104 §1 lists them, measured in `kernel/crypto/`):

- ChaCha20-Poly1305 AEAD (`aead.h`, RFC 8439, DDR-819), with
  `chacha20_stream` exposed;
- HMAC-SHA256 and HKDF-SHA256 (`hkdf.h`, DDR-818);
- SHA-256 and SHA-512, X25519, Ed25519, SHA-3/SHAKE, ML-DSA;
- `rng_bytes` (`rng.h`, DDR-816), which **fails closed**: virtio-rng or RDSEED,
  with no timing fallback.

(A small correction to the instruction's citation: DDR-1059 is the ledger
key-custody assessment. The primitive list is DDR-1104 §1.)

**None of these is a length-preserving, tweakable block cipher.** There is no
AES and so no XTS, and nothing wide-block. The usual disk-encryption shape
(dm-crypt/XTS, BitLocker: sector in, sector out, keyed by sector number) is
therefore **not available by reuse**. The obvious substitute is wrong, and the
reason is recorded so nobody builds it:

> **Refused: ChaCha20 with nonce = sector number.** Every overwrite of a sector
> reuses the same keystream. An attacker holding two images of the disk taken
> at different times, or one image plus knowledge of either version, recovers
> `P_old XOR P_new`. SFS reuses freed blocks (exact-fit free runs, §INV.21) and
> overwrites its superblock and journal in place, so this is the normal case,
> not an edge case. It is also fully malleable: a bit flipped in the ciphertext
> flips the same bit in the plaintext. It would **read as confidentiality and
> provide much less**, which is the DDR-1059 shape.

**Chosen: per-block AEAD with a stored nonce and tag**, in the manner of
dm-crypt authenticated mode with dm-integrity metadata. It reuses
`aead_seal`/`aead_open` unchanged.

## §2 Construction

- **Unit:** one 4,096-byte block, which is SFS's block size, so SFS sees a
  block device of logical 4 KiB blocks, unchanged.
- **Per write of logical block `b`:**
  - `nonce` is 96 random bits from a per-volume DRBG: `chacha20_stream` keyed
    by 32 bytes from `rng_bytes` at unlock, reseeded every 2^20 blocks. That is
    reuse again, and it avoids calling virtio-rng once per block.
  - `K_b = HKDF-SHA256(VMK, salt=volume_uuid, info="prdy-blk-v1"‖b)`. A
    per-block subkey means a random-nonce collision only matters **within one
    block's own write history**, not across the whole volume. That turns the
    birthday bound from a volume-lifetime concern into a non-issue. Cost:
    HKDF is 4 SHA-256 compressions per block I/O, beside 64 ChaCha20 blocks
    plus Poly1305 over 4 KiB. That is a stated cost, to be **measured** as a
    static count per DDR-870's convention, not quoted as a speedup.
  - `aad = volume_uuid ‖ b ‖ header_generation`. **Binding the block number
    detects relocation.** A block copied to a different position fails
    authentication.
  - Store the ciphertext in the data area. Store the `(nonce, tag)` entry,
    28 B, in the metadata area.
- **Read:** `aead_open` verifies the tag **before** writing plaintext
  (`aead.h` property 1). A failure returns **`-EIO`**, never bytes.
- **Layout on P2:**
  - header copy A, then header copy B, then groups of
    `[1 meta block][146 data blocks]`;
  - 146 × 28 = 4,088 ≤ 4,096;
  - overhead is 1/147 ≈ **0.68 %** of capacity;
  - interleaving keeps each metadata block on the same track as its data.

### §2.1 What this does and does not protect, stated at its real size

| Property | Provided? | How |
|---|---|---|
| Confidentiality of data at rest | **Yes** | AEAD with a unique nonce per write |
| Detection of modification | **Yes** | Poly1305 tag. A modified block returns `-EIO`. |
| Detection of relocation or swap | **Yes** | `b` is in the AAD |
| Detection of **rollback** of a block to an older valid version | **NO** | A block and its metadata entry replaced together with an earlier genuine pair still authenticates. Closing that needs a Merkle root or a TPM monotonic counter over the volume. **Named, not built.** |
| Hiding which blocks changed | **No** | Nonces change on write, so an observer with two snapshots sees which blocks were written. The same is true of every sector-level scheme. |

### §2.2 Crash consistency, which the construction makes harder

A data block and its metadata entry live in different sectors, so a crash can
leave new data with old metadata. On read that is an **authentication failure,
which is detected**, not silent corruption.

- For SFS's **out-of-place** blocks this is harmless. An unfinished write goes
  to a block that the root does not reference until the journal commits.
- For the **in-place** blocks (the superblock and the journal record), a torn
  write would make the volume unmountable.
- So the design requires SFS to write those two **A/B alternately by
  generation**. The superblock already carries a generation, and mount picks
  the newest copy that authenticates.

This is an SFS change, and it is listed as part of this DDR, not hidden.
Ordering depends on DDR-1143 §4.2's flush barriers.

## §3 Header and keyslots

- **Header** (plaintext, 4 KiB, two copies):
  - magic `PRDYCRY1`, version, `volume_uuid` (128 random bits; this is also the
    DDR-1146 device id), `header_generation`, geometry, KDF parameters;
  - **four keyslots**;
  - `hdr_mac = HMAC(K_hdr, header)`, with `K_hdr = HKDF(VMK, "prdy-hdr-v1")`.
    After unlock, tampering with the header is detected. Before unlock the
    header is untrusted input, bounds-checked field by field. It is parsed,
    never followed.
- **VMK:** 256 random bits from `rng_bytes`, generated once at install. It
  never changes, so rotating a passphrase or recovery key re-wraps rather than
  re-encrypting the disk.
- **Keyslot** `{type, salt[32], kdf params, wrap_nonce[12], wrapped_vmk[32], tag[16]}`,
  with `aad = volume_uuid ‖ slot_index ‖ type`:
  - **P (passphrase):** `KEK = KDF(passphrase, salt)`, where KDF is decision
    D5 (§4);
  - **R (recovery):** `KEK = HKDF(RK, salt, "prdy-rk-v1")`. RK has 256 bits of
    entropy, so no slow KDF is needed (DDR-1146);
  - **T (TPM):** `KEK` is a 32-byte secret sealed in the TPM. The slot also
    stores the sealed object's public and private blobs (DDR-1145);
  - one slot spare, for re-seal or rotation without a window where no slot
    exists.
- **Wrong passphrase:** the AEAD tag on the keyslot fails, and unlock returns
  exactly **`-EACCES`**. There is no separate "check value", which would be a
  second oracle.

## §4 Passphrase KDF, decision D5

Two options:

- **PBKDF2-HMAC-SHA256.** It is a *composition* of HMAC, already in tree, not a
  new primitive. It is **not memory-hard**, so a GPU attacker pays far less per
  guess than we do.
- **Argon2id (RFC 9106).** Memory-hard, but it needs **BLAKE2b, a new
  primitive**. It would be built to the ML-DSA bar: RFC vectors pinned
  byte-exact, a Python oracle first, mutants on the round constants and on the
  memory-block indexing.

A cost that bites either way: **under TCG a work factor sized for real hardware
is slow.** PBKDF2 at 600,000 iterations is ~1.2 M SHA-256 compressions. The
gate must run the **shipping** parameters; a lowered test value is DDR-1040's
vacuity trap. So the gate's time budget must absorb it. That is to be measured
before the gate is sized.

**Recommendation:** Argon2id if the schedule allows, otherwise PBKDF2 with the
weakness named in the release notes. This is the operator's call (DDR-1143
§8 D5).

## §5 Unlock flow at boot

1. Kernel boot reaches the point where DDR-972 would create ramdisks. DDR-1143
   §4.6 detects an installed layout.
2. If a TPM slot exists and a TPM is present, try unsealing (DDR-1145). On
   success, derive the VMK and go to step 5.
3. Otherwise, or on any TPM failure (absent, cleared, PCR mismatch, lockout),
   spawn the embedded **`unlock` ELF**. It is 8 KiB in the kernel, because the
   root is not mounted yet.
   - It prompts on the console and reads the passphrase **without echo**. It
     also accepts `recovery` and then a recovery key.
   - It calls **`SYS_VOL_UNLOCK` (NSI 105)** with it.
   - The kernel runs the KDF, tries the matching slots, and **zeroes the
     passphrase buffer** on every return path.
4. Three failures in a row make each further attempt wait an increasing delay.
   That is honest about its limit: it slows a person at the keyboard and does
   nothing against an offline attack on the header, which is why the KDF
   matters.
5. Register the crypt device, mount SFS from it, and set it as the default
   root. Boot continues exactly as today.
6. **After a passphrase or recovery unlock on a TPM-equipped machine, offer to
   re-seal.** This is how a firmware or kernel update avoids stranding the
   TPM path (DDR-1145 §3).

## §6 Gate: `smoke-crypt`, vacuity checked first

It builds on `smoke-install`'s two boots, with host-side inspection of the raw
disk image between them. Arms:

| Arm | Assertion | The vacuous version it replaces | Mutant that must fail it, and only it |
|---|---|---|---|
| C | A 32-byte random marker, generated by the guest and printed, is written to a file. The host searches the raw image for it: **absent**. Boot 2 reads it back through SFS: **present**. | Absence alone passes if the file was never written. | M1: identity cipher |
| W | Wrong passphrase gives exactly `-EACCES`. The correct one unlocks. | "Boots" | M2: accept any passphrase |
| T | The host flips one byte inside a data block. Reading that file gives exactly `-EIO`, **not** altered bytes. | "Read fails" (any errno) | M3: skip tag verification (returns altered data) |
| S | The host swaps two data blocks together with their metadata entries. Both reads give `-EIO`. | — | M4: AAD omits `b` (the swap reads back silently) |
| N | A self-test rewrites one block 64 times and prints the nonces, which must all be distinct. The host confirms the metadata entry changed on disk. | "Encrypts" | M5: nonce derived from `b` (C still passes, which is **why N exists**) |
| R | The recovery key unlocks when the passphrase is withheld. | — | M6: R slot not written |

Every expected value is exact (DDR-1044), and each mutant is built to land on
one arm (DDR-1042).

## §7 Not claimed

- **Rollback of individual blocks is NOT detected** (§2.1).
- **Constant-time behaviour** of the tag compare is enforced by construction
  and review, as `aead.h` already records. It is **not gate-testable**.
- **No power-loss durability claim** (DDR-1143 §7).
- **No implementation exists.**

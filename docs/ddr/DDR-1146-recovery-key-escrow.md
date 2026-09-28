# DDR-1146 — Per-device recovery keys and operator escrow

**Status: DESIGN. Committed before code (NON-NEGOTIABLE 5).** Part of the track
DDR-1143 opens (PR #17 comment 5839562349, item 3). Size, risk and the operator
decisions live in DDR-1143 §6 and §8.

The instruction's hard constraint, quoted so every section below can be checked
against it: *"Do not implement any single key, credential, or login that can
unlock more than one device."*

## §1 What is built in the OS, and what cannot be

| Piece | Where it lives | Built here? |
|---|---|---|
| Recovery key (RK) generation, one per device | installer (DDR-1143) | **Yes** |
| Keyslot R, wrapping the VMK under RK | volume header (DDR-1144 §3) | **Yes** |
| Escrow record: RK encrypted to the escrow public key | installer output | **Yes** |
| Using an RK at boot | `unlock` ELF, `SYS_VOL_UNLOCK` (DDR-1144 §5) | **Yes** |
| Rotating RK and passphrase after a recovery unlock | OS, first boot after recovery | **Yes** |
| Device-side audit of recovery events | persistent audit (§5) | **Yes, with a named dependency** |
| Escrow database, its storage and backups | operator infrastructure | **No — external** |
| Escrow private key custody (HSM, threshold) | operator infrastructure | **No — external** |
| Office identity and ownership verification | a human process plus operator software | **No — external** |
| Office-side audit log of releases | operator infrastructure | **No — external** |

**A kernel cannot verify a human's identity or proof of purchase.** Anything in
the OS claiming to would be a fake, which the instruction forbids. The OS side
ends at "here is an encrypted record" and "this RK unlocks this volume". The
office side is a separate system, specified in §4 so it can be built, not built
here.

## §2 Device side

- **RK:** 256 bits from `rng_bytes` (fails closed, DDR-816). Shown to the user
  once at install as 64 hex characters in groups of 8 with a check group, so a
  mistyped key is caught before any keyslot is tried. The user is told to
  record it; it is never written to disk in plaintext.
- **Device id:** `volume_uuid` (DDR-1144 §3), 128 random bits. It identifies a
  record; it is **not** a secret and grants nothing.
- **Keyslot R:** `KEK = HKDF(RK, salt, "prdy-rk-v1")`, wrapping the VMK
  (DDR-1144 §3). RK has full entropy, so no slow KDF.
- **Uniqueness is by construction:** RK, VMK and `volume_uuid` are fresh random
  values per install. No value is derived from any key shared across devices.
  In particular **nothing uses `g_owner_seed`**, the 32 literal bytes compiled
  into every image (`sys_vault.c:22`, DDR-1059). A recovery scheme keyed from
  that seed would be a universal master key shipped in every ISO, which is
  exactly what the instruction forbids.

### §2.1 Escrow record (reused primitives only)

An ECIES-style sealed box to a compiled-in **escrow public key** `EPK`
(X25519), built only from `x25519.h`, `hkdf.h` and `aead.h`:

1. Generate an ephemeral X25519 key pair `(e, E)`.
2. `S = X25519(e, EPK)`; reject the all-zero result.
3. `K = HKDF-SHA256(S, salt = E ‖ EPK, info = "prdy-escrow-v1")`.
4. `ct = AEAD(K, nonce = 0, aad = "prdy-escrow-v1" ‖ volume_uuid ‖ created_at ‖ install_nonce, RK)`.
   A zero nonce is safe because `K` is single-use (fresh `e` per record).
5. Record = version ‖ `volume_uuid` ‖ `created_at` ‖ `install_nonce` ‖ `E` ‖ `ct` ‖ tag.
6. Wipe `e`, `S`, `K`.

`volume_uuid` is inside the AAD, so a record cannot be relabelled to another
device: the office decrypts, and the AAD fails unless the claimed device id
matches.

**Transport (decision D2):** v1 exports the record as text (base64 in fixed
lines) shown at install and written to the ESP as `ESCROW.TXT`. The ESP is
unencrypted, but the record is ciphertext to `EPK`, so it reveals nothing
without the escrow private key. Network upload is **not** in v1: it needs TLS,
which needs a trust anchor this tree does not have (DDR-1104 §1). Named, not
built.

### §2.2 After a recovery unlock

Releasing an RK means a person at the office has seen it. So the first boot
after an R-slot unlock **forces rotation**:

- new passphrase (new P slot);
- new RK, new R slot, new escrow record (the old record is then useless);
- TPM re-seal if applicable (DDR-1145 §3).

The VMK stays; nothing is re-encrypted. The spare slot (DDR-1144 §3) makes the
swap atomic: write the new slot, then invalidate the old one, with no moment
where no slot exists.

## §3 The master-key tension, decision D1, stated plainly

The operator asked for per-device keys **and** an operator escrow able to
release any device's key after verification. Those pull in opposite
directions. Whatever decrypts escrow records is, in effect, a credential that
can recover every escrowed device. Per-device RKs remove the *on-device*
master key; they do not remove the *escrow-side* one. Pretending otherwise
would be the DDR-1059 shape.

What can be done is to make that credential never usable alone:

| Option | Single point that can recover every device? | Cost |
|---|---|---|
| (a) One escrow key pair, private key in an HSM that refuses bulk export and logs every decrypt | Yes, the HSM (a controlled one) | Simplest. The HSM's policy and log are the control. |
| (b) **k-of-n threshold** escrow key (for example 2-of-3 custodians) | No single person | Needs a threshold scheme at the office. The device side only changes if the record is split per custodian. |
| (c) Per-distributor escrow keys (a region or batch gets its own `EPK`) | Limited to one batch | Needs per-build `EPK` selection; reduces blast radius, does not remove it. |
| (d) Owner-bound share: RK split so one share is escrowed and one printed for the owner | No, for the operator alone | Recovery then needs the owner's share, which defeats the "lost everything" case. |

**Recommendation: (b) combined with (c).** Threshold custody at the office,
per-distributor escrow keys in the image. The device-side work for (a), (b) and
(c) is identical (one `EPK` per build), so this decision does not block device
code. (d) changes the device side and should be decided before building.

**Also for the operator:** `EPK` is compiled into the image. Rotating it needs a
new build; devices already installed keep records to the old key. An escrow
private key must therefore be kept for the lifetime of every device installed
under it.

## §4 The office flow, specified, not built

1. The owner presents the device (or its device id) and proof of ownership and
   identity. **What counts as proof is the operator's policy** (decision D3):
   receipt, account, government ID, or a combination.
2. The office looks up the escrow record by `volume_uuid`.
3. The office decrypts under the custody rule of D1. It verifies the AAD
   against the device id the owner presented.
4. It releases **only that RK**, preferably read aloud or shown once, not
   emailed.
5. It appends to the office audit log: device id, time, staff, custodians,
   evidence reference, outcome.
6. The device forces rotation at next boot (§2.2), so the released RK is dead.

A host-side reference tool (`tools/escrow/`, Python, one sealed-box open plus
AAD check) may be supplied so the format is testable end to end. It is a
**format reference, not the office system**: it has no identity check, no
database and no custody logic, and is labelled so.

## §5 Device-side audit of recovery events

The request says to reuse the audit-chain pattern. The in-kernel chain
(DDR-842) is **in-memory and circular**, and F#76 durability is unbuilt
(DDR-1095 §3, DDR-1098 §4). A recovery event recorded only there vanishes at
reboot, which is exactly when it matters.

So the recovery audit is a small **append-only log on the encrypted volume**
(`/etc/recovery.log`), hash-chained like DDR-842 (each entry carries
`SHA-256(prev ‖ fields)`), with one entry per:

- R-slot unlock (time, slot, outcome);
- forced rotation (new slot ids, new record fingerprint);
- failed recovery attempt (count only, never key material).

It is written on the unlocked volume, so it is confidential and tamper-evident
within the volume's rollback limit (DDR-1144 §2.1: whole-volume rollback is
not detected, and neither is truncation to an earlier chain head). The
append-extent limit (DDR-1098 §4, DDR-1100) applies: four appends per file for
the file's life. So the log rotates into a new file per generation, or waits on
the SFS extent work. This is a stated dependency, not hidden.

This log is **not** the office audit (§4 step 5). The office log is the
authoritative record of releases; the device log records what happened on the
device.

## §6 Gate: `smoke-recovery`, vacuity checked

| Arm | Assertion | Mutant that must fail it, and only it |
|---|---|---|
| U | Install prints an RK. Boot 2 with the passphrase withheld and that RK entered unlocks and reads back the DDR-1143 marker. | M1: R slot not written |
| D | Two installs produce two RKs and two `volume_uuid`s. **Install A's RK against install B's disk gives exactly `-EACCES`.** | M2: RK derived from a constant (for example `g_owner_seed`). **The load-bearing mutant**: it passes U and fails D. |
| E | The host reference tool, holding the test escrow private key, opens `ESCROW.TXT` and recovers the same RK that was shown. With the record's `volume_uuid` altered, it fails. | M3: `volume_uuid` omitted from the AAD |
| X | After an RK unlock, the next boot requires a new passphrase and prints a new RK; **the old RK then gives `-EACCES`**. | M4: rotation writes a new slot but does not invalidate the old one |
| L | After arms U and X, `/etc/recovery.log` holds the expected entries and its chain verifies; a host-edited entry fails verification. | M5: chain hash omits `prev` |

A test escrow key pair is generated for CI and clearly marked test-only. The
shipping `EPK` is the operator's and is never in the tree.

Vacuity: "recovery works" (arm U alone) passes on M2, where every device shares
one RK. Arm D is the arm that checks the instruction's hard constraint.

## §7 Not claimed

- No escrow backend, HSM, database, identity check or office audit is built.
  §4 is a specification for an external system.
- The escrow-side key is a credential able to recover every device escrowed to
  it (§3). What is built minimises it; nothing removes it.
- No network upload of escrow records (no TLS trust anchor, DDR-1104).
- Device-side recovery audit does not survive a whole-volume rollback.
- No implementation exists.

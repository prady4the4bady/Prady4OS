# DDR-1153 — The installer persists the ledger seed, and an installed boot reloads it

**Status:** design committed BEFORE the code (NON-NEGOTIABLE 5, `a779eab`); **BUILT + GATED + M9–M12, §7.**
**Date:** 2026-09-27.
**Builds on:** DDR-1150 (the ledger key and `SYS_LEDGER`), and DDR-1143 pieces 4–6 (the installer and the P2 root).
**Closes:** DDR-1150 §3.3's open item, *"key persistence rides on the installer"*. DDR-1143 §10.10 recorded that the installer, as built, does not write the seed.

## §1 What exists, measured

- `SYS_LEDGER` (NSI 106) holds one ML-DSA-44 key.
  - The only stored secret is the 32-byte seed; `sk` is re-derived from it by `mldsa44_keygen`.
  - `KEYGEN` draws the seed from `rng_bytes`, which fails closed.
  - `LOAD` accepts a seed back.
  - `LOAD` has no caller (DDR-1150 §9).
- `SYS_INSTALL` (NSI 105) runs **in the kernel**. It already draws a nonce from `rng_bytes` and fails closed without one.
- The P2 header is written in `write_p2`:
  - LBA 0 of P2 is `PRDYVOL1`.
  - LBAs 1–7 are **reserved, written as zeros**.
  - The SFS volume starts at P2+8.
- Root selection (`disk_root_select`, `kernel/main.c`) already reads the P2 header on an installed disk before mounting.
- `grep` over `kernel/syscall/` finds exactly one ring-3-reachable `blk_read`: `sys_disk.c:38`. It reads LBA 0 to classify a disk and returns flags, not bytes. **No syscall returns raw sector contents to ring 3.**

## §2 Decision: the seed goes in P2 sector 1, not in an SFS file

DDR-1150 §3.3 proposed `/etc/aether/ledger.seed` on the target SFS, read by the daemon, which then calls `LOAD`. That is **rejected here**, for a measured reason.

**A file is readable by every process holding a read capability on that root.** On an installed system, the default process root is the P2 SFS, and every ELF gets `CAP_FS_READ` on it (`elf.c`). An agent could therefore `open` the seed file and read it. `SYS_LEDGER` being sovereign-only would not help: with the seed in hand, the agent signs **off the machine**, and an editor could re-sign an edited log. That is DDR-1150 §2's forgery, reachable from inside the machine rather than only from outside it.

**P2 sector 1 is outside the filesystem.**
- No VFS path names it.
- §1 shows no ring-3 path reads raw sectors.
- Only two things reach it:
  1. the kernel itself;
  2. physical or host access to the disk. That is exactly DDR-1150 §2's stated boundary (*"a reader of the installed disk can forge"*), which this design does not claim to move.

**The installer, not ring 3, generates the seed.** `SYS_INSTALL` calls into the ledger code kernel-side, so the seed never crosses into ring 3 at any point. That is strictly tighter than DDR-1150's copy-out to a sovereign caller. The live session's own ledger state is **not** touched: the installer derives a key for the *target*, it does not take one for the running system. The copy-out path in `KEYGEN` stays for the smoke-ledger probe and is not widened.

**The installed boot loads the seed kernel-side**, in root selection, from the sector it has already found the P2 header next to. No daemon change is needed, and no seed ever sits in a ring-3 buffer.

## §3 Sector 1 layout

| Offset | Bytes | Field |
|---|---|---|
| 0 | 8 | `PRDYSEED` |
| 8 | 4 | version = 1 |
| 12 | 32 | the seed |
| 44 | 32 | SHA-256(pk), where pk is `keygen(seed)` |
| 76 | 436 | zero |

The pk digest is stored so boot can **refuse a seed that does not produce its recorded key**. For example, a torn write yields a seed that no longer matches, and loading it would sign under a key nobody published. On a mismatch, boot prints `[ledger] seed REFUSED` and loads nothing. Failing closed matches `KEYGEN`.

## §4 What prints, and where

**The installer prints the full pk once.** It uses exactly the `PRADYOS_LEDGER_PK n=0 i=<j> <hex>` chunk format `ledgertest` already uses, so `tools/ci/ledger_verify.py` reads it unchanged. This is the value the operator records off the machine (DDR-1150 §3.3). It then prints `[install] ledger fp=<16 hex>`, the first 16 hex characters of SHA-256(pk).

**An installed boot prints `[ledger] loaded fp=<16 hex>`.** It prints `[ledger] seed REFUSED` on a digest mismatch, and `[ledger] no seed` when the magic is absent.

## §5 Gate: extend `smoke-install`, no new gate

The obvious arm, *"boot B prints loaded"*, is **vacuous by itself**. A kernel that loaded any seed, and printed the fingerprint of whatever it loaded, passes it. So the arms tie three independent derivations of one key together:

| Arm | Check | What it rules out |
|---|---|---|
| **K1** (host) | Read P2 sector 1 off the installed image with `dd`. Check magic and version. Re-derive pk from the seed with `tools/ci/mldsa_ref.py`, an implementation that is **not** the kernel's. SHA-256(pk) must equal both the stored digest and boot 1's printed `fp`. | The installer printing one key and persisting another. |
| **K2** (host) | `ledger_verify.py <boot1> fp` (the printed full pk) must equal boot 1's `fp` line. | The printed pk not being the key the fingerprint names. |
| **K3** (arms B and U) | `[ledger] loaded fp=` equals boot 1's `fp`, on BIOS and UEFI. | Loading nothing, loading the wrong bytes, or loading from the wrong sector. |
| **K4** (negative arm) | The foreign-MBR boot prints no `[ledger] loaded`. | Loading from a disk that is not an installed one. |

**A constant-seed mutant is not caught by K1–K4.** A constant key is self-consistent. That case is DDR-1150's arm E on `smoke-ledger`, which needs two keygens; it is **not duplicated** here because it would double this gate's cost. The installer's seed source is `rng_bytes` through the same function `KEYGEN` uses, and a mutant replacing it is recorded as **covered by `smoke-ledger` arm E only if the code is shared**. The implementation therefore routes both through one helper, so that coverage transfers by construction and not by assertion.

**Planned mutants**, each to fail a different arm:

| Mutant | Change | Expected to fail |
|---|---|---|
| M9 | the installer zeroes the seed before writing it | K1 |
| M10 | boot skips the load | K3 |
| M11 | boot reads sector 2 instead of 1 | K3 (as `no seed`) |
| M12 | the load skips the digest check | **K5**: a copy of the installed image with one seed byte flipped by the host must print `[ledger] seed REFUSED`; M12 prints `loaded` instead |

**K5** is one more ~6 s boot, and it is the only arm that proves the digest check exists.

## §6 Not claimed

- **Not a defence against a reader of the disk.** The seed is plaintext in P2 sector 1, exactly as the SFS is plaintext (`INST_VOL_PLAINTEXT`). DDR-1144's encryption and DDR-1145's TPM sealing are what would move this, and both are designs only. A stolen disk can forge. **This goes into the release notes verbatim, as DDR-1150 §2 already does.**
- **Nothing changes for a live/ISO boot.** It still holds no key unless a sovereign `KEYGEN` makes one.
- **No daemon change.** The daemon does not yet call `SIGN` on a schedule; DDR-1150 left that to the operator's publishing workflow.
- **No change to `SYS_LEDGER`'s ABI.**
- `g_owner_seed`, the vault and AGS are untouched.
- No open issue moves.

## §7 Built, gated, mutation-checked (2026-09-27)

**Code.**
- `kernel/syscall/ledger.h` (new) exports three kernel-side calls:
  - `ledger_new_seed`: the **one** seed source, `rng_bytes`, failing closed. `KEYGEN` now calls it too, so `smoke-ledger` arm E's constant-seed coverage transfers **by construction** (§5).
  - `ledger_derive_pk`: derives the *target's* pk. It does not touch the held key.
  - `ledger_load_seed_verified`: keeps the key only if SHA-256(pk) equals the stored digest. Otherwise it forgets the key and returns `-ETAMPER`; `ETAMPER`'s own definition is "record hash failed verification", so no new errno was added.
- `install.c` `write_seed`:
  - writes the §3 sector between the P2 header and the zeroed reserved LBAs;
  - zeroes the seed, the sector and both staging buffers afterwards;
  - prints the pk and `[install] ledger fp=` only after the whole install has succeeded.
- `main.c` `ledger_seed_boot` runs inside `disk_root_select`'s installed branch, on the page it already holds, and zeroes that page before it is freed.

**Gate.** `smoke-install`, extended; no new gate, so there are still 186. Clean kernel `1e94746ee8548cc6`, 1,474,954 B (+4,096). The clean run prints:

```
[install] arm K2 ok (printed pk fp=60410d1b089ae363)
[install] arm K1 ok (seed -> pk fp=60410d1b089ae363, independent keygen)
[install] arm B ok (bios, root=p2, mark=0fefec1fcb5f3862, ledger fp=60410d1b089ae363)
[install] arm U ok (uefi, root=p2, mark=0fefec1fcb5f3862, ledger fp=60410d1b089ae363)
[install] neg ok (foreign MBR: no root selection, no ledger key)
[install] arm K5 ok (flipped seed byte -> REFUSED)
```

**One key, three independent derivations, agreeing:**
1. the kernel's printed fingerprint;
2. `ledger_verify.py` over the printed full pk;
3. `mldsa_ref.py` over the seed bytes `dd`'d off the disk by the host, which is not the kernel's code.

**Mutants.** Each fails exactly its predicted arm, on four distinct hashes:

| Mutant | Hash | Result |
|---|---|---|
| M9 | `0212d9b7e1935a87` | K1: `keygen(seed) does not match the stored digest` |
| M10 | `b1f7ead5135bd13c` | K3, arm B: no `loaded` |
| M11 | `df904d86a1ae0d0e` | K3, and the boot printed `[ledger] no seed` as §5 predicted |
| M12 | `bad06e9ab969a853` | K5: the corrupted image printed `[ledger] loaded fp=bc1646b8e94afac6`, **a key nobody published**. This is exactly the failure the digest check exists to refuse. |

The revert returns `1e94746ee8548cc6` bit for bit, verified by rebuild.

**A tooling trap, recorded.** The mutant script restored each file with `mv` from a backup, which keeps the **older** mtime. `kernel.bin`'s rule depends on source mtimes, so the first "revert" build printed *Nothing to be done* and left **M12's** hash in place. It was caught by hashing, then fixed by `touch` and a rebuild. The mutant runs themselves were sound: each mutation gives its file a fresh mtime, which recompiles every object from the then-restored sources. This is the §INV.10 family again. **Restore with `cp`, or `touch` afterwards; never trust `make` without a hash.**

**Regression**, run with the hash pinned: hygiene ALL TEN; smoke-shell 5/5; `smoke-install`, `smoke-ledger`, `smoke-part`, `smoke-iso-userspace`, `smoke-iso-x86`, `smoke-uefi`, `smoke-blkmq`, `smoke-rqstress-liveness` and `smoke-blk-integrity` all rc=0.

**§6 is unchanged.** It is not a defence against a reader of the disk, and the plaintext seed goes into the release notes verbatim.

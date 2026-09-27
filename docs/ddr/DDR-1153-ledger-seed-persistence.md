# DDR-1153 — The installer persists the ledger seed, and an installed boot reloads it

**Status:** design, committed BEFORE the code (NON-NEGOTIABLE 5).
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

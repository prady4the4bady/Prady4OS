# DDR-1143 — Installer and persistent root (with the size/risk assessment for DDR-1143..1146)

**Status: DESIGN. Committed before code (NON-NEGOTIABLE 5). No implementation
exists.** This is the parent record for the installer/encryption/recovery track
that PR #17 comment **5839562349** (OWNER-verified, 2026-09-25) put in v1.0.0
scope. That comment supersedes two earlier decisions:

- **#40**, the deferral of full-volume encryption, which I had recommended;
- **#19**, "no installer, stated as a known limitation" (decision in
  5822896320).

The track has four DDRs:

| DDR | Scope |
|---|---|
| **1143** (this) | Installer, disk layout, persistent root, boot-from-disk. Also carries the size/risk assessment and the operator decisions for all four. |
| **1144** | Volume encryption: the crypt block device, header, keyslots, passphrase KDF and unlock. |
| **1145** | TPM 2.0: transport, command subset, measured kernel, sealing and unsealing. |
| **1146** | Per-device recovery key and escrow: what the OS can build, and what must be an external system. |

Every fact below was measured in the tree at `219a943`. Nothing is carried from
memory.

---

## §1 What exists, measured

- **Block layer** (`kernel/drivers/blk/blk.h`):
  - `struct blk_device` is `{read, write, capacity_sectors}` over 512-byte
    sectors.
  - Registered by virtio-blk, AHCI (`ahci.c:291`), NVMe (`nvme.c:507`) and the
    ramdisk (`ramdisk.c:86`).
  - **There is no flush or FUA operation** anywhere in `drivers/blk`, `ahci` or
    `nvme` (grep is empty). So no write is guaranteed durable on hardware with
    a volatile write cache. That is a pre-existing SFS limitation, and a disk
    install makes it real.
  - **There is no partition support.** SFS mounts a whole `blk_device`, and
    `main.c` only reads the MBR signature (`:1307`).
- **SFS** is copy-on-write, and its journal makes only the root swap
  crash-atomic (`sfs.c:27-31`, ADR-018 slice 4g):
  - data blocks are written out of place;
  - the superblock and journal record are overwritten in place.
- **The kernel cannot read its own install media.** `ramdisk.c:5` says so, and
  AHCI explicitly excludes ATAPI (`ahci.c:7`, `:73`). There is also **no USB
  stack** (grep for `xhci|ehci|usb` over `kernel/` is empty).
- **The BIOS disk layout is raw and has no partitions** (`Makefile:753`):
  - LBA 0: stage1 (512 B, 446 B of code, which ends with `times 510-($-$$)`);
  - LBA 1: stage2 (≤ 16 sectors, 1,452 B today);
  - LBA 17: the kernel, up to 48×64 sectors = 1.5 MiB, so it ends by LBA 3089.
- **stage1 already tolerates an MBR partition table.** The ISO's hard-disk
  emulation image is a copy with one written by `mk_hdimg.py`, and it boots in
  `smoke-iso-x86`.
- **The UEFI loader is 6,144 B** (`build/BOOTX64.EFI`) and reads `KERNEL.BIN`
  from its ESP.
  - The ESP is built **host-side** with `mkfs.fat` plus `mtools`
    (`Makefile:1126`).
  - **There is no in-kernel FAT formatter.** The kernel has a FAT32 file
    driver, but no mkfs.
- **The live ISO's root is a RAM disk** (DDR-972), so nothing survives reboot.
  **There are no user accounts and no login.** The OS is single-user by
  decision #25.
- **Headroom:** `kernel.bin` is 1,352,074 B against the 1,572,864 B gate, so
  **220,790 B remain**. Each embedded probe ELF costs a page-aligned 8,192 B.

## §2 The payload problem, and the choice

An installer has to write a bootable system: stage1, stage2, the kernel,
`BOOTX64.EFI`, and an ESP to hold the last two. The running kernel cannot read
the CD it booted from (§1). Three routes:

- **(A) The loaders keep a pristine copy of `kernel.bin` in memory and hand its
  physical address over in `boot_info`.**
  - stage1, stage2 and `BOOTX64.EFI` are small (512 + 1,452 + 6,144 B), so they
    are **embedded in the installer ELF** with `incbin`.
  - The kernel itself cannot be dumped from memory, because `.data` and `.bss`
    are live. So the loader copies it before jumping.
  - Cost:
    - stage2 does a second 48-chunk read to a separate physical range;
    - the UEFI loader does a second `AllocatePages` plus copy;
    - `boot_info` gains two fields. This is **§INV.13's class**: both loaders
      change in one commit.
- **(B) ATAPI in AHCI plus an ISO 9660 reader.** It reads the real media. But
  it is a new command set (PACKET), a new filesystem, and it depends on QEMU's
  CD attachment (IDE on i440fx vs AHCI on q35). Roughly 3× the code of (A).
- **(C) A second, "installer" boot medium.** It moves the problem rather than
  solving it.

**Choice: (A).** It is mine to make and needs no operator decision. It also
gives the installer a known-good kernel image independent of the boot medium.
That property matters: a USB-stick boot would have the same "cannot read the
media" problem, because there is no USB stack.

## §3 Installed disk layout

The disk uses **MBR**, not GPT. GPT's header lives at LBA 1, which is exactly
where stage2 sits, so a GPT disk would need stage1/stage2 relocated and both
loaders re-proven. UEFI firmware boots an MBR disk's type-`0xEF` partition, and
OVMF does. That is a checked claim, to be **measured** in gate arm U, not
assumed.

| Region | LBA | Contents |
|---|---|---|
| Boot area | 0 | stage1 plus an MBR table with two entries |
| | 1..16 | stage2 |
| | 17..3089 | `kernel.bin` (raw, for the BIOS path) |
| gap | ..4095 | zero |
| P1 ESP, type `0xEF` | 4096, 64 MiB | FAT16: `EFI/BOOT/BOOTX64.EFI` and `KERNEL.BIN` |
| P2 root, type `0xDA` (non-FS data) | P1 end .. disk end | a DDR-1144 encrypted volume containing SFS |

Notes on the layout:

- **Both loaders carry the same kernel bytes.** The kernel is written twice:
  raw at LBA 17 for stage2, and as `KERNEL.BIN` for the UEFI loader.
  "Byte-identical, nothing conditional on which loader ran" (`Makefile:1127`)
  stays true.
- **FAT16, not FAT32**, for a small ESP. UEFI accepts FAT12/16/32 on an ESP.
  The installer carries a **minimal FAT16 formatter** that writes one boot
  sector, two FATs and a root directory, then lays out two files contiguously.
  That is about 250 lines. A cheaper host-built "skeleton" image was
  considered and **refused**: its cluster positions would be a silent contract
  with a host tool.
- The partition type byte `0xDA` is chosen so that no other OS auto-mounts P2
  as a filesystem it knows.

## §4 Components

1. **Partition sub-device.**
   - `blk_part_create(parent, lba_start, sectors)` registers a `blk_device`
     whose read/write add an offset and **bounds-check** against the partition
     end.
   - SFS then mounts a partition unchanged.
   - Bounds: an out-of-range LBA returns `-EINVAL`. It is never clamped or
     wrapped.
2. **Flush.**
   - A `flush` op on `blk_device`, implemented in three places:
     - virtio-blk `VIRTIO_BLK_T_FLUSH`, when `VIRTIO_BLK_F_FLUSH` is negotiated;
     - AHCI `FLUSH CACHE EXT` (0xEA);
     - NVMe Flush (opcode 0x00).
   - The ramdisk no-ops.
   - SFS `txn_commit` gains flush barriers: data blocks, then flush, then the
     journal record, then flush, then the superblock.
   - That makes the existing root-swap atomicity true **on a disk with a write
     cache**. Today it is true only on QEMU's defaults.
3. **Disk enumeration** as a syscall, listing name, capacity, and whether the
   disk is the boot medium or a ramdisk. It is **NSI 104**, the next free
   (verified in `syscall.h:185`).
4. **The install syscall(s)**, sovereign-only. Ring 3 must never be able to
   write the boot area of an arbitrary disk without this gate. It is a
   capability check in the S2 invariant family.
5. **The `install` ring-3 program**, a text flow on the PRISM console:
   - list the disks, choose one, and type the disk name to confirm the wipe;
   - set the passphrase (DDR-1144);
   - show the recovery key and write the escrow record (DDR-1146);
   - write, verify by reading back, and reboot.
   - **Whole-disk wipe only.** No resizing and no dual-boot.
6. **Installed-boot root selection.**
   - At boot the kernel scans disks for this layout's MBR signature plus the
     DDR-1144 volume header magic in P2. If found, it unlocks (DDR-1144/1145)
     and mounts SFS from the crypt device as the **default root**, instead of
     the DDR-972 ramdisk.
   - If nothing is found, the live path is **unchanged**. That is the
     compatibility claim, and every one of the 182 existing gates tests it for
     free.

**User accounts and login are NOT included, and they are not cheap.** No
concept of a user exists anywhere in the tree. So they are named as a
**follow-on**. This bears directly on DDR-1145 §5: without a login, TPM-only
auto-unlock protects less than it appears to.

## §5 Gate design (installer half); vacuity checked before writing

**`smoke-install`** runs QEMU with a **blank** virtio disk attached beside the
ISO:

- **Boot 1:** the ISO boots, the injector drives `install`, and the setup
  writes a marker file `/INSTALLED.MARK`, whose content is a random nonce
  **generated by the guest** and printed.
- **Boot 2:** the ISO is **removed**, and QEMU boots from the disk alone, once
  under BIOS and once under OVMF.

Arms:

- **P:** boot 2 prints the **same nonce** read back from `/INSTALLED.MARK`.
  - The vacuous version would assert only that "boot 2 reached
    `NEXUS KERNEL OK`". A disk that boots but roots at a RAM disk passes that.
    The nonce is the one thing a RAM-disk root cannot produce, because it was
    generated in boot 1.
- **B / U:** boot 2 succeeds on both loaders, with `[uefi] handoff` present on
  the U arm only.
- **R:** boot 2 prints `[root] disk` and **not** `[ramdisk] formatted SFS`.
- **Negative:** a disk carrying a foreign MBR and no volume magic boots the
  **live** path. Detection must not take over a disk it did not write.

Planned mutants:

| Mutant | Change | Must fail |
|---|---|---|
| M1 | the installer skips the SFS format | arm P |
| M2 | the partition sub-device drops the offset | arms P and R |
| M3 | the UEFI kernel copy is written but the ESP FAT entry is wrong | arm U only |
| M4 | root selection ignores the header | arm R |

**Crash consistency is recorded, not claimed.** The flush barriers (§4.2) are
testable only for ordering, by trace. QEMU cannot pull power mid-write. So "the
disk survives power loss" is a **hardware-only** claim (§7).

## §6 Size and risk: all four DDRs

These are estimates, stated as ranges. None is a measurement.

| Piece | New code (approx.) | New gates | Risk | Why |
|---|---|---|---|---|
| DDR-1143: partition, flush, installer, FAT16, root selection, loader pristine copy | 1,800–2,400 lines | `smoke-install` (two boots × two loaders) | **High** | It changes **both boot loaders**. §INV.13/§INV.18 class, and the loaders are the least-instrumented code in the tree. |
| DDR-1144: crypt device, header, keyslots, KDF, unlock syscall and prompt | 1,200–1,600 | `smoke-crypt` | Medium | The crypto is reused. The risk is in the construction (DDR-1144 §2) and in wiring unlock ahead of root mount. |
| DDR-1145: TPM TIS+CRB, TPM2 command subset, loader kernel measurement, seal/unseal/reseal | 1,800–2,500 | `smoke-tpm` (needs swtpm in CI) | **High** | A new subsystem. Real-chip behaviour is **not verifiable here** (DDR-1145 §6). |
| DDR-1146: recovery key, device id, escrow record, export, rotation after use; host reference tool | 500–800 in the OS, plus a ~300-line host tool | `smoke-recovery` | Medium in the OS | The **backend is external and is not built** (DDR-1146 §4). |
| **Total** | **~5,300–7,300 lines** | 4 gates, some multi-boot | | |

- **Kernel headroom:** 220,790 B is enough for the kernel-side pieces, roughly
  60–110 KB, if the installer, unlock and recovery programs are ring-3 ELFs
  that are **not** embedded in `kernel.bin`, as `term.elf` is not today. If it
  is not enough, the stage2 window must be raised in both loaders (§INV.18).
  That would be flagged at the time, not absorbed silently.
- **Time:** at the cadence of DDR-1139..1142, including CI three-green cycles,
  roughly **8–12 working sessions**. The TPM half is the largest single
  uncertainty.
- **Sequencing:** 1143 → 1144 → 1146 (device side) → 1145.
  - TPM goes last because it is the most separable and the riskiest.
  - Passphrase plus recovery is a complete, testable encryption story without
    it.
  - Your instruction requires **both layers present** before v1.0.0, so 1145 is
    not optional. It is only last.

## §7 What cannot be verified without physical hardware

This is stated plainly, as the operator asked.

1. **Power-loss durability** of the flush ordering on a real disk with a write
   cache.
2. **Real UEFI firmware** booting an MBR ESP. OVMF is the proxy, exactly as for
   DDR-1142.
3. **Real NVMe/SATA controllers** under an install-sized write load. Only QEMU
   models are exercised.
4. **Everything TPM-specific that swtpm does not model:**
   - a discrete TPM's bus exposure;
   - fTPM/CRB vendor quirks;
   - real firmware PCR values and event logs;
   - dictionary-attack lockout timing.
   See DDR-1145 §6.

## §8 Decisions needed from the operator (all four DDRs)

| # | Decision | Where argued | My recommendation |
|---|---|---|---|
| D1 | Escrow key custody. **Any escrow that can recover every device holds, by construction, a credential that decrypts every record.** Options: one HSM key; threshold M-of-N; per-distributor keys; owner-bound shares. | DDR-1146 §3 | Threshold (2-of-3) in HSMs, with per-distributor records. |
| D2 | How the escrow record leaves the device: printed/typed text (needs nothing new) vs network upload (needs your escrow endpoint and its public key). | DDR-1146 §2.1 | Text record for v1. Upload after a backend exists. |
| D3 | Where the escrow backend lives, who runs the offices' identity check, and where the office-side audit log is kept. **External to this OS.** | DDR-1146 §4 | Yours. The OS side is built against a record format, not a service. |
| D4 | TPM-only auto-unlock vs **TPM+PIN**, given there are no user accounts. | DDR-1145 §5 | **TPM+PIN**, or TPM-only with the limitation stated in the release notes. |
| D5 | Passphrase KDF: PBKDF2-HMAC-SHA256 (composes existing primitives; not memory-hard) vs Argon2id (needs BLAKE2b, a **new** primitive, KAT-gated like ML-DSA). | DDR-1144 §4 | Argon2id if the schedule allows. Otherwise PBKDF2 with the weakness stated. |
| D6 | BIOS-booted installs get **passphrase only** (no TPM measurement path on the BIOS side). | DDR-1145 §4 | Accept. |

### §8.1 Decided 2026-09-26 (PR #17 comment 5841525203, OWNER)

| # | Decision |
|---|---|
| D1 | Threshold 2-of-3 custody plus per-distributor escrow keys (DDR-1146 §3 (b)+(c)). Device side: one `EPK` per build. |
| D2 | Text escrow record for v1 (`ESCROW.TXT` on the ESP, DDR-1146 §2.1). |
| D3 | The escrow backend, the office identity/ownership check and the office audit log are the operator's, external. The OS builds against the record format only. |
| D4 | **TPM+PIN.** TPM-only is rejected: with no login, a stolen machine must not reach the desktop on power-on alone. |
| D5 | **Argon2id**, if the schedule allows. If it does not fit, fall back to PBKDF2 **and say so explicitly with the reason**; no silent downgrade. |
| D6 | BIOS-booted installs get passphrase + recovery only, no TPM. |

All four DDRs are approved to proceed in the order 1143 -> 1144 -> 1146 -> 1145.
The first TPM-path step, measuring OVMF's TCG2 support, is done: DDR-1145 §1.1,
and the track proceeds.

## §9 Not claimed

- **Only §10's pieces are implemented.** The rest is design.
- **Nothing is built, gated or tagged.**
- The line-count and session estimates are **estimates**.
- **No physical-hardware claim** of any kind.
- "No installer, a known limitation" (#19) and the #40 deferral are
  **superseded by operator instruction 5839562349**, not by anything built.
- Kernel unchanged: `9ff230a9dc3395ec`, 1,352,074 B. GLOBAL_FORBIDDEN 77. 182
  gates.

## §10 Implementation record

### §10.1 Piece 1 — partition sub-device + MBR parser (2026-09-26)

Built as §4.1 describes. `kernel/drivers/blk/blk_part.c`:

- `blk_part_create(parent, start, sectors)` registers a partition as an
  ordinary `blk_device`. It is **appended** to the registry, so no existing
  device index moves. It has to be registered: `vfs_mount` takes a registry
  index, and `sfs_bd_is_registered` (DDR-985) refuses unknown devices.
- Bounds are **refused** (`-EINVAL`), never clamped. The check is
  `lba <= cap && count <= cap - lba`, so a huge `lba` cannot wrap `lba + count`
  (the same shape `ramdisk.c` uses).
- `blk_mbr_parse` needs the `0x55AA` mark and skips any entry that is empty,
  zero-length, or **extends past the disk**. Root selection (§4.6) will pass
  these numbers straight to `blk_part_create`, so a lying table is refused per
  entry. `blk_mbr_set` sits beside the parser so the two cannot disagree about
  the layout.

**Gate `smoke-part`** (shard 2, strict, probe key `part`). It uses a
512-sector ramdisk: P1 `0xEF` at 64+128, P2 `0xDA` at 192+320, and a third
entry that overruns the disk. Arms:

| Arm | Line | What only a correct implementation produces |
|---|---|---|
| mbr | `n=2 ok` | the overrunning entry is skipped |
| sig | `rc=-EINVAL` | an unsigned sector is refused |
| mk | `over=-EINVAL wrap=-EINVAL` | creation bounds, including a wrapping start |
| off | `w=0 parent192=marker sector0=intact` | partition LBA 0 is **parent** LBA 192, read back through the parent |
| bnd | `w128=-EINVAL r127x2=-EINVAL r127=0 neighbour=kept` | P1's end is enforced, not the parent's; P2's first sector survives |
| wrap | `rc=-EINVAL` | `lba = 2^64 - 1` is refused |
| sfs | `format=0 mount=ok p1_dirty=0` | SFS formats and mounts **a partition**, and writes nothing outside it |

**Mutants, each on a recorded hash, each failing a different arm:**

- **M1 (write offset dropped, `1da2dc4aceac966f`):** fails `off`, and downstream
  `bnd` and `sfs`.
- **M2 (bound checked against the parent's capacity, `a4ad521383206f1d`):**
  fails `bnd` with `w128=written neighbour=overwritten`. This is the
  load-bearing mutant: the obvious one-arm gate ("a partition read works")
  passes it.
- **M3 (no signature check, `d1b502efd69babad`):** `sig=accepted`.
- **M4 (`lba + count <= cap`, `a723f02e5dd4d81d`):** `wrap=created`, and
  `rc=` shows a read of parent sector 191, which is outside the partition.

The revert returns `90f14648c3752503` **bit-for-bit**. The regression suite
(`smoke-shell`, `smoke-blkmq`, `smoke-rqstress-liveness`,
`smoke-blk-integrity`) is all rc=0 with the hash pinned. `smoke-shell` is 5/5.

`kernel.bin` is 1,352,074 → 1,356,170 B (+4,096, one page). 183 gates.

**Not claimed:**

- nothing is installed;
- no disk is partitioned outside the probe's ramdisk;
- the flush (§4.2) is piece 2 and is not built.

### §10.2 Piece 2 design: flush op + SFS barriers (committed before the code)

**Interface.**

- `struct blk_device` gains `int (*flush)(struct blk_device *)`.
- `blk_flush(dev)` returns `-ENOSYS` when the op is NULL. It does **not**
  return 0, because a driver that has a cache but forgot the op must not look
  flushed.
- Every in-tree driver sets the op:
  - **virtio-blk:** negotiate `VIRTIO_BLK_F_FLUSH` (bit 9). When it is
    negotiated, submit a 2-descriptor request (header + status, no data) with
    `type = VIRTIO_BLK_T_FLUSH (4)`. When it is not negotiated, return 0,
    because the virtio spec defines such a device as write-through.
  - **AHCI:** `FLUSH CACHE EXT` (0xEA), a non-data command with `prdtl = 0`.
  - **NVMe:** Flush, opcode 0x00, NSID 1, no PRPs.
  - **ramdisk:** an explicit no-op returning 0.
  - **partition:** forwards to the parent's flush.

**SFS barriers.** A helper `sfs_barrier(c)` calls the op.

- `sfs_journal_write`: `F` before writing the journal record, so every CoW
  block of the transaction is durable before the record naming them.
- `sfs_write_super`: after `sfs_freelist_save`, an `F`, then the superblock,
  then an `F`.
- So a transaction commit issues `F J L F S F`, and a plain
  (non-transaction) commit issues `L F S F`, where:
  - `D` = data / B+tree block;
  - `J` = journal block;
  - `L` = free-list block;
  - `S` = block 0.

**Gate arms (on `smoke-part`, same probe key).** A **trace device** wraps a
ramdisk and records every op as a letter, classified by block number against
the mounted context:

| Arm | Checks | Mutant that must fail it |
|---|---|---|
| `txn` | the exact commit window of one transaction | M1: no `F` before `J`; M2: no `F` before `S` |
| `tail` | the op after `S` is `F` | M3: no `F` after `S` |
| `plain` | the exact window of one non-txn commit | M2 as well |
| `virtio` | `neg=1`, a flush on a real virtio disk returns status 0, and the count of flushes issued by the SFS work matches the count completed | M4: the virtio header type is left at its previous value |

Each commit-window string is **pinned after measuring it once** (L's presence
depends on `sfs_freelist_save`'s own condition). The pin is recorded here
together with the capture.

**Honest limits.**

- The arms prove **ordering at the block interface**, and that the device
  **accepts** the flush command. They do **not** prove data reached stable
  media: QEMU cannot pull power (§7).
- AHCI and NVMe flushes are exercised only by their existing gates' self-tests
  (a printed rc).

**Cost, measured before and after.** QEMU's default `cache=writeback` turns
each guest flush into a host `fdatasync`. SFS commits happen on every
operation, so the SFS-heavy gates (`smoke-sfs-gc`, `smoke-sfs-btree`,
`smoke-fs-sfs-rw`) are timed on both sides. If the cost threatens a gate's
window, the choice (scratch disks at `cache=unsafe` in the harness, which is
honest only for disks that are never persisted) is stated here. It is not
applied silently.

### §10.3 Piece 2 implementation record (2026-09-26)

**Shipped exactly as §10.2 designed.**

- `blk_flush(dev)` returns `-ENOSYS` for a NULL op.
- Every driver sets the op:
  - virtio-blk negotiates bit 9 and sends a 2-descriptor `T_FLUSH`;
  - AHCI issues `0xEA` with `prdtl = 0`;
  - NVMe issues opcode `0x00`, NSID 1;
  - ramdisk has a no-op;
  - a partition forwards to its parent.
- SFS barriers:
  - one before the journal record;
  - two around the superblock write;
  - one at the end of `sfs_format`.

`kernel.bin` is `df4d7d6d472f4fe7` at 1,360,266 B (+4,096). The build is
warning-clean at `-Werror`.

**Pins, measured once and then asserted.** A trace device wraps the probe's
ramdisk and prints one letter per op. The `L` letter is present, which confirms
that `sfs_freelist_save` writes its block on both paths.

| window | measured | pinned sentinel |
|---|---|---|
| plain create commit | `DDDLFSF` | `[part] trace plain=DDDLFSF …` |
| transaction commit | `FJLFSF` | `… txn=FJLFSF tail=ok setup=ok` |
| virtio flush on a real disk | `neg=1 flush=0`, issued == completed | `[part] virtio neg=1 flush=0 issued=ok=yes` |
| AHCI self-test | `rc=0` | `[ahci] flush rc=0` (smoke-ahci; `flush FAIL` forbidden) |
| NVMe self-test | OK | `PRADYOS_NVME_FLUSH_OK` (smoke-nvme; `…_FAIL` forbidden) |

**Mutants.** Each changes one thing, is built to its own recorded hash, and is
run on `smoke-part`. The revert returns `df4d7d6d472f4fe7` bit-for-bit, verified
by rebuild.

| mutant | hash | result |
|---|---|---|
| M1 no `F` before `J` | `97cb9d6b0d0cd545` | `txn=JLFSF` — **caught** (plain unchanged: the non-txn path has no journal) |
| M2 no `F` before `S` | `b7f192221aacbb7b` | `plain=DDDLSF txn=FJLSF` — **caught by both windows** |
| M3 no `F` after `S` | `d4cf9778f7956210` | `DDDLFS / FJLFS tail=BAD` — **caught** |
| M4 virtio header type left stale on a flush | `9e1753e74143dacf` | **PASSES — UNCOVERED.** QEMU completes a stale-typed header (a zero-length IN/OUT) with status 0, so from inside the guest a flush sent as the wrong type is indistinguishable from a real one. Recorded as uncovered, per §10.2's own caveat. No guest-side arm can see it. |
| all barriers counted but not flushed (the cost baseline) | `76dde81b74dea40f` | `plain=DDDLS txn=JLS tail=BAD` — caught (3/3, recorded below) |

**Cost, both sides, same host.** The measurement is `g_ticks` (100 Hz *emulated*
time, so it is valid only as a comparison on one host) across the DDR-763
churn loop plus the DDR-762 GC loop, per boot:

| build | ticks (3 boots) | barriers |
|---|---|---|
| barriers flush | 111, 93, 134 (median 111) | 306 / 322 / 322 |
| counted, not flushed | 102, 82, 84 (median 84) | 306 / 322 / 322 |

- The difference is about 27 ticks (~270 ms emulated) over ~315 barriers, i.e.
  **~0.9 ms per barrier**.
- The two ranges overlap (93 < 102), so the spread is as large as the effect.
  The cost is real and small.
- No gate window is threatened: every regression gate is inside its timeout
  (§10.3 regression below).
- So **`cache=unsafe` is NOT applied**, and no harness change is made.

**Not claimed.**

- Ordering is proven at the block interface only; durability on real media is
  not (§7).
- AHCI and NVMe are proven only by a self-test rc, not by a trace.
- M4 is uncovered.
- The `D` count in `plain` is a property of today's create path (inode, dirent,
  B+tree). A future create that writes a different number of blocks changes the
  pin, and that is the pin doing its job.

### §10.4 Piece 3 design: the pristine kernel image handoff (committed before the code)

**Why.** Per §2 route (A), the installer must write the kernel bytes, and
nothing in memory is those bytes once the kernel runs: `.data` is live and
`.bss` sits on top of the load window. So the loader keeps a second copy,
taken before it jumps.

**Where the copy goes.** Physical `0x800000` (8 MiB), in a window of up to
1.5 MiB (ending at `0x980000`). Measured, not assumed:

- It is **below `PMM_MIN_PHYS` (16 MiB, `pmm.c:14`)**, so the PMM can never
  hand it out.
- A grep for physical constants in 6–16 MiB across `kernel/`, `boot/` and
  `arch/` returns **nothing**.
- The kernel's own window is `0x400000..0x600000`, and the page tables are at
  `0x300000`.
- It sits in a different 2 MiB page from the DDR-1046 RO+NX alias of the
  kernel image, and is readable through the low 1 GiB identity map both
  loaders build.

**The handoff block.** The `boot_info` header cannot grow, because it ends in
the flexible `e820[]` array (the DDR-1142 reasoning). So a second 32-byte block
goes at `0x4FC0`, directly below `boot_fb` at `0x4FE0`:
`struct boot_kimg { magic 'KIMG', source, base, size, check }`, where `check` is
the xor of every other word.

- The UEFI loader's E820 cap must fall from **167 to 165**
  (32 + 165×24 = 0xF98 ≤ 0xFC0), enforced by a `_Static_assert`.
- stage2 caps at 32 entries (0x320), far clear.
- Both loaders change in one commit. This is **§INV.13's class**.

| field | BIOS (stage2) | UEFI |
|---|---|---|
| `source` | 1 | 2 |
| `size` | the **read window**, 0x180000. stage2 reads a fixed 48 chunks and never learns the file size. | the **exact file size** from `EFI_FILE_INFO` |

**stage2 needs NO second disk read**, which is cheaper than §2 assumed. Each
32 KiB chunk already sits in the bounce buffer, so it is copied **twice** — to
`KERNEL_PHYS` and to `0x800000` — inside the same unreal-mode window. stage2
must stay ≤ 8 KiB (asserted by its build).

**The UEFI loader** claims `0x800000` with `AllocateAddress` (`EfiLoaderData`,
which `fill_boot_info` already reports as reserved), and copies the file bytes
from `0x400000` right after the read.

**Kernel side.**

- `kernel.ld` gains `__data_end` at the end of `.data`. That adds a symbol,
  not bytes, so the hash is unchanged (verified at build).
- The image length is `__data_end - KERNEL_VBASE`. Today that is 1,360,266,
  the size of `kernel.bin`; `__bss_start` is 1,360,320 because of the ALIGN(64).
- `kimg_init()` validates magic and check, then requires `size >= image length`
  (BIOS) or `size == image length` (UEFI). Otherwise it refuses and reports
  `none`.
- It exposes `kimg_get(&base, &len)` for the installer (piece 5).

**Gate arm, vacuity checked first.**

- "The block validates" is **vacuous**: it passes when the loader copied
  nothing.
- **The arm is the HASH.** Under probe key `kimg`, the kernel prints
  `[kimg] src=<bios|uefi> len=<n> sha=<first 16 hex of SHA-256 over len bytes>`.
  The Makefile recipe computes `sha256sum build/kernel.bin` **at make time**
  and requires that exact string. So the expected value comes from the host's
  file, not from anything the guest can print.
- The arm goes on **`smoke-part`** (BIOS; the key joins `part`, a gate that
  already boots) and on a **new `smoke-kimg-uefi`**. `smoke-uefi` has no probe
  plumbing to borrow, so this is decided at build: whichever form adds the
  fewest moving parts.

**Mutants, planned.**

| mutant | expected result |
|---|---|
| K1: stage2 skips the second copy | sha of zeros → BIOS arm fails |
| K2: the kernel hashes `KERNEL_PHYS` (the live image) instead of the copy | `.data` has changed by then → sha differs. **This is the one that proves the copy is taken before the jump, not after.** |
| K3: the UEFI loader copies from the wrong source | UEFI arm fails |

**Not claimed.**

- The pristine copy is a boot-time snapshot. Nothing prevents a ring-0 bug from
  scribbling on it later. **CR0.WP plus an RO mapping of that range** is
  recorded as a follow-on, not done here.
- The BIOS size is the window, not the file. The installer uses the
  linker-derived length in both paths, and for UEFI that length is
  cross-checked against the file.

### §10.5 Piece 3 implementation record (2026-09-26)

**Shipped, with one measured correction to §10.4.**

- stage2 copies each bounce chunk twice (no second disk read) and writes the
  32-byte `boot_kimg` block at `0x4FC0` only after the last chunk lands.
- The UEFI loader copies the file bytes from `0x400000` right after the read.
- The kernel (`kernel/kimg.c`) validates magic, check, base range and size.
  It prints `[kimg] src=… len=…` on every boot, or `[kimg] refused reason=…`,
  and exposes `kimg_get()` for the installer.
- `__data_end` = `0xffffffff8014c18a`, so the image length is 1,360,266, equal
  to `kernel.bin`, which was checked.
- `kernel.bin` is `afecbb54b2774641` at **1,360,266 B, size unchanged**. The new
  code fits in page padding, so the size/headroom carriers are unaffected.
- stage2 is 1,540 B, well under 8 KiB.

**THE CORRECTION: §10.4's UEFI address was wrong, and the first boot said so.**

- `[uefi] FATAL cannot claim 0x800000` — OVMF owns that range. §10.4's "nothing
  uses 6–16 MiB" was measured over *this tree's* code and said nothing about
  firmware.
- **A second error was caught before the fix shipped:** §10.4 claimed
  `EfiLoaderData` is "reported reserved". In fact `fill_boot_info` reports it
  **usable** (`type = 1`), so a copy placed above 16 MiB would have been handed
  out by the PMM.
- **The fix:**
  - The UEFI loader uses `AllocateMaxAddress` capped at `0xFFFFFF`, below the
    PMM floor.
  - If there is no room, it **skips the copy and keeps booting**. The kernel
    then says `refused reason=none` and the installer must refuse. That is
    better than a loader that dies.
  - The kernel checks a **range** for UEFI (page-aligned, ≥ 1 MiB, wholly
    below 16 MiB, clear of `0x300000..0x600000`) and an **exact** `0x800000`
    for BIOS.
  - The E820 cap moves 167 → 165, enforced by a `_Static_assert` against
    `0xFC0`.

**Arms — the expected hash comes from the host, not the guest.**

- `smoke-part` (key `part,kimg`) and `smoke-uefi` (key `kimg`) each compute
  `sha256sum build/kernel.bin` at make time and require
  `[kimg] src=<bios|uefi> len=<size> sha=<first 16 hex>`.
- `[kimg] refused` and `KIMG FAIL` are forbidden on both gates.
- Measured: `sha=afecbb54b2774641` on both paths, equal to the host's hash.

| mutant | kernel / stage2 / efi | result |
|---|---|---|
| K1 stage2 skips the second copy | `afecbb54…` / `a69e365f…` | `sha=48b55d7bf3386177` — **caught** |
| K2 the kernel hashes the LIVE image at `0x400000` | `265d3790…` | `sha=e4914b8767506378` — **caught**. **This is the mutant that proves the copy predates the kernel's own writes to `.data`.** |
| K3 the UEFI loader allocates but never copies | efi `00e20b94…` | `sha=48b55d7bf3386177` — **caught** |

- K1 and K3 print the **same** hash. That is the SHA-256 of 1,360,266 bytes of
  the zeroed memory both leave behind, which is itself a check that the arm
  hashes the region it names.
- **Revert:** the kernel returns `afecbb54b2774641` and stage2 returns
  `67b8243b8eb35b99`, both bit-for-bit.
- **`BOOTX64.EFI` is not reproducible bit-for-bit across builds of identical
  source.** `cmp -l` shows exactly one byte at offset 129, the PE
  `TimeDateStamp`. Its revert is therefore verified by source, not hash.
  Recorded rather than papered over, because a future "efi hash changed"
  observation must not be read as a code change.

**Not claimed.**

- The copy is a boot-time snapshot and is not write-protected afterwards
  (follow-on: an RO mapping under CR0.WP).
- Where OVMF placed the UEFI copy is **not pinned**, because it is the
  firmware's choice. The arm proves the bytes, not the address.
- Real UEFI firmware may have less free memory below 16 MiB. The skip path is
  the answer and is **not exercised by a gate**: forcing it would need a
  loader mutant, and the kernel side (`refused reason=none`) is the same code
  the BIOS path's absent-block case uses.

### §10.6 Pieces 4–6 design: install engine, console gate, root selection, `smoke-install` (committed before the code)

The shape is chosen by measurement, and two §4 assumptions do not survive it.

**Finding 1: there is no ring-3 place for an installer ELF to come from on the
ISO.**

- The ISO root is a freshly formatted ramdisk (DDR-972).
- The only runnable programs are those **embedded in `kernel.bin`** (PRISM is
  `prism_elf`, loaded by `user_boot_from_sfs`).
- A separate `install.elf` would need embedding anyway, so it saves nothing.
- **The install engine therefore lives in the kernel.** It is driven by a
  **PRISM builtin** `install`.
- The three loader blobs are `incbin`'d into `kernel.bin`: stage1 512 B,
  stage2 1,540 B and `BOOTX64.EFI` 6,656 B, about 12 KiB page-aligned against
  212,598 B of headroom.

**Finding 2: `BOOTX64.EFI` is not reproducible** (§10.5: one byte, the PE
`TimeDateStamp`).

- Embedding it would make `kernel.bin`'s hash change on every rebuild of
  unchanged source. That would destroy bit-for-bit revert checks and every
  pinned-hash measurement in this project.
- **Prerequisite:** `lld-link -Brepro` (a deterministic timestamp). This is
  verified by building twice and comparing, and it is done **before** anything
  embeds the file.

**Finding 3: PRISM is not sovereign.** `user_boot_from_sfs(…, 0)` sets
`is_sovereign` only when asked, and PRISM is not asked.

- **The authority check for `SYS_INSTALL` is "the caller is the boot console
  shell".** That means `current pid == g_console_pid`, recorded when
  `main.c` spawns PRISM.
- A forked child, including a piped builtin, gets a new pid and is refused.
  So is every other process.
- The human consent is the typed confirmation (below). A kernel cannot verify a
  person (DDR-1146 §4); what it can guarantee is that **only the interactive
  console path reaches the disk-wiping syscall**.
- An S2-family denial arm is included (see gate).

**NSI allocation** (verified free: max defined 103):

| NSI | call | authority |
|---|---|---|
| 104 | `SYS_DISK_LIST(struct disk_info *out, n)` → count: name, sectors, flags (ramdisk, partition, blank, installed) | any process (read-only) |
| 105 | `SYS_INSTALL(disk_idx, const char *confirm, uint64_t *nonce_out)` | console shell only, else `-EPERM` |

For `SYS_INSTALL`, `confirm` must equal `"WIPE-<name><idx>"`; anything else
returns `-EINVAL` **before any write**. A ramdisk or partition target returns
`-EINVAL`.

**Layout writer** (`kernel/install/install.c`), following §3. MBR disk
signature at 440 = `'PRDI'`.

| LBA | write |
|---|---|
| 0 | stage1 plus the table: `0xEF` at 4096 for 131,072 sectors (64 MiB); `0xDA` from 135,168 to the end |
| 1..16 | stage2, zero-padded |
| 17.. | the pristine kernel from `kimg_get()`, then zeros to 4095 |
| P1 | FAT16: one reserved sector, 2 FATs, a 512-entry root, 4-sector clusters. Contents: `EFI/BOOT/BOOTX64.EFI` and root `KERNEL.BIN`, contiguous. About 250 lines. |
| P2 LBA 0..7 | the volume header: magic `PRDYVOL1`, version 1, `flags = PLAINTEXT` (**DDR-1144 replaces this with the crypt header; until then P2 is NOT encrypted, and the header says so**), SFS offset 8 |
| P2+8.. | `sfs_format` on a partition sub-device, then `/INSTALLED.MARK` holding a 64-bit nonce from `rng_get`, returned to the caller |

- Minimum disk size: 135,168 + 32,768 sectors (80 MiB). Smaller returns
  `-ENOSPC` before any write.
- Barrier: `blk_flush` last, and its rc is returned.
- **Every write is read back and compared** (§4.5 "verify by reading back").
  A mismatch returns `-EIO` with the LBA printed.

**Live-path condition, widened by exactly one case** (boot 1 of the gate is
the ISO plus a *blank* disk):

- The DDR-972 ramdisk branch fires on `blk_count() == 0` (unchanged) **or** on
  `blk_count() == 1 && sector 0 of blk0 is all zero`.
- In the second case the blank real disk plays blk0's "boot-disk stand-in"
  role. So only the root (blk1) and scratch (blk2) ramdisks are created, and
  the topology the boot path expects is identical.
- Every gate's blk0 is `pradyos.img` with an MBR, so **no gate can take the
  new case**. That is the same safety argument DDR-972 used, and it is checked
  by the full hygiene set.

**Root selection (piece 6):**

- It fires on `blk_count() == 1`, with blk0's MBR carrying `'PRDI'` at 440,
  entry 1 type `0xDA`, and the P2 header magic valid.
- It registers the P2 SFS sub-device (blk1) plus a 4 MiB scratch ramdisk
  (blk2), which is the ISO topology again. `fs_test_thread`'s existing loop
  then mounts blk1.
- It prints `[root] disk p2 lba=<n>`. **It never formats blk1.**
- A header with `flags != PLAINTEXT` prints `[root] encrypted volume: unlock
  not built` and falls to the live path. That is the hook DDR-1144 replaces.
- **v1 limit, stated:** more than one disk present means no root selection.
  Real machines with two disks are not handled until the topology is
  generalised.

**Gate `smoke-install`** (a new script, `tools/qemu_runner/install_test.sh`;
sequential QEMU only, NON-NEGOTIABLE 12):

| arm | checks |
|---|---|
| boot 1 | ISO plus a blank 128 MiB raw disk. The injector types `install`, then `install 0 WIPE-virtio-blk0`. Requires `[install] ok nonce=<N>`, and captures N. |
| deny | the console-only check is proven by a ring-3 probe calling NSI 105 → exactly `-EPERM` (put on `smoke-part`, which already boots probes) |
| B | boot 2, **disk only**, SeaBIOS: `[kimg] src=bios`, `[root] disk`, NOT `[ramdisk] formatted SFS`, **and `[install] mark nonce=<N>` equal to boot 1's N** (arm P) |
| U | boot 2 again under OVMF: `[uefi] handoff` plus the same three |
| negative | a disk with a foreign MBR (the existing `pradyos.img` copy has no `'PRDI'`) → no `[root] disk` |

- **Arm P is the load-bearing one:** the nonce is produced by boot 1's RNG and
  can only be read back if the root is really the installed P2.
- Boot 2 prints the mark from `fs_test_thread` right after mount:
  `[install] mark nonce=`.

**Planned mutants:** §5's M1–M4, plus:

| mutant | change | must fail |
|---|---|---|
| M5 | the console check removed | deny arm |
| M6 | the blank-disk widening removed | boot 1 (no PRISM) |
| M7 | readback verification skipped, plus a corrupted write | (planned) readback `-EIO` — may be uncovered; decided at build |

**Build order:** `-Brepro` → NSI 104 → the install engine (FAT16 and layout)
plus NSI 105 plus the builtin → root selection → `install_test.sh` and the
gate → mutants → regression. Each is committed separately.

**Not claimed here:** encryption (DDR-1144); TPM (DDR-1145); recovery
(DDR-1146); passphrase prompts; power-loss durability (§7); two-disk machines.

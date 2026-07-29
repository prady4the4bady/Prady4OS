# DDR-799 — GLOBAL_FORBIDDEN false positives: full probe audit

**Status:** audited and fixed in this slice.
**Date:** 2026-07-29
**Follows:** DDR-791 (`GLOBAL_FORBIDDEN`), DDR-798 (the first instance of this
pattern).

## Why a full audit rather than another single fix

DDR-798 fixed one probe that declared failure on a deadline it could not judge.
CI then failed on a *different* probe (`ROOTMOUNT FAIL` in `smoke-sfs-persist`).
Fixing them one CI run at a time costs ~105 minutes per instance and gives no
idea when it ends, so this is a single pass over every probe against every
`GLOBAL_FORBIDDEN` pattern.

## Method

Two passes, because neither alone is sufficient.

**Static** — for every probe that can print a `… FAIL` string, find how `kmain`
spawns it and whether the spawn is guarded on the precondition it needs.

**Empirical** — boot the four *distinct device configurations* the gates
actually use and grep the serial capture for every `GLOBAL_FORBIDDEN` pattern.
Static analysis lists candidates; only this lists offenders.

## Result — the empirical pass

| configuration | forbidden patterns observed |
|---|---|
| default (1 CPU, ext4 present, no GPU/NVMe) | **clean** |
| `QEMU_NO_EXT4=1 QEMU_SFS2=1` (the `smoke-sfs-persist` shape) | **`ROOTMOUNT FAIL`** |
| `QEMU_SMP=4` | **clean** |
| `QEMU_GPU=1` | **clean** |

**One offender in this pass — but the pass itself was incomplete; see the
correction below.** The audit is materially
smaller than feared, and three suspicions from the static pass are refuted:

* `surfdestroytest` is spawned unconditionally and needs surfaces — but surfaces
  do not require virtio-gpu, so the no-GPU boot is clean.
* NVMe patterns (`PRADYOS_NVME_*`, `controller not ready`, `identify-*failed`)
  only emit from `nvme_init`, which runs only when PCI enumeration finds a
  controller. Opt-in device, guarded by construction.
* SMP self-tests (`tss/percpu/gs/locks/cross-CPU/ap preempt/resched FAIL`) live
  in the AP bring-up path and cannot emit with one CPU.

## The one real defect

`kernel/main.c:895`:

```c
int ext4_mnt = (blk_count() > 3) ? vfs_mount(3) : -1;
```

The ext4 volume is identified by **disk index**, not by content. `smoke-sfs-persist`
sets `QEMU_NO_EXT4=1` (dropping ext4 to stay under `VBLK_MAX=4`) **and**
`QEMU_SFS2=1` (attaching the host mkfs SFS image). The mkfs image therefore
occupies index 3, `blk_count() > 3` still holds, and `vfs_mount(3)` mounts an
**SFS** volume as if it were ext4.

`rootmounttest` is then spawned rooted at that SFS volume, cannot find
`/EXT4.TXT` — because it is not on an ext4 filesystem — and correctly reports
what it sees. `GLOBAL_FORBIDDEN` then fails a gate that deliberately withheld
ext4.

The harness is right. The kernel guard is weak, and the probe cannot tell
"ext4 is absent" from "ext4 is broken".

## Fix

Per the operator constraint, the **probe owns its precondition check**; `kmain`
and `boot_test.sh` are untouched.

`rootmounttest` gains a three-way verdict, using a discriminator it already has
the means to evaluate:

| `/EXT4.TXT` | `/HELLO.TXT` | meaning | verdict |
|---|---|---|---|
| present + correct | absent | rooted at ext4, selection worked | `PRADYOS_ROOTMOUNT_OK` |
| absent | **absent** | root is neither ext4 nor the FAT default → this gate did not provide ext4 | **`ROOTMOUNT SKIP`**, exit 0 |
| absent | **present** | root fell back to the FAT default → the root-mount selection genuinely failed | `ROOTMOUNT FAIL`, exit 1 |

The third row is the assertion that matters and it is preserved exactly: a real
`root_mnt` regression still fails, loudly, because the fallback root is the one
place `/HELLO.TXT` exists.

`smoke-rootmount` continues to REQUIRE `PRADYOS_ROOTMOUNT_OK`, so a genuinely
broken ext4 path fails that gate on a **missing required sentinel** — which
cannot be masked — rather than relying on a forbidden string.

### Not done, deliberately

* **`GLOBAL_FORBIDDEN` is not narrowed.** `ROOTMOUNT FAIL` stays listed; it now
  only appears for the real failure.
* **`kmain` is not made gate-aware.** The weak `blk_count() > 3` guard is
  recorded here as a known limitation rather than patched with device-probing
  logic in the boot path; that is its own slice if it ever matters beyond this.
* **No probe is silenced.** The skip path prints an informational line, because
  an observation that stops being reported is how a regression becomes invisible.


## Correction — the first pass enumerated configurations by hand and missed one

The audit above scanned four configurations chosen by reading the Makefile by
eye. CI then failed on `smoke-aether-sfsroot` with `SFSROOT FAIL`, because that
gate uses a **fifth** configuration (`QEMU_SFSROOT=1`, which swaps
`build/sfsroot.img` into the SFS2 slot) that was never scanned.

A hand-enumerated list of configurations is the same class of mistake as a
hand-checked rule: it decays the moment someone adds a knob. The scan is now
derived from the Makefile — every `QEMU_*` variable any gate sets — and lives in
`tools/qemu_runner/scan_forbidden.sh` so it can be re-run rather than
reconstructed.

Full result after both fixes:

| configuration | forbidden patterns |
|---|---|
| default | clean |
| `QEMU_SMP=4` | clean |
| `QEMU_GPU=1` | clean |
| `QEMU_GPU=1 QEMU_SMP=4` | clean |
| `QEMU_NVME=1` | clean |
| `QEMU_NO_EXT4=1 QEMU_SFS2=1` | clean |
| `QEMU_SFSROOT=1` | clean |
| `QEMU_SFSROOT=1 QEMU_NO_EXT4=1` | clean |

### The second offender

`sfsroottest` (DDR-760) opens `/etc/aether/config` through its SFS root.
`kmain` provisions that file onto the probe's root only on the path that formats
a clean SFS volume (`main.c` ~1327). `smoke-aether-sfsroot` roots the daemon at
the provisioned mkfs image instead, so the probe's root has no config and it
reported that as a failure in a gate that never asked for it.

Notably that gate declares **no** `FORBIDDEN_SENTINEL`, so before DDR-791 it
early-exited on its three required sentinels and the FAIL string sat unread in
the log — the same silent tolerance DDR-791 was written to end.

Fixed with the same three-way verdict: config absent → `SFSROOT SKIP`; config
present but wrong → `SFSROOT FAIL`. `smoke-sfsroot` still REQUIRES
`PRADYOS_SFSROOT_OK`, so a genuine regression fails there on a missing required
sentinel.

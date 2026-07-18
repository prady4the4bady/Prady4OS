# DDR-761 — AETHER config migration: daemon reads /etc/aether/config on SFS

**Status:** proposed (pre-code)
**Layer:** boot/fs + user (daemon). M2 storage 2/N. Builds on DDR-760.

## Problem

The AETHER daemon reads its boot policy from `/AETHER.CFG` on the **FAT** boot
volume (DDR-732), a stopgap because SFS could not be a durable root. The daemon
source even notes "SFS config (/etc/aether/config) reading is deferred." DDR-760
now provides a clean, provisioned, persistent **SFS root**, so the config belongs
on the sovereign FS at its real path, not on the FAT boot volume.

## Decision

Root the daemon at the DDR-760 SFS root and read `/etc/aether/config` from it. The
daemon opens exactly one file (its config), so switching its root is safe.

- **`user/aether_daemon.c`:** `cfg_load` opens `/etc/aether/config` instead of
  `/AETHER.CFG`. No other daemon file I/O changes.
- **`kernel/main.c`:** the daemon is currently `user_boot_from_sfs`'d (load +
  *immediate* unblock, rooted at the FAT default). But the clean SFS root
  (`root_smnt`) is created later (after the destructive tests, DDR-760). So the
  daemon is now **loaded blocked** at its spawn point — hand-rolled `elf_load`
  (like the DDR-739 probes): set `is_sovereign`, `parent_pid`,
  `g_aether_daemon_pid`, but **do not unblock**. Then, in the DDR-760
  reformat+provision block, `dm->root_mnt = root_smnt; sched_unblock(dm)`.
- The DDR-760 provisioning content becomes the **full daemon config**
  (`mode=sovereign\ntask=test\nslot=0\nnet=10.0.2.2:11434\n`) so the daemon parses
  `mode/task/slot` (the `smoke-aethercfg` assertion) and installs the CAP_NET
  allowlist row (DDR-734) exactly as it did from FAT.

Deferring the daemon's unblock by a few boot steps is safe: only the daemon spawns
agents, and no agent spawn happens until it runs; `g_aether_daemon_pid` is set at
load time so agent parenting is unaffected. The FAT `/AETHER.CFG` is left in place
(now unused, harmless) — removing it from the image build is a later cleanup.

## Gate — existing `smoke-aethercfg` (no new gate; stays 96)

`smoke-aethercfg` already asserts `PRADYOS_AETHER_CFG_OK mode=sovereign task=test
slot=0` and forbids `PRADYOS_AETHER_CFG_DEFAULT`. It now proves the value came
from **SFS** `/etc/aether/config` rather than FAT — the migration is validated by
the existing gate turning green against the new source. Regression: the full
AETHER/agent set (`smoke-aether`, `smoke-agents`, `smoke-netallow`,
`smoke-agentmetrics`, `smoke-agentpanel`) plus `smoke-sfsroot`.

## Non-goals

- No config schema change; same keys (`mode`/`task`/`slot`/`net`).
- FAT `/AETHER.CFG` not removed yet (dead-but-harmless; image-build cleanup later).
- No cross-reboot persistence (host `mkfs.sfs` still deferred).

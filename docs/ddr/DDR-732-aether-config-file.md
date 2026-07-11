# DDR-732 — AETHER boot config from disk (/AETHER.CFG)

**Status:** proposed (pre-code)
**Layer:** 6 (AETHER daemon policy), closes the last deferred non-visual L7 item.
**Extends:** ADR-026 §D11 (the daemon is the operator's proxy).

## Problem

The AETHER daemon's boot policy — initial mode, the bring-up agent's task and
roster slot — is compiled in (`user/aether_daemon.c` hard-codes test mode,
task "test", slot 0). The deferred item (ADR-026 / build_status) is to read it
from disk so an operator changes policy by editing a file, not rebuilding.

## Where the file lives — and why not SFS /etc/aether/config

The deferral note said "SFS `/etc/aether/config`". Two facts about the reference
build make that literal path wrong today:

1. **Ring 3 cannot reach SFS.** `SYS_OPEN` resolves on the process root mount
   (`tcb.root_mnt`), which `elf_load` sets to `vfs_default_mnt()` — the **FAT32
   boot volume**. SFS is the kernel loader's ELF store; no NSI path reaches it.
2. **No subdirectory namespace on the boot volume path in use** — config as a
   root-level file matches every other artifact (`/HELLO.TXT`, `/EXECTEST.ELF`).

So the config lives at **`/AETHER.CFG` on the FAT32 boot volume**, seeded at
image-build time by `mcopy` (exactly how `/HELLO.TXT` gets there) — genuinely
operator-editable without recompiling anything. When SFS becomes the process
root, moving it is a path rename, not a redesign.

## Decision

**File format** (line-oriented, `key=value`, unknown keys ignored, ≤256 bytes
read):

    mode=sovereign        # or: manual
    task=test             # bring-up agent's task string (<=63 chars)
    slot=0                # roster slot 0..7 (KRYOS..SOLIN)

**Daemon behavior** (`user/aether_daemon.c`):
- Open `/AETHER.CFG` (SYS_OPEN/READ/CLOSE), parse the three keys.
- Print `PRADYOS_AETHER_CFG_OK mode=<m> task=<t> slot=<n>` and apply: set the
  configured mode (it already holds CAP_SOVEREIGN), spawn the bring-up agent
  with the configured task into the configured slot.
- Missing file or unparsable content: fall back to the compiled defaults
  (sovereign/test/0) and print `PRADYOS_AETHER_CFG_DEFAULT` — the daemon must
  never fail to boot over a config problem.
- The DDR-701 mode-toggle self-check is unchanged (it ends by restoring
  SOVEREIGN; the shipped config also says sovereign, so gate behavior is
  identical).

**Image build** (`Makefile` fat-image): generate `build/aether.cfg` with the
default policy and `mcopy` it to `::/AETHER.CFG` next to the existing files.

## Gate — `smoke-aethercfg` (78 gates)

Asserts `PRADYOS_AETHER_CFG_OK mode=sovereign task=test slot=0` (the daemon
provably read and parsed the file — the DEFAULT sentinel is the forbidden
pattern) plus the downstream `PRADYOS_AGENT_DONE` (the configured spawn ran).
Forbidden: `PRADYOS_AETHER_CFG_DEFAULT`.

Regression: `smoke-aether*`, `smoke-mode`, `smoke-agents`, `smoke-agentmetrics`,
`smoke-fs` (FAT image gains a file), then the full suite.

## Non-goals

- No runtime re-read / SIGHUP-style reload — boot-time policy only.
- No new syscalls; pure ring-3 + image plumbing.
- Agent mem/rate limits stay kernel defaults (SYS_SET_MEM_LIMIT is lower-only
  and per-process; a config knob for it is a later slice if wanted).

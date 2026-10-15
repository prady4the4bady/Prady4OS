# DDR-1166 — Repository audit: feature status, duplicate trees, chipset log path

Status: accepted, built. Date: 2026-10-14.

## §0 Why

Operator instruction (2026-10-14): orient, re-measure, write
`docs/FEATURE_STATUS.md` from the code, and audit `user/` against `userspace/`
and `aether/` against `kernel/aether/` for duplication.

## §1 OPEN-2 state, re-read rather than inherited

* The double-dispatch mechanism (DDR-1139) is fixed. Its confirmation was
  pre-registered and met: 2,200 boots, 0 `[schedcheck]` hits.
* Later hunts on this kernel family:
  * DDR-1154: 6,545 of 11,000 boots run, 0 signals.
  * Run 38109673379 (2026-10-11): 20 lanes × 55 runs. 20/20 lanes `success`, and
    lane 5's DONE line reads `signal_runs=0 churn_runs=55`.
  * None of these produced an artefact.
* The residual signatures U1/U2/U3 (DDR-1155) each have **n=1 and no live
  artefact**.
* Non-negotiable 3 forbids a fix without a measured mechanism. So there is
  nothing to fix *first*. The honest state is "principal mechanism fixed and
  confirmed; three residual signatures unattributed; hunts clean".
* OPEN-2 is **not** declared closed. Per `docs/HANDOFF.md`, that claim is the
  operator's to make.

## §2 Duplicate trees

### `userspace/` is deleted

* It held three empty directories (`init/`, `libc/`, `shell/`), each with only
  a `.gitkeep`.
* Nothing references it: `grep -rn userspace/` over `Makefile`, `tools/` and
  `.github/` finds only the unrelated gate name `smoke-iso-userspace`.
* The real ring-3 tree is `user/`, which has 85 files.
* An empty sibling with a plausible name is a trap: a session can put code
  there and nothing will build it. It is deleted, and `docs/RECOVERY_AUDIT.md`
  no longer lists it.

### `aether/` is kept, with no change

* `aether/` is the **host-side Python layer**: build provenance, JSONL audit
  log and invariants. It runs on the developer machine and is tested by CI's
  `pytest aether/tests` job.
* `kernel/aether/` is the in-kernel C policy engine.
* They share a name and a concept (an audit log). They share no code, and
  neither is built from the other.
* This is not duplication. Merging or deleting either would remove a working,
  tested component. The disposition is recorded so it is not re-derived.

## §3 `smoke-chipset` log path: two defects in one line

The recipe wrote each variant's log to `/tmp/cs_$$m_$$c.log`. That line had two
problems:

1. It wrote to `/tmp`, against NON-NEGOTIABLE 7.
2. `$m_` is parsed by the shell as a variable named `m_`, which is unset. So
   the path collapsed to `/tmp/cs_<cpu>.log`, and `q35/qemu64` and `pc/qemu64`
   wrote **the same file**. A failure on one variant could have printed the
   other variant's log.

Fix: `build/gatelogs/chipset_${m}_${c}.log`, created with `mkdir -p`.

Measured: `make smoke-chipset` passes all 4 variants and leaves **four**
distinct files.

## §4 Free-range carriers

All four carriers still read `DDR-1154+` while DDR-1154 through DDR-1165 were
in use. They now read `DDR-1167+`. DDR-1166 is this record; DDR-1167 is reserved
for USB piece 6.

## §5 Not claimed

* No kernel change: `kernel.bin` is `d8d48f39c25e0ff9` before and after.
* No gate added. The gate count stays at 188.
* OPEN-2 is not closed.

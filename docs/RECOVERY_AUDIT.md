# RECOVERY AUDIT — everything built, mapped against the sovereign architecture spec

**Phase R-00.** Date: 2026-07-27. Repo: `prady4the4bady/Prady4OS`, branch
`dev/phase1` @ `29471d2`.

Instruction: *"Expected mapping (you must verify each one against actual files)."*
This document reports what verification found. It is an audit, so it records what
is there rather than confirming what was expected.

## Headline finding

**None of the artifacts the recovery brief describes exist in this repository.**
There is no Buildroot ISO, no Docker stack, no GRUB, no systemd, no FastAPI
services. The check is not a judgement call:

```
git ls-files | grep -icE "buildroot|docker|grub|systemd|fastapi|\.iso$"   ->  0
```

Zero tracked files match any of those terms. Every path named in the
KEEP / REPURPOSE / RETIRE mapping was probed individually and is absent.

## Verification of every path named in the brief

| Path from the brief | Bucket assigned | Actually present? |
|---|---|---|
| `ai-core/prax-brain/` (8 modules, 21 tests) | KEEP AS-IS | **ABSENT** |
| `ai-core/neila/` (Ouroboros fork, 30 tests) | KEEP AS-IS | **ABSENT** |
| `ai-core/ahnis/` (MemPalace fork, 37 tests) | KEEP AS-IS | **ABSENT** |
| `platform/inventor-engine/` (55 tests) | KEEP AS-IS | **ABSENT** |
| `platform/proposal-gate/` | KEEP AS-IS | **ABSENT** |
| `build/iso/` | KEEP AS-IS (substrate) | **ABSENT** |
| `docs/HONEST_LIMITATIONS.md` | KEEP AS-IS | **ABSENT** |
| `docker-compose.dev.yml` | REPURPOSE | **ABSENT** |
| `model-gateway` | REPURPOSE | **ABSENT** |
| `agent-runtime` (Prax) | REPURPOSE | **ABSENT** |
| `workflow-engine` | REPURPOSE | **ABSENT** |
| `audit-log` service | REPURPOSE | **ABSENT** (see note) |
| `watchdog` service | REPURPOSE | **ABSENT** |
| `notification-bus` | REPURPOSE | **ABSENT** |
| `CONTRIBUTING.md` ("44-service Docker stack") | RETIRE | **ABSENT** |
| `grub.cfg` | RETIRE (archive) | **ABSENT** |
| `kryos_defconfig` | RETIRE (archive) | **ABSENT** |
| `.github/workflows/ci.yml` | KEEP | **PRESENT** |

Note on `audit-log`: no such *service* exists, but `aether/kernel/audit/audit_log.py`
does — it was built here two commits ago (B-02), which is the merge target the
brief describes. That target already exists; there is nothing to merge into it.

## What this repository actually contains

| Area | Tracked files | What it is |
|---|---|---|
| `kernel/` | 122 | **NEXUS** bare-metal x86_64 kernel — C + NASM. Clean-room, no Linux lineage |
| `docs/` | 164 | ADRs, DDRs, build status, master feature doc |
| `aether/` | 31 | The Python agent layer started this session (B-01…B-04) + placeholder dirs |
| `user/`, `userspace/` | 33 | Ring-3 programs incl. the PRISM shell (static ELF, musl subset) |
| `tools/`, `arch/`, `boot/`, `fs/`, `drivers/` | ~50 | Toolchain, bootloader, arch code |

`README.md` already opens with:

> "A clean-room, bare-metal, AI-native operating system: the **NEXUS** kernel …
> **No code lineage from Linux/BSD.**"

It contains **zero** occurrences of "Linux distro", "Buildroot", "ISO", "Docker",
or "Prady OS".

## Status of the recovery phases against this tree

| Phase | Requirement | Status here |
|---|---|---|
| **R-00** | Audit built work against spec | **This document.** Mapping cannot be applied — the mapped files do not exist |
| **R-01** | "Push B-01/02/03 — never pushed" | **Already done before this brief.** `2390a84` (B-01/02/03) and `29471d2` (B-04) are on `origin/dev/phase1`; 14 files under `aether/kernel/` are on the remote |
| **R-02** | Remove Linux-distro positioning from README | **No such positioning exists** to remove. `docs/ARCHITECTURE.md` does not exist and could be written, but Layer 0 of the proposed model ("the Buildroot Linux layer") has no referent here |
| **R-03** | Resume B-series | **B-04 is already built and green.** B-05 is genuinely next |
| **R-04** | Add `test-aether-kernel` / `test-aether-agents` CI jobs | **Genuinely actionable** — the Python gates are not yet wired into `ci.yml` |

The one number in the brief that matches is CI volume: the repo has **441**
workflow runs. Those runs are the NEXUS kernel's QEMU boot gates, not ISO builds.

## Conflicts requiring a decision (the brief says to stop and report these)

1. **The KEEP/REPURPOSE/RETIRE mapping has no referent.** Nothing can be kept,
   repurposed, or retired, because none of it is here. Most likely this brief is
   aimed at a different repository — the same `prady-os` vs `Prady4OS` divergence
   that came up earlier in this session.
2. **A third, conflicting invariant set.** The brief defines S1–S8 ("every agent
   interaction mediated by AETHER", "zero visible CLI", "the Linux layer is
   substrate"). One instruction earlier, Decision 3 established that Section E's
   **S1–S14** is authoritative for the AETHER layer and must **not** be merged
   with the C kernel's own S1–S8. There are now three numbered sets in play and
   two of them claim the same range. `aether/kernel/invariants/core_invariants.py`
   currently implements S1–S14 as instructed.
3. **Test layout.** The brief asks for `aether/kernel/lockbox/tests/…`; the
   previously-supplied directory layout put every gate in `aether/tests/`, which
   is where they are. One convention should win.
4. **Gate strictness.** The brief asks for `-W error::DeprecationWarning`. The
   gates currently run `-W error`, which is **stricter** (all warnings are
   errors). Kept as-is; loosening it would weaken S7/"0 warnings".

## Recommendation

Do **not** archive, retire, or rewrite anything on the basis of the mapping —
there is nothing matching it to act on, and acting anyway would damage a
clean-room kernel that is not what the brief describes. Confirm the target
repository, then re-issue R-00 against it. Meanwhile the genuinely actionable
items here are **R-04** (wire the Python gates into CI) and **R-03** (continue at
B-05), both of which apply regardless of how the identity question resolves.

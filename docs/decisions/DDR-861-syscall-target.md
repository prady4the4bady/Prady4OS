# DDR-861 — the v1.0.0 syscall target is ~100, not 200+ (Group 3 item 21)

**Status:** Accepted
**Date:** 2026-08-07
**Scope:** Assessment. No code change.

The brief asked for this explicitly: *"assess and report a realistic v1.0.0
target rather than blindly targeting 200+; implement only what's functionally
required by other items in this queue."*

## Measured, not estimated

| | count |
|---|---|
| `#define SYS_*` in `kernel/syscall/syscall.h` | **90** |
| handlers actually `syscall_register(...)`-ed | **85** |
| highest NSI in use | **93** |
| `MAX_SYSCALLS` table size | **128** |

The 90-vs-85 gap is not drift: `SYS_*` numbers are also defined for the ring-3
side of calls whose kernel half is registered under a different translation
unit's name, plus the reserved/superseded numbers DDR-840 and DDR-860 record.
It is worth stating because a naive reading of either number alone is wrong.

## Why "200+" is the wrong target

200+ is a **blueprint** figure, and a blueprint counts a mature POSIX surface —
the full `*at()` family, xattrs, quotas, aio, timerfd/signalfd/eventfd, sched
tuning, extended mount options. PRADYOS needs almost none of that to ship an
honest x86_64 v1.0.0, and every syscall added is permanent surface: an ABI to
keep, a fuzz target (`smoke-syscallfuzz`), and a capability decision.

**Counting to 200 would mean adding ~110 calls nothing in this queue needs.**
That is not completeness; it is unbounded attack surface with no caller.

## What the remaining queue actually requires

Derived by walking Groups 3–9 item by item, not by rounding:

| item | new syscalls | why |
|---|---|---|
| 15 service manager | ~3 | start / stop / status, capability-gated |
| 19 6-arg ABI widening | 0 | widens the *signature*, adds no number |
| 20 non-zero `ftruncate` | 1 | **absent today** — no `ftruncate` anywhere in `kernel/` |
| 34 shell job control | ~3 | `setpgid`/`getpgid`/`tcsetpgrp`; `SYS_KILL` and `SYS_SIGACTION` already exist |
| 37 NUMA affinity hints | 1 | `sched_setaffinity` |
| 40 PRADYOS Drive | ~1 | a mount/attach entry point |
| 22–28 drivers, 29–31 FS | 0 | device and FS work lands under existing VFS/blk entry points |
| 32 musl header, 35 linker, 36 PRISM, 38 prad | 0 | userspace; `mmap`/`open` already suffice |
| Groups 8–9 asm, ISO, release | 0 | no new surface |

**Total ≈ 9 new calls → a v1.0.0 target of ~99, comfortably inside the existing
128-entry table.** No table growth, no `MAX_SYSCALLS` change, and DDR-823's
`syscall_register` panic on `num >= MAX_SYSCALLS` stays the backstop.

## Decision

**Target ~100 for v1.0.0. Add a syscall only when an item in this queue needs
it**, and record which item required it. 200+ is explicitly *not* a goal and
should not be treated as an unmet one in any release note.

## The number that would actually be worth reporting

Syscall count is a poor completeness metric — it measures surface, not
capability. A more honest release figure is **how many syscalls are covered by a
gate**, since `smoke-syscallfuzz` already exercises the table for robustness but
not semantics. That is not built here (it would be its own item), but it is the
measurement a future assessment should prefer over a count.

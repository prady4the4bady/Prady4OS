= DDR-879 — the v1.0.0 syscall surface: 91 today, ~95 at release, not 200+ (item 21)

**Status:** Accepted — assessment, as the item asks for
**Date:** 2026-08-09
**Scope:** `kernel/syscall/syscall.h` (NSI table). No code change.

## The question, restated

The blueprint carries a long-term number of **200+** syscalls. Item 21 asks for a
realistic v1.0.0 target rather than that number pursued blindly, and says to
implement only what other items in this queue functionally require.

## Where the count actually stands

**91 NSI numbers defined, 91 registered.** Not 76 — that figure predates several
slices. The two counts matching is itself worth stating: a defined-but-
unregistered number is a syscall that returns `-ENOSYS` while looking present in
the header, and the gap between the two lists is where that hides.

The surface is not one flat list. It is four:

| Group | Count | Character |
|---|---:|---|
| POSIX process/file/memory core | 31 | `read`/`write`/`open`/`mmap`/`fork`/`execve`/`wait4`/`epoll`/signals/io_uring |
| AETHER agent + capability | 26 | actions, approvals, agent lifecycle, vault, ACC, audit, checkpoints |
| Layer-7 desktop (surface/fb/input) | 22 | `SURFACE_*`, `FB_*`, `INPUT_POLL`, `MOUSE_POLL` |
| Introspection + system | 12 | `SYSINFO`, `DMESG`, `MEMINFO`, `GETPROCS`, `TIME`, `POWEROFF` |

## Why 200+ is the wrong target for v1.0.0

Linux's ~350 x86_64 syscalls are mostly **variants and compatibility strata**:
`open`/`openat`, `stat`/`fstat`/`lstat`/`fstatat`/`statx`, `select`/`pselect6`,
32-bit and `time64` doubles, and a long tail kept alive only by binaries
compiled decades ago. PRADYOS has no compatibility stratum to carry — nothing
was ever compiled against an older PRADYOS ABI.

Counting to 200 without that pressure means **inventing** syscalls. Each one
invented is a number that is permanent (NSI numbers are contract), a handler
nobody calls, and a gate nobody wrote — while `-Wunused-parameter` and the ABI
freeze make every one of them a real maintenance surface. A syscall count is an
output of what userspace needs, never an input.

There is a concrete precedent in this queue: `SYS_MMAP` sat at four arguments
for several slices, silently discarding `fd` and `offset` (DDR-877). The defect
was in a syscall that already existed and was already counted. Adding a hundred
more numbers would have produced a hundred more places for that shape of bug,
and a higher number on the tracker.

## The target: ~95, and it is derived, not chosen

Only the remaining queue items imply new numbers, and only these do:

| Item | Likely new NSI | Why it is genuinely new |
|---|---|---|
| 15 service manager | `SERVICE_START` / `SERVICE_STOP` / `SERVICE_STATUS` | capability-based supervision replaces init's direct reap; no existing call expresses it |
| 34 job control | none | `kill`/`wait4`/`sigaction` already exist; job tables are a shell-side structure |
| 35 dynamic linker | none | `mmap` + `open` + `read`; a loader needs no privileged operation |
| 38 prad | none | file + network syscalls already present |
| 40 PRADYOS Drive | none | it is a VFS mount, which is kernel-side |
| 27 ACPI S3 | maybe 1 (`SUSPEND`) | only if suspend is triggerable from ring 3 rather than by the kernel |

**91 + 3 (service manager) + 1 (conditional suspend) ≈ 95.**

Anything beyond that at v1.0.0 would be speculative. `pread`/`pwrite`,
`MAP_FIXED`, file-backed `mmap`, `mprotect`, `readv`, `statx`, and threads
(`clone`/`futex`) are all real and all defensible — as **post-1.0 items with
their own gates**, not as numbers reserved now against a future that has not
been designed.

## Recommendation

**Target ~95 for v1.0.0.** Retire "200+" from the tracker as a long-term
aspiration rather than a release criterion, and treat the syscall count as a
reported measurement, never a goal. A release should be able to say "every
number in the table has a handler, a caller, and a gate" — which 91/91 currently
allows and 200 would not.

The blueprint's 200+ is not wrong as a ten-year shape for a system that grows
threads, namespaces, a full VM subsystem and a network stack API. It is wrong as
a thing to hit by v1.0.0.

**Group 3 item 21 complete (assessment delivered; no code change, by design).**

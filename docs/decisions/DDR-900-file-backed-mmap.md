= DDR-900 — file-backed mmap (item 35.1, prerequisite for the dynamic linker)

**Status:** Design. Implementation not started.
**Date:** 2026-08-11
**Scope:** `kernel/syscall/sys_mmap.c`, `kernel/fs/vfs/`, `smoke-mmapfile`.
**Blocks:** DDR-897 sub-phases 35.2–35.6 — the loader cannot map a shared object
without this.

## 1. Where this starts

`sys_mmap` is anonymous-only today and **refuses `fd != -1` with `-ENOSYS`**
(DDR-877). That refusal was correct: silently ignoring `fd` returned zero pages
for "map this file", which is the absorb-bad-input defect. This DDR makes the
refusal unnecessary rather than deleting it.

## 2. Supported subset, and what stays refused

**Supported:** `MAP_PRIVATE` with a file, `PROT_READ`, `PROT_READ|PROT_WRITE`,
page-aligned `offset`, regular files on any mounted VFS filesystem.

**Refused — each with its own errno, never a shared one:**

| Case | Errno | Why |
|---|---|---|
| `MAP_SHARED` with a file | `-ENOSYS` | shared write-back needs a page cache with dirty tracking; none exists |
| `PROT_EXEC` | `-EINVAL` | ADR-021 W^X. The loader maps text through a separate path that sets X only after relocation, never via a W-capable mapping |
| unaligned `offset` | `-EINVAL` | POSIX requires page alignment |
| directory or special file | `-EACCES` | only a regular file has a byte extent to map |
| `offset + len` past EOF | **partial map, zero-filled tail** | POSIX behaviour; the tail must read as zero, not as stale frame content |

## 3. Eager population, not demand paging

Pages are read at `mmap` time, not on fault.

Demand paging needs a fault handler that can perform **blocking I/O inside a page
fault**. The `#PF` path runs in the faulting thread's context with no provision
for sleeping on a block device, and making it sleepable touches the same
scheduler paths item 16 is currently unresolved in. Eager population is slower
for sparse access and completely avoids that coupling.

Consequence, stated plainly: a 4 MiB mapping costs a 4 MiB read up front.
Acceptable here — the consumer is the dynamic linker mapping shared objects it
will read in their entirety anyway.

## 4. No page cache; private frames

Each mapping gets **private frames** populated by `vfs_read`. With no page cache,
two processes mapping the same file get two copies.

That is memory-inefficient and it is the right v1 trade: a shared cache means
dirty tracking, write-back ordering, and coherency with `vfs_write` — a
filesystem subsystem, not an mmap feature. `MAP_PRIVATE` semantics remain
**correct** (writes stay private and never reach the file); only the sharing is
absent, which is invisible to correctness.

`PROT_WRITE` therefore needs no COW machinery: the frames are already private, so
a write simply writes. This is also why `MAP_SHARED` is **refused rather than
approximated** — approximating it would silently lose writes another process
expects to observe.

## 5. Accounting and bounds

Charged against `aether_mem_charge` exactly as anonymous mmap is (ADR-026 D5),
and bounded by the same `VM_AREA_MAX` slot table. A file mapping must not become
a way around the per-agent memory cap.

## 6. The gate must prove CONTENT

`smoke-mmapfile`:

1. Map a known file; assert the mapped bytes **equal the file bytes**. A mapping
   that returned zero pages would pass any "the call succeeded" check — exactly
   the DDR-877 defect this replaces.
2. Map at a non-zero page-aligned offset; assert the correct region.
3. Map past EOF; assert the tail reads **zero**.
4. Write through `PROT_WRITE`; assert the **on-disk file is unchanged**.
5. Refusal arms asserting **specific** errnos: `MAP_SHARED`, `PROT_EXEC`,
   unaligned offset, directory.

Distinct errnos per arm matter: asserting only "it failed" passes even when the
wrong check fired — the trap DDR-866 recorded with `-EINVAL` versus `-EIO`.

## 7. Sequencing

35.1 lands and is gated **before** 35.2 (`PT_INTERP`/auxv). The loader is
untouchable until a shared object can be mapped at all.

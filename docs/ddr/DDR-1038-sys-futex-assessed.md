# DDR-1038 — `SYS_FUTEX` assessed: NOT buildable, and building it would be worse than omitting it

**Status:** ASSESSMENT — no code. Queue item deferred with a named blocker.
**Date:** 2026-09-02
**Queue:** Group D, the `SYS_FUTEX` half of the row `SYS_MPROTECT`/`SYS_POLL`/`SYS_FUTEX` (the first two shipped as DDR-1031 and DDR-1037).

---

## §1 — What a futex needs, and what this kernel has

A futex is a wait queue keyed on **a word of memory two schedulable entities can
both see**. `FUTEX_WAIT` sleeps if the word still holds an expected value;
`FUTEX_WAKE` is issued by whoever changed it. Without a genuinely shared word,
`WAIT` blocks on something nobody else can observe or modify, and `WAKE` has no
caller.

**Measured, not assumed:**

| requirement | state in this kernel |
|---|---|
| a second thread in one address space | **absent.** `SYS_CLONE`, `CLONE_VM`, `CLONE_THREAD` return **zero** matches across `kernel/`. One thread per process. |
| shared anonymous memory between processes | **explicitly refused.** `sys_mmap.c:83` — `if (!(flags & MAP_ANONYMOUS) \|\| (flags & MAP_SHARED))` rejects the call. The file header states the baseline as `MAP_ANONYMOUS \| MAP_PRIVATE` only. |
| a writable page surviving `fork` as shared | **no.** `fork` downgrades writable pages to read-only and tags `PTE_SW_COW` in both address spaces (`vmm_cow.c:5-6`), so the first write un-shares the word — precisely the write a futex protocol makes. |

The only frames genuinely shared across address spaces are the vDSO, the
framebuffer and the metric page, all carrying `PTE_SW_SHARED`. Every one is
kernel-owned, and the metric page is **read-only to ring 3 by design** (DDR-812).
None is a place a futex word could live.

## §2 — The conclusion, and why it is a refusal rather than a narrow build

A `SYS_FUTEX` shipped today would be **unreachable by any correct program**.
Not "limited", not "baseline" — there is no pair of entities that can share the
word it operates on.

That is the shape `aether.h` already names for the six absent 3C action types:
*"Declaring an enum value with no enforcement is worse than omitting it — an
agent could submit one and the kernel would queue an action nothing
implements."* A futex whose `WAIT` can never be woken is the same defect with a
syscall number instead of an enum value.

It is also the shape DDR-877 called **"worse than incomplete"** for `mmap`'s
dropped `fd`/`offset`: an interface that accepts its arguments, returns success,
and does nothing the caller asked for.

## §3 — The blocker is named, and it is already in the queue

`SYS_FUTEX` is unblocked by **either** of two items that already sit in the
Group D backlog:

1. **`pthreads` / `clone(CLONE_VM|CLONE_FILES|CLONE_THREAD)`** — a second thread
   sharing the address space, which is the classic futex user; **or**
2. **`MAP_SHARED` anonymous mmap** — two processes mapping one frame, which is
   the other.

Either makes a futex word real. Neither exists. So this is a **dependency**, not
a deferral for lack of time, and the queue entry should say which item unblocks
it rather than leaving a future session to re-derive §1.

## §4 — What this DDR is NOT

- Not a claim that futex is hard. The wait-queue machinery exists —
  `sched_block_timeout()` (DDR-955, `sched.c:1434`) is the bounded-wait
  primitive a futex would use, with four callers already.
- Not a claim it should never be built. It should be built **after** either
  blocker in §3, and at that point it is a normal piece of work.
- Not a licence to skip the rest of the row. `SYS_MPROTECT` (DDR-1031) and
  `SYS_POLL` (DDR-1037) shipped; only this third one is blocked.

**Third assessment of this kind**, and the pattern is worth naming: DDR-1017
recorded `ACTION_SEND_IPC` as blocked on a missing ring-3 door, DDR-1021
recorded `ACTION_RUN_EXPERIMENT` as blocked on a missing executor — and **both
were later built once the missing piece was identified precisely enough**
(DDR-1033, DDR-1034). Naming the blocker exactly is what made those buildable.
This DDR names the blocker for futex in the same terms.

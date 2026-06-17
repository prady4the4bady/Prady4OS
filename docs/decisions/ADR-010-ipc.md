# ADR-010: IPC (NIA) — synchronous, capability-gated first

- **Status:** Accepted 2026-06-18 (user-approved scope)
- **Phase:** 2c

## Context

The Layer-2 board's IPC fabric has three primitives: synchronous call gates,
lock-free async SPSC ring buffers, and the sovereign broadcast bus. Building all
three at once is a large slice, and synchronous IPC also needs scheduler
block/wakeup (deferred from the scheduler slice). The user chose to build
synchronous message passing first, on top of capabilities.

## Decision

- **Scheduler block/wakeup** added: `THREAD_BLOCKED` state; `sched_block()` marks
  the current thread blocked and switches away; `sched_unblock()` makes it
  runnable again; `schedule()` now skips non-runnable threads (idle is always
  runnable, so progress is guaranteed).
- **Synchronous one-slot endpoint** (`kernel/ipc/ipc.{c,h}`): a receiver blocks
  until a sender delivers a small fixed message; the sender wakes a waiting
  receiver.
- **Capability-gated and resource-bound:** every send/recv calls
  `cap_authorize(caps, h, RES_IPC, endpoint.res_id, CAP_IPC_SEND|RECV)` — the
  capability must refer to *this* endpoint and carry the right. This is why
  `cap_authorize` (resource-bound check) was added to the NCS.
- **Lost-wakeup race** closed with a `cli`/`sti` critical section around the
  check-and-block (receiver) and deliver-and-wake (sender). Valid on the current
  single CPU; an SMP-safe lock replaces it when we add SMP. `context_switch`
  preserves per-thread RFLAGS, so a blocked receiver resumes with interrupts
  still masked and re-checks the condition before `sti`.

## Consequences / deferred

- Async lock-free SPSC ring buffers and the sovereign broadcast bus are the next
  two IPC slices (per the user, not combined).
- One message slot, fixed 4-word payload — enough to prove the path; larger /
  zero-copy (shared-memory descriptor) transfers come with the async work.
- Sender does not block on a full slot yet (single outstanding message in the
  demo); a full rendezvous (sender waits for receiver) is a small extension.

## Verification

QEMU: receiver blocks, sender delivers and wakes it, receiver reads the exact
message; a recv-only capability is refused for send. smoke PASS; `-Werror` clean.

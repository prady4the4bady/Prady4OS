# Lifetime / use-after-free audit — 2026-10

Operator-requested defensive reliability review (PR #27 comment 5971011693,
OWNER, 2026-10-03), to run **after** Fix 1 (DDR-1156) and Fix 2 (DDR-1157) had
landed. This is a **read-only** audit: no kernel code was changed while writing
it. Per the operator, **no finding here claims to close U1, U2, U3 or OPEN-1** —
each VERIFIED finding is a real defect in its own right; whether any of them
*also* contributes to an open signature is stated separately and conservatively.

Every finding is marked **VERIFIED** (code read, lines quoted) or **PROPOSED**
(reasoned, not yet proven from a reproduction). Nothing is implemented here;
worth-fixing findings get a ranked list and a proposed DDR, and wait for
operator approval before any fix PR.

## Audited tree

- Branch `fix/blk-slotwait-checkpoint`, commit `ebba28c` (the PR #29 tip:
  Fix 1 + Fix 2 landed).
- `build/kernel.bin` = `f5124d9b145df8cb`, 1,474,954 B.
- Line numbers below are against this tree. (The PR-#29 stack is not yet on
  `dev/phase1`, so a reader on `dev/phase1` will see different line numbers —
  the quotes, not the line numbers, are authoritative.)

## Method

Per the operator's prescription: mechanical search per bug class, then read the
hits in context.

- Class 1 (thread pointers that outlive the thread): enumerated every
  `struct tcb *` field (`next`, `waiter`, `rq_next`, `blk_wait_next`) and every
  stored `tcb *` outside the ring (request `waiter`, slot `slot_head`/`tail`,
  IPC `waiting_receiver`, roster lookups).
- Class 3 (raw pointer returned after a lock drop): every `struct tcb *…(` that
  looks something up — `sched_find_pid`, `find_pid_locked`, `sched_find_child`,
  `tcb_by_pid`, `rq_take`/`rq_pop` — and every caller's dereference.
- Class 4 (DMA after give-up): every device that writes memory — virtio-blk,
  virtio-net, e1000e, NVMe, AHCI — on its timeout/error path.
- Class 5/6 (free sites, late completions): `kfree`/`pmm_free`/`sched_free_tcb`
  and every `sched_block_timeout` / wake site.
- Class 7 (new-field init, NON-NEGOTIABLE 10): `sched_create_state` initialisers.
- Class 8 (lock-with-IF-off that waits cross-CPU): the known OPEN-2 shape.

No static analyser was added (the operator forbade new tooling without a DDR,
and none is already wired into CI for C). The existing `-Werror` clang build is
the only static gate and it is clean.

---

## VERIFIED findings

### F1 — virtio-blk: a late completion prematurely wakes the request that REUSED the slot — VERIFIED

- **File/lines:** `kernel/drivers/blk/virtio_blk.c`, `submit()` data-completion
  timeout branch `:468-473`, and `complete()` `:143-162`.
- **Pattern:** 6 (late completion acting on a reused slot) + 4 (device DMA after
  the driver gave up).
- **What is there.** On a per-request **data**-completion timeout the driver
  does (`:468`):
  ```c
  v->req[s].used = 0;
  v->req[s].waiter = 0;
  slot_wake_one(v);
  spin_unlock_irqrestore(&v->compl_lock, fl);
  return -EIO;
  ```
  It does **not** clear `v->head2slot[head]`, does **not** `virtq_free_chain`
  the head, and does **not** reset the device. The descriptor chain for the
  timed-out request is still **owned by the device**, and `head2slot[head]`
  still maps that head to slot `s`.
- **What could go wrong.** Slot `s` is now free and is immediately handed to a
  new submitter (`slot_wake_one`, or any new `submit`). The new request takes a
  **different** head (the old descriptors were never freed, so `virtq_add`
  cannot re-allocate them) and sets `head2slot[new_head] = s`, `req[s].head =
  new_head`. When the device **finally** completes the old head, `complete()`
  (`:145`) reads `s = v->head2slot[old_head]` — still `s` — and runs
  (`:150-161`):
  ```c
  if (!v->req[s].used || v->req[s].head != head)
      v->late++;            /* DDR-1148 flags it ... */
  v->req[s].done = 1;       /* ... but STILL marks the NEW request done */
  if (v->req[s].waiter) { … sched_unblock(w); }   /* and wakes the NEW waiter */
  ```
  The `v->req[s].head != head` guard only **counts** the stale completion
  (DDR-1148's `late=`); it does **not** gate the `done = 1` / wake. So the new
  request's submitter is woken as "complete" before the device has finished it,
  then reads `*status` for its own slot — which the device may not have written
  — and returns success-with-garbage (status 0) or `-1` (prefill 0xFF). And the
  device, when it completes the **old** request, writes the old status byte at
  `reqbuf + s*32 + 16`, which is now the **new** request's status location:
  a device DMA write racing the new submitter (class 4).
- **Which signature.** The data-completion timeout is a 5 s pathological event
  (`sched_block_timeout(&v->compl_lock, &v->req[s].done, 500)`, `:401`), which
  is exactly the state an OPEN-2 AP-freeze induces (a frozen AP delays
  completions past the deadline). So this is a plausible **downstream amplifier**
  of the `[blk] multi-inflight FAIL` / block-integrity family once a timeout has
  already happened — **not** a root cause, and **not** claimed to close anything.
- **Confidence.** Mechanism real: **high** (code-read, the guard demonstrably
  does not gate the wake). Relevance to an open signature: **low-to-medium** (it
  only bites after a 5 s timeout has already occurred).
- **Fix shape (PROPOSED).** In `complete()`, gate the `done = 1` / wake on the
  head matching: if `!v->req[s].used || v->req[s].head != head`, `late++`,
  `virtq_free_chain`, clear `head2slot`, and **return without touching
  `req[s].done`/`waiter`**. This preserves DDR-1148's "count, do not change a
  stale completion" intent while stopping the stale completion from *completing
  someone else's request*. Needs its own DDR, a gate that reuses a slot across a
  forced timeout, and a mutation check.

### F2 — AETHER `tcb_by_pid`: the unfixed sibling of Fix 2 (raw `tcb *` dereferenced after an UNLOCKED ring walk) — VERIFIED

- **File/lines:** `kernel/syscall/sys_aether.c`, `tcb_by_pid()` `:42-51`
  (no lock), dereferenced in `sys_kill_agent()` `:320-326` (a **write**:
  `t->sig_pending |= (1ull << SIGKILL)`), in `sys_agent_metrics()` `:276-283`
  (reads `t->run_ticks`, `t->dispatches`, `t->state`, `t->mem_used`), and the
  unlocked walk alone in `roster_active()` `:55`.
- **Pattern:** 3 (raw pointer after lock drop) + 1 (pointer outliving the
  thread) + 5 (dereference racing a free).
- **What is there.** `tcb_by_pid` walks `current_thread->next` with **no
  `g_sched_lock` / `irq_save`** — byte-for-byte the shape `sched_find_pid` had
  before Fix 2:
  ```c
  static struct tcb *tcb_by_pid(uint32_t pid) {
      struct tcb *t = current_thread;
      do { if (t->is_user && t->pid == pid && …) return t; t = t->next; }
      while (t != current_thread);
      return 0;
  }
  ```
  `sys_kill_agent` then does `t = tcb_by_pid(a1); if (!t) return -ESRCH;
  t->sig_pending |= …;` and `sys_agent_metrics` reads several `t->` fields.
- **What could go wrong.** The reaper frees a TCB only after
  `sched_ring_unlink()` under `g_sched_lock`, then `sched_free_tcb()` **outside**
  the lock (`reaper_thread`, `sched.c:2596-2604`). Between `tcb_by_pid`
  returning `t` on one CPU and the dereference, the target can exit → become
  `THREAD_ZOMBIE` → be reaped/freed on another CPU, and the `|=` write or the
  reads land in freed, `PMM_POISON`-stamped memory. This is **exactly** the UAF
  Fix 2 (DDR-1157 §1-2) closed for `sys_checkpoint`/`sys_resume`, present
  unchanged in the AETHER kill/metrics path. The interrupts-off state of the
  syscall's own CPU does **not** help — the free is on another CPU, and only
  `g_sched_lock` (which `tcb_by_pid` does not take) serialises against it.
  Additionally the walk itself is unlocked, so it can follow a `->next` that
  `sched_ring_unlink` is rewriting (`roster_active`'s `!= 0` test included).
- **The project already knows this class and applied it everywhere but here.**
  `ipc_recv` (`kernel/ipc/ipc.c:65-76`) documents the identical hazard and
  guards it under the endpoint lock; `sys_wait4`/`sched_find_child` is safe by
  the reaper's parent-liveness rule (see Examined-safe below); Fix 2 fixed
  `sched_find_pid`'s callers. `tcb_by_pid` is the one lookup that still returns a
  raw pointer for a later cross-CPU dereference.
- **Which signature.** `sys_kill_agent` is the sovereign's kill switch on a
  runaway agent; `sys_agent_metrics` is polled by the compositor's agent panel.
  Both are live, SMP-reachable, and both touch a thread that is (by intent) about
  to die — the exit-racing-the-syscall window is real. **Not** claimed to close
  U1/U2/U3; it is an independent UAF. If anything it is a candidate *source* of a
  `#GP`/poison-read signature on the syscall path, but that is a hypothesis, not
  a claim.
- **Confidence.** Mechanism real: **high** (identical to the just-fixed Fix 2
  defect; code-read). Reachability: **medium** (needs the target reaped in a
  few-instruction cross-CPU window).
- **Fix shape (PROPOSED).** The Fix-2 remedy, applied here: either (a) a
  `sched_kill_pid(pid)` / `sched_agent_metric(pid, …)` helper that walks and acts
  under one `g_sched_lock` section (set `sig_pending`, or copy the counters into
  an out-struct) and returns a status/bool rather than a pointer; or (b) have
  `sys_aether` call the existing `find_pid_locked`-style pattern. Needs its own
  DDR, and a mutation check in the shape of Fix 2's M2 (drop the `-ESRCH`).

### F3 — NVMe: PRP pages freed on completion timeout with no controller reset — VERIFIED (bring-up only)

- **File/lines:** `kernel/drivers/nvme/nvme.c`, `nvme_submit()` returns `0xFFFF`
  on timeout `:217-229` **without advancing `cq_head`, without toggling phase,
  and without resetting the controller or reclaiming the posted command**; the
  self-test callers then free the DMA pages regardless —
  `nvme_selftest` `:336-337`, `:351`, and the identify path `:315-316`/`:333-334`.
- **Pattern:** 4 (DMA after give-up) + a CQ phase-desync hazard.
- **What could go wrong.** The SQ entry's doorbell was already rung, so the
  command is outstanding. On `0xFFFF` the caller `pmm_free_page(wp/rp)` /
  `pmm_free_pages(wq/rq, 2)` — the controller can still DMA into those (now
  freed, reallocatable) pages. Separately, because `cq_head`/`phase` were not
  advanced, the eventual CQE for the timed-out command will later be read as the
  completion of a **different** command (phase-bit desync).
- **Which signature.** **None of U1/U2/U3/OPEN-1.** NVMe is a bring-up/self-test
  path (DDR-765/766/772); the CI block hot path is virtio-blk. The timeout is
  5e7 `pause` iterations and QEMU never hits it. This is a real latent hazard for
  real/!responding hardware, not a contributor to the observed signatures.
- **Confidence.** Mechanism real: **high**. Relevance: **very low** (unreachable
  in CI; a real timeout would need a controller reset path the driver lacks).
- **Fix shape (PROPOSED, low priority).** A controller reset (or at least an
  abort + CQ drain) on timeout before freeing, or retaining the pages until the
  controller is known quiesced. Larger than F1/F2; defer unless NVMe moves onto a
  hot path.

---

## Examined and found safe (stated so the audit is an audit, not just a bug list)

- **`sys_wait4` / `sched_find_child`** (`sys_wait.c:49`, `sched.c:1439-1458`).
  `sched_find_child` walks under `irq_save()` and returns the pointer **after**
  `irq_restore()`, and `sys_wait4` dereferences it unlocked — but this is
  **safe**: the returned zombie is `self`'s own child, and the reaper frees a
  zombie only when `!waiter && !pid_alive(parent_pid)` (`reaper_thread`,
  `sched.c:2559`). The parent (`self`) is alive and is the child's parent, so the
  reaper is excluded; and a child has exactly one parent, so no second `wait4`
  can race the `sched_destroy`. `live->waiter = self` writes a live child that
  only `self` could free, and `self` is blocked in `wait4`. This is the model
  Fix 2 was built to match.
- **`main.c` `sched_find_pid` callers** (`:2250`, `:2823`, `:3484`). All are
  `while (sched_find_pid(pid) && g_ticks < dl)` **liveness polls** — the return
  is tested for non-NULL and **never dereferenced**. `main.c:3468` documents the
  discipline explicitly. Safe. (The walk is unlocked, so in principle it could
  follow a `->next` under rewrite; it runs on the BSP during bounded bring-up
  polls, and is the same residual-walk concern as F2's `roster_active`, noted
  there.)
- **IPC** (`kernel/ipc/ipc.c:45-78`). `ipc_recv`'s timeout path clears
  `e->waiting_receiver == current_thread → 0` under `e->lock` and documents the
  exact UAF it avoids. `ipc_send` wakes under the same lock. Safe, and the
  template F2 should follow.
- **Class 7 (new-field init).** Every `tcb` pointer/flag field added over time is
  explicitly initialised in `sched_create_state` — `on_cpu` `:1096`, `rq_next`
  `:1098`, `rq_on` `:1099`, `blk_wait_next` `:1119`, `waiter` `:1157`,
  `agent_caps` `:1165`, `is_net/ipc/exec/memory` `:1170-1181`, `checkpointed`
  `:1183`, `next` `:1211`. NON-NEGOTIABLE 10 is honoured for the TCB.
- **Class 2 (slot-registration unwind) on virtio-blk.** Both early-return paths
  in `submit()` release the slot and wake a waiter: the `virtq_add` failure
  (`:371-380`, with a comment that the original stranded waiters here) and the
  data timeout (`:468`). The slot bookkeeping unwinds; F1 is about the *head /
  device ownership*, not the slot.
- **e1000e RX timeout** (`e1000e.c:329-342`). The RX ring buffers are persistent
  and driver-owned; the timeout restores the callback and does not free a
  per-request buffer, so there is no DMA-after-free. A late RX lands in the ring
  and is handled normally.

---

## Ranked by how plausibly it bears on an open signature

1. **F2** (AETHER `tcb_by_pid` UAF) — highest. It is the *same* UAF class as the
   two fixes just approved, on a live SMP syscall path, dereferencing a raw
   pointer the reaper can free. Most likely of the three to produce a
   poison-read / `#GP` on the syscall path. **Worth fixing.**
2. **F1** (virtio-blk stale-completion premature wake) — medium. Real
   correctness defect; bites only after a 5 s completion timeout, which the
   OPEN-2 freeze is exactly the thing that induces. **Worth fixing.**
3. **F3** (NVMe free-on-timeout) — low. Real but unreachable in CI and off the
   hot path. **Worth fixing eventually**, not now.

Each worth-fixing finding will be proposed as its **own** branch + DDR + gate
(with a mutation check) + PR, per the operator's one-issue rule, and only after
approval — F2 as the natural "Fix 3", F1 as "Fix 4".

## What I could NOT check, and why

- **virtio-net TX timeout** (`virtio_net.c:132` "returns 0 on TX completion,
  -1 on timeout"): the TX give-up path's descriptor/netbuf reclaim vs a late
  device write was not read in full. Deferred to a second pass.
- **AHCI**: only the identify path's `pmm_free_page` was seen; the FIS/command
  timeout → DMA-after-free question was not audited. Deferred.
- **Full `user/` tree**: ring-3 programs were out of scope for a lifetime audit
  of kernel object frees; the operator's scope names `kernel/` and `user/`, and
  `user/` is a second pass.
- **Class 8 beyond OPEN-2**: a systematic "lock taken with IF off that then waits
  on another CPU" sweep is the OPEN-2 investigation itself (the double-dispatch,
  DDR-1139) and the `compl_lock`/`g_sched_lock` holders; no *new* instance was
  found in this pass, but it was not exhaustive.
- **cap table / fd table frees** (`cap.c:38 kfree(t)`, `fd.c:60/90 kfree`): the
  "is anything still linked?" question for these was only spot-checked (the TCB
  is the dominant case); a dedicated pass is owed.
- **Exhaustive class-2** across every non-block registration site (netallow,
  epoll, io_uring slots, vma) was not completed.

These are listed so the next pass has a precise starting point; none is a claim
that those paths are clean, only that they were not fully read here.

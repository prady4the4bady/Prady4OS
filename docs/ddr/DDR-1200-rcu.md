# DDR-1200 — C1.4 P5: RCU in the kernel, one grace-period scheme, first consumer the timer-tick ring sweep

**Status:** design. No code in this PR.
**Part:** C1.4 P5 (DDR-1181 §2: "RCU, one grace-period scheme, chosen in its own amendment"). This record is that choice.
**Asked by:** the operator, in this session: "Start C1.4 RCU: write its design record (DDR-1200) as a docs-only PR … If the RCU code would touch the kernel, say so and wait for me."
**Answer to that question: yes.** The code this record designs touches the kernel: the scheduler, the reaper, a new `kernel/proc/rcu.[ch]`, `kernel/main.c` (the torture test), the `Makefile` and the CI workflow. Under #45 comment 6655226539 it waits for the operator (§9).
**Honesty labels:** nothing here is built or measured yet. Every reading of the tree below is "by reading, at `733ac91`". QEMU TCG only; not verified on real hardware.

---

## 1. Where the tree is (by reading, at `733ac91`)

- **No RCU, no deferred free.** `grep -rniE '\brcu\b|grace.period|quiescent'` over `kernel/` finds nothing. Every kernel object is freed with `kfree` as soon as its last unlink is done.
- **The only cross-CPU lock is the test-and-set `spinlock_t`** (`kernel/include/spinlock.h:12`). `sched.c`'s `irq_save()` **is** `spin_lock_irqsave(&g_sched_lock)` (`sched.c:885`). The file-local `local_irq_save()` (`sched.c:896`) only masks IF.
- **The all-threads ring.** It is a circular, singly linked list through `tcb->next`. Its users:

| site | what it does | protection |
|---|---|---|
| `sched_create_state` (`sched.c:1269-1272`) | inserts (`t->next = cur->next; cur->next = t`) | `irq_save()` = `g_sched_lock` |
| `sched_ring_unlink` (`sched.c:1468`) | unlinks (`p->next = t->next`), **leaves `t->next` intact** | caller holds `g_sched_lock` |
| `sched_tick` (`sched.c:2192-2306`) | **reads the whole ring on every tick on every CPU**, in the timer ISR (DDR-955's timed-wake sweep) | `irq_save()` = `g_sched_lock` |
| `sched_snapshot`, `sched_find_child`, the `*_pid_*` helpers (DDR-1001, 1157, 1159) | read; some act on what they find | `g_sched_lock` |
| `sched_free_tcb` (`sched.c:1479`, reached from the reaper) | frees the tcb and its kernel stack, **after** the unlink | none needed today: every reader holds the lock |

- **So every ring reader is locked today**, and the lock is what keeps a freed tcb from being read. The price is that each timer tick on each CPU takes `g_sched_lock` and walks the ring (O(threads)) while holding it, in an ISR, even when no timed wait is due.
- **How hot that lock is (measured, but not at the tip):** DDR-1047 measured `g_sched_lock` as the most contended lock in the kernel, about 450 times the busiest runqueue lock (1,902,380 contended acquisitions in 5,000 ticks). That was before rq-2 removed `g_sched_lock` from the `schedule()` path (`sched.c:892-895`), so the figure at the tip is **not known**. The code PR re-measures it (§7, arm RE).

### 1.1 Found on the way: DDR-1096 §3's "unlocked ring walk" does not match the code

- DDR-1096 §3's table says `sched_tick`'s walk, `sched_snapshot`, `sched_destroy` and `reaper_thread` are protected by "`irq_save()` only", which "masks IF locally; on SMP it excludes nothing on another CPU". The debug-only comment at `sched.c:2197-2203` repeats it.
- **That reading is wrong, and the tree already recorded the same mistake once.** DDR-1001 §4 (written before DDR-1096) says: *"My first reading was that `sched_destroy`'s unlink is guarded only by `irq_save()` … **That is wrong** … `static inline uint64_t irq_save(void) { return spin_lock_irqsave(&g_sched_lock); }` It is a real spinlock."* The tip has the same definition (`sched.c:885`).
- **Not verified:** whether some commit between DDR-1001 and DDR-1096 defined `irq_save` differently. This clone is shallow (`git rev-parse --is-shallow-repository` = `true`), so the history is not available here.
- **What this changes, and what it does not:**
  - DDR-1097, 1115, 1116 and 1118 cite "DDR-1096 §3's unlocked ring walk" as a candidate OPEN-2 mechanism. By reading, the walk is locked, so that candidate has no mechanism at the tip.
  - **This does not change OPEN-2's status.** OPEN-2 is still not declared closed (CLAUDE.md, DDR-1139). Removing one hypothesis by reading is not a finding about the others.
  - The stale comment is corrected in the code PR (§9). It is in a kernel file, so that correction waits too.

## 2. What C1.4's "RCU" means here (D1)

**D1: kernel RCU, in its simplest flavour: read-side sections are regions with interrupts masked (the "RCU-sched" shape).** A writer that unlinks an object waits for a grace period before freeing it; a reader inside an IF-off region may use what it found until the region ends.

Why the kernel, and not a user-space RCU library:
- DDR-1181 §2 puts P5 on **P1** (kernel threads), not on P2/P3 (futex and `psync.h`), which a user-space RCU would need.
- The kernel has a consumer today (§5). A user-space library would have none in the tree.
- A user program that wants RCU can build it on `psync.h` and futex. That is a library choice, not missing OS functionality. It is **not** planned here; if it is wanted later, it is its own record.

Rejected alternatives:
- **Make the tick walk lockless without RCU.** That is exactly the use-after-free DDR-1096 described: the reaper could free the node the walk stands on.
- **Sleepable or counter-based read sides (SRCU or epoch counters).** They cost two atomic operations per read section and let readers block. The first consumer runs in an ISR with IF already clear, so it needs neither.
- **Grace periods driven by an IPI round** (reusing `tlb_shoot`'s ack machinery, `kernel/mm/tlb.c:95`). That is faster, but it adds a vector and couples writers to the IPI path. The reaper is not latency-bound, so ticks suffice (D2). It stays an option if a latency-bound writer appears.

## 3. The grace-period scheme (D2)

- **Quiescent state:** a CPU that **takes an interrupt** has IF set at that instant, so it is outside every IF-off read section. Each CPU already counts its timer ticks (`percpu.ticks`, `percpu.h:25`, incremented at the top of `sched_tick`, `sched.c:2183`). Context switches and idle `hlt` are covered too: an idle CPU in `hlt` with IF set keeps taking ticks.
- **The increment becomes a locked RMW** (`__atomic_fetch_add(&pc->ticks, 1, __ATOMIC_SEQ_CST)`, a `lock xadd` on x86). Today it is a plain `++`.
  - **Why it must be locked:** x86 TSO lets a later load pass an earlier store. Then the ISR's own read section could load a ring pointer before the counter store becomes visible: a writer could see the counter move, free the node, and the reader would then use it. A locked instruction orders the later loads after the store.
  - The cost is one locked add per tick per CPU, in place of today's `g_sched_lock` acquire and release on the same path.
- **`synchronize_rcu()`** (writer side):
  1. a full fence (`__atomic_thread_fence(__ATOMIC_SEQ_CST)`), so the unlink is visible to every CPU before the snapshot;
  2. snapshot `ticks` of every CPU that is ticking (`shoot_ready` set, `percpu.h:68`), skipping the calling CPU, which is not in a read section by rule 2 below;
  3. wait until every snapshotted counter has changed, yielding between polls.
- **Why this is enough:** suppose CPU X's counter changed after the snapshot. Then an interrupt was taken on X after the snapshot. Any read section X was in when the grace period began had ended by that instant. Any read section that started later loads ring pointers after the locked increment, so after the unlink was visible, and cannot reach the unlinked node.
- **Rules (each enforced by an assert in the code PR):**
  1. A read section must not block, sleep or re-enable interrupts.
  2. `synchronize_rcu()` must be called with IF set, outside every read section, and **holding no spinlock**. Otherwise a reader that takes `g_sched_lock` inside its section (§5's act step) could deadlock against a writer that holds it while waiting.
  3. A CPU that is not ticking (not yet started, so `shoot_ready` is 0) runs no readers and is not waited on. Nothing in the tree clears `shoot_ready` once set, so a CPU that stops ticking after start is the stall case below, not this one.
- **Stall:** a CPU that keeps IF clear for a long time (a wedged CPU, an `[apfreeze]`) stops the grace period.
  - The writer then waits, so memory waiting to be freed grows. **It never frees early.** A leak is the safe failure, a use-after-free is not.
  - After `RCU_STALL_TICKS` (proposed 1,000 ticks of the writer's own CPU) it prints `[rcu] stall cpu=<n> waited=<ticks>` once per grace period and keeps waiting.
  - The stall line goes into the gate's FORBIDDEN list (§7).

## 4. The API (D3), `kernel/proc/rcu.h`

| call | meaning |
|---|---|
| `rcu_read_lock()` / `rcu_read_unlock()` | `local_irq_save` / `local_irq_restore`, plus a per-CPU nesting count used only by the asserts |
| `rcu_dereference(p)` | `__atomic_load_n(&p, __ATOMIC_ACQUIRE)` (a plain load plus a compiler barrier on x86) |
| `rcu_assign_pointer(p, v)` | `__atomic_store_n(&p, v, __ATOMIC_RELEASE)`, for every store that publishes a node to lockless readers |
| `synchronize_rcu()` | §3. Blocking; kernel threads only |
| `rcu_defer_free(struct tcb *)` | appends to one deferred list under a leaf lock; the reaper drains it (D5) |

No `call_rcu` with arbitrary callbacks: the only deferred action needed is "free this tcb". A general callback list waits until a second consumer needs one.

## 5. The first consumer: the timer-tick timed-wake sweep (D4)

**Today** (`sched.c:2192-2308`): under `g_sched_lock`, walk the ring. For each thread with `state == THREAD_BLOCKED && block_deadline != 0 && g_ticks >= block_deadline` (at most 32 per tick), set `wake_timed_out = 1` and `block_deadline = 0`. Then drop the lock and `sched_unblock` each one.

**Designed:**
1. Under `rcu_read_lock()`, walk the ring with `rcu_dereference` on each `->next`. For each node, read only `state` and `block_deadline`. A node that is BLOCKED and expired is a candidate, together with the deadline value it was seen with.
2. If there are none, which is the common tick, the walk takes no lock of any kind. **No `g_sched_lock` on any tick** (§5.2 point 6).
3. Act on each candidate **as the walk reaches it**, under that thread's own wait lock, not under `g_sched_lock` (§5.2).
   - A candidate whose lock is busy is skipped and the walk goes on.
   - **No per-sweep cap** (corrected twice after CodeRabbit on #114). Every expired candidate the walk reaches is acted on in that sweep.
   - Today's 32-entry cap exists only because the sweep collects candidates into a stack array (`_wake[32]`, `sched.c:2190`) and acts afterwards. Acting during the walk removes the array, and so the reason for a cap.
   - The walk's own work stays O(ring length), the same order as today's walk: it visits every node anyway, and each act is one trylock, four loads and at most one `sched_unblock`. That count excludes the runqueue-lock wait inside each `sched_unblock`, which has no bound (§5.2), so the ISR's total time has no bound either. Today's sweep has the same exposure.
   - **Why both earlier drafts starved waiters:**
     - The first collected 32 candidates before acting. Candidates with busy locks could fill the list every tick, so a later waiter whose lock was free was never tried.
     - The second capped successful timeouts at 32 per sweep. Every sweep starts at the same ring position, so 32 early waiters that time out and block again before each sweep could take the cap every tick, and a later waiter was skipped forever.

Why the behaviour is unchanged:
- The write half (step 3) re-checks the same predicate under the lock the waiter and its normal wakers already use, and also checks that it is still **the same wait** (§5.2). A candidate that stopped qualifying, or that has moved on to a new wait, fails the re-check.
- A thread whose deadline passes just after the scan is not seen until the next sweep. That is today's granularity: the sweep already runs once per tick. It is woken by that sweep only if its own wait lock is free then, and that sweep can itself be late (runqueue waits, overrun). §5.2 gives the full lateness, which is not bounded; the designed sweep has no per-sweep cap.
- A candidate tcb cannot be freed between steps 1 and 3: the whole tick runs with IF clear, so it is one read section, and every tcb free waits for a grace period (D5).

What the walk needs from the writers:
- **Insertion** (`sched.c:1270-1271`) publishes with `rcu_assign_pointer(current_thread->next, t)` after `t->next`, `t->state` and `t->block_deadline` are initialised. Today these are plain stores, which the compiler may reorder inside the locked region.
- **Unlink** keeps `t->next` intact, as `sched_ring_unlink` already does. A reader standing on `t` follows it back into the ring.
- **No walk bound in the product** (corrected after CodeRabbit on #114). An earlier draft stopped the walk after 4,096 nodes. But the tree has no system-wide thread limit, only `THREAD_MAX_PER_MM` (64 per process, `sys_thread.c:23`), so a ring longer than that is legal. A walk that stopped early would omit threads, and their timeouts would never fire.
  - The walk runs once round the whole ring. A consistent ring is finite, and an unlinked node's `->next` still leads back into it (above).
- **The walk is anchored on `idle0`, not on `current_thread`** (corrected after CodeRabbit on #114). It starts at `idle0->next` and stops when it reaches `idle0` again.
  - `idle0` is the static BSP idle thread, which the tree already names "the BSP idle + ring anchor" (`sched.c:1050`, `g_idle[0]`). Idle threads never exit, and the reaper takes only zombies, so `idle0` is never unlinked.
  - `current_thread` is not safe as an anchor: it can be a zombie that the reaper has already unlinked (§5.3).
  - The `RCU_TORTURE` build records the longest walk seen (`ring_len_max`, printed by arm RF) as a diagnostic. It never stops the walk.

### 5.1 Concurrent access to the predicate fields (added after CodeRabbit on #114)

**Today's sweep already reads these fields concurrently with their writers (by reading, at `733ac91`).** `g_sched_lock` does not exclude them:

| field | written by | under |
|---|---|---|
| `block_deadline` | `sched_block_timeout` sets it, and clears it after `schedule()` returns (`sched.c:2555`, `:2567`) | the caller's wait lock `lk`, **not** `g_sched_lock` |
| `state` | `sched_block_timeout` (`:2556`) and `sched_block_on` (`:2530`) set `THREAD_BLOCKED` | `lk` |
| `state` | `sched_block` (`:2514`) sets `THREAD_BLOCKED` | no lock (local IRQ masking) |
| `state` | `sched_unblock`: BLOCKED to READY | a CAS, no lock; its comment already accepts that the locked walk "observes READY this pass or the next tick (benign)" |
| `state` | `schedule_locked` (`sched.c:1720`; stores at `:1762-1852`) | the runqueue lock and local IRQ masking |
| `block_deadline` | the sweep, to 0 (`:2250`) | `g_sched_lock` |

So the race CodeRabbit named is not introduced by RCU. RCU does not remove it either. What the code PR adds is a stated discipline that makes these accesses well defined and bounds a miss:

1. **Every access to `state`, `block_deadline` and `wake_timed_out` is an `__atomic` operation.** That covers loads and stores on every path in the table, plus the sweep. On x86-64 these are aligned 32- and 64-bit plain moves, so the generated code barely changes. The C-level data race, which is undefined behaviour today, becomes defined.
2. **Order inside `sched_block_timeout`.**
   - The blocker stores `wake_timed_out = 0` and `block_deadline` (relaxed) first, then `state = THREAD_BLOCKED` with **release**.
   - The scan loads `state` with **acquire**, and reads `block_deadline` only after seeing `THREAD_BLOCKED`.
   - So a scan that sees BLOCKED also sees that block's deadline, never a stale or zero one from before.
3. **A scan can miss an expired thread only while that thread's own writes are in flight.**
   - That is, it is mid-way through blocking or being woken at that instant.
   - Every CPU runs the sweep on every tick, so the next tick on any CPU sees the settled values.
   - **This race adds at most one tick.** That is not the total wake-up delay: busy wait locks, runqueue waits and sweep overruns add to it, and §5.2 states the total as not bounded. Today's locked walk has the same exposure, since the lock does not exclude the writers. Today's sweep is also capped at 32 wakes per tick; the designed sweep has no cap (§5 step 3).
   - **Progress assumption, stated rather than proved:** a store becomes visible to every CPU well within one tick. Release/acquire orders what a scan sees once it sees the release; it says nothing about **when**.
     - On x86 a store leaves the store buffer in a bounded but unspecified time. The SDM gives no number.
     - Every spinlock release in this kernel already relies on the same assumption.
     - Under TCG, memory is shared by the host threads that run the vCPUs, so this is about host visibility, not modelled hardware.
     - Arm RE measures the bound, and nothing here proves it.
   - A thread that is BLOCKED with an expired deadline and no writer in flight is seen by every scan. It cannot be omitted indefinitely.
4. **The act step** is §5.2. An earlier draft of this record re-checked under `g_sched_lock`, which does not exclude the waiter or its wakers. CodeRabbit showed on #114 that a stale act could then hit a **new** wait (below).

**Found on the way (by reading, not verified):** two races in **today's** sweep. §5.2 closes both; neither is claimed as an observed defect.
- **Spurious timeout.** The sweep can set `wake_timed_out = 1` on a thread that a waker is waking normally at the same moment. That thread then returns `-ETIMEDOUT` although `*done` is set.
- **A stale timeout hits a new wait** (CodeRabbit on #114).
  1. The sweep reads a thread as BLOCKED and expired.
  2. A normal waker wakes it, and it runs and starts a new `sched_block_timeout`.
  3. The sweep, still on its first reading, writes `block_deadline = 0` over the new wait's deadline. Its later `sched_unblock` can then wake the new wait spuriously, or the new wait is left blocked with no timeout.
  - Today the window runs from the sweep's read to the `sched_unblock` loop after the walk. It is short but real on SMP, because nothing on the waiter's side takes `g_sched_lock`.

### 5.2 The act step: under the waiter's own lock, bound to the same wait (added after CodeRabbit on #114)

The tree already has this pattern for futex sleepers. A sleeper publishes the lock it sleeps under in `t->fx_lock` (`futex.c:139`, cleared at `:155`). `sig_raise` wakes it by taking that lock and re-checking `t->fx_lock == lk` (`futex.c:161-165`). The design generalises it to every timed wait:

1. **Publish the wait.** `sched_block_timeout` stores `t->block_lk = lk` and the deadline, then `state = THREAD_BLOCKED` with release, all while holding `lk`. After `schedule()` returns and it re-takes `lk`, it clears `block_lk` and `block_deadline`.
   - `block_lk` is a new tcb field, initialised in `sched_create_state` (CLAUDE.md non-negotiable 11).
2. **Act under `lk`.** For each candidate:
   - The sweep loads `lk = __atomic_load_n(&t->block_lk, __ATOMIC_ACQUIRE)`. **If it is NULL, the candidate is skipped**: the waiter has already woken and cleared it. This is the same load-then-NULL-check `futex_kick` does (`futex.c:160-163`).
   - Then it calls `spin_trylock(lk)`. It never spins, because the timer ISR may have interrupted a holder of `lk` on this CPU. If the trylock fails, the candidate is left for the next tick.
   - **The lock must stay valid from that load to the unlock (corrected after CodeRabbit on #114).** **The rule: a lock passed to `sched_block_timeout` has static storage.** A static lock is never freed, so it is valid wherever the sweep loads it.
     - An earlier draft also allowed a heap object freed after a grace period. That is **not** enough on its own: a sweep could load `block_lk` after the grace period has started and then use it after the free. The owner would first have to clear every published `block_lk` that names the object and block new ones.
     - This record does not design that. A caller that needs a heap-allocated wait lock needs its own record.
   - **Every caller already meets the static rule** (by reading, at `733ac91`):

     | waiter | its lock lives in |
     |---|---|
     | futex | `g_fx` buckets, `futex.c:46` |
     | virtio-blk | `g_inst`, `virtio_blk.c:132`, and the static instance at `:768` |
     | bcast | the subscribers `sub_a` and `sub_b`, `main.c:295` |
     | IPC | `g_agent_ep`, `sys_aether.c:435`, and `demo_ep`, `main.c:166` |

     The code PR asserts it in `sched_block_timeout`: `lk` must lie inside the kernel image's `.data`/`.bss`. A future caller with a heap-allocated lock then fails loudly, and needs its own record, as above.
3. **Re-check under `lk`:** `t->block_lk == lk`, `state == THREAD_BLOCKED`, `block_deadline == d_seen` (the exact value the scan saw), and `g_ticks >= d_seen`. Only then set `wake_timed_out = 1` and `block_deadline = 0`, call `sched_unblock(t)`, and release `lk`.
4. **Why this binds the timeout to the same wait.** While the sweep holds `lk`:
   - A normal waker of this wait cannot run. The design requires every waker of a timed wait to set `*done` and call `sched_unblock` under that wait's `lk`; the code PR checks each of the five callers (`futex.c:148`, `virtio_blk.c:358` and `:429`, `bcast.c:78`, `ipc.c:65`).
   - The thread cannot return from this wait either: it re-takes `lk` first. If it was already woken, it is not `BLOCKED`, so the re-check fails.
   - If it has already started a new wait, `block_lk` or `block_deadline` differs from what the scan saw, so the re-check fails. Deadlines of one thread strictly increase: a new wait's deadline is `g_ticks + timeout` with `timeout >= 1`, taken after the old one expired. `sched_block_timeout` is required to treat `timeout_ticks == 0` as an immediate `-ETIMEDOUT` without blocking.
   - So a stale act changes nothing. Both races above are closed.
5. **`*done` wins.** `sched_block_timeout` returns 0 when `*done` is set under `lk`, even if `wake_timed_out` is also set. A wake that raced the timeout is not reported as a timeout.
6. **No `g_sched_lock` in the act step at all.** Arm RD (`tick_lock=0`) then holds on every tick, not only on ticks with nothing due.

**Lateness bound, stated honestly.**
- Every ticking CPU sweeps every tick, and a sweep acts on **every** expired candidate it reaches, with no cap. So an expired waiter is woken by the first sweep that sees it, unless its own `lk` is busy at that moment.
- A candidate whose `lk` is busy is skipped and the walk goes on, so no **other waiter's wait lock** can delay it. It is retried on the next tick, on every CPU.
- **Runqueue contention does delay later candidates within a sweep** (corrected after CodeRabbit on #114).
  - Each act calls `sched_unblock`, whose `rq_push` takes the target CPU's runqueue lock and spins for it. Later candidates in the same sweep wait for all the earlier acts.
  - **That wait has no bound** (corrected again after CodeRabbit on #114). The runqueue lock is a plain test-and-set spinlock (`spinlock.h:23-26`), which is not fair: a waiter can lose every race to other CPUs for as long as they keep taking the lock. And the section it waits on is not constant: `rq_pop` runs `fair_candidate`, which scans the queue under the lock (`sched.c:621-636`, `:342`). So one act can wait for many sections, each as long as its queue.
  - A sweep that overruns a tick makes the next tick on that CPU late by the overrun.
  - Today's sweep has the same exposure: it also calls `sched_unblock` from the timer ISR for each wake (`sched.c:2309`).
- **The bound, therefore:** an expired waiter is woken within the first sweep that reaches it while its own `lk` is free. That sweep can finish late by the runqueue waits of the acts before it, and those waits are unbounded. Neither delay is bounded here; both are measured (RE (f)).
- This rests on the progress assumption of §5.1 point 3: a store becomes visible to every CPU well within a tick.

**Gate:** arm RE (timed wakes on time, `late_max<=1`) is the bound of point 3. A mutant **U7** makes the scan read `block_deadline` before checking `state`, with no acquire. That is the ordering point 2 forbids. Like the locked increment (§8), it is unlikely to show under TCG, so it is stated not gated up front.

### 5.3 Found on the way: today's tick walk can loop forever on an unlinked zombie (by reading, not verified)

Found by CodeRabbit on #114 and traced in the tree at `733ac91`. **It is a defect in today's kernel, not in this design.**
1. `sched_exit_common` marks the exiting thread `THREAD_ZOMBIE` under `g_sched_lock`, releases the lock, and calls `schedule()` (`sched.c:2786-2797`). The thread is still `current_thread` on its CPU.
2. While it switches away, `schedule_locked` calls `switch_wait_offcpu_sched(next, cpu)` (`sched.c:1822`). That spins on `sti; pause; cli` (`sched.c:721` onward) while `next` is still on another CPU, so **a timer tick can land with `current_thread` == the zombie**. DDR-887's comment at `sched.c:2322` already notes this tick.
3. On another CPU, the zombie can be unlinked while it is still on-CPU. There are two paths, and both wait for the zombie to be off-CPU only **after** the unlink, before freeing it:
   - `reaper_thread` takes a zombie whose parent is gone and unlinks it under `g_sched_lock` (`sched.c:2975` onward);
   - a parent's `sys_wait4` calls `sched_destroy(child)` (`sys_wait.c:66`), which unlinks under `g_sched_lock` and only then calls `sched_free_tcb` (`sched.c:1685-1692`). `sched_destroy` has other callers too (`sched.c:1391`, `:1436`, `:1453`, `:3039`).
4. The tick's walk then starts at the unlinked zombie and follows its `->next` into the live ring, which no longer contains the zombie. `while (_t != current_thread)` never ends.
   - It loops with IF clear **and holding `g_sched_lock`**, so every other CPU that needs the lock stops too. That is a whole-system stall.

**Status:**
- Not reproduced.
- Not attributed to OPEN-2 or to any recorded stall: a matching shape is not an attribution (DDR-1056).
- Whether the window is wide enough to hit in practice is not measured.
- Fixing it is a kernel change. It is tracked as issue #115 so the operator can decide it apart from RCU. Either of these closes it:
  - `idle0` anchoring (above), which does not depend on who unlinks;
  - keeping an on-CPU zombie linked until it is off-CPU, on **every** unlink path: `reaper_thread` and `sched_destroy` (so `sys_wait4` and its other callers). A check in the reaper alone does not close it.

Not changed: `sched_snapshot`, `sched_find_child` and the `*_pid_*` helpers stay under `g_sched_lock`. They act on what they find, and DDR-1157 and DDR-1159 already fixed their lifetime that way. Moving them to RCU is a later record, if a measurement asks for it.

## 6. Freeing a tcb (D5)

- Every free of a tcb that was **ever linked into the ring** goes through `rcu_defer_free(t)` after the unlink: the reaper path through `sched_free_tcb`, and `sched_destroy` for threads that were linked.
- A tcb freed before its ring insert (the early failure paths in `sched_create_state`) is freed at once, as today.
- **The reaper drains the list:** take the whole list, `synchronize_rcu()`, then `sched_free_tcb` each entry (kernel stack, then tcb).
  - An entry pushed after the take waits for the next round. It was unlinked before it was pushed, so its grace period is covered.
- **Memory bound:** at most one grace period's worth of zombies waits, plus the stall case of §3. The code PR prints the high-water mark (`rcu_deferred_max`) in the gate.

## 7. Gate: `smoke-rcu` (new), and its mutants

A kernel self-test built with `-DRCU_TORTURE=1`. The flag is off by default and is set only for this gate, as `OPEN2_HUNT` is (`Makefile:294`), so the default kernel build does not contain it. Each arm is checked for vacuity before it is written (CLAUDE.md non-negotiable 3).

| arm | what it does | sentinel (proposed) |
|---|---|---|
| **RA** use-after-free | One reader kernel thread pinned to each ticking CPU (`pin_cpu`, `sched.h:278`). Each one loops: `rcu_read_lock`; `p = rcu_dereference(g_slot)`; check `p->magic`; pause about 2 ticks; check again; unlock. A writer swaps `g_slot` to a fresh node, calls `synchronize_rcu`, fills the old node with `0xDD` and frees it. | `PRADYOS_RCU_A swaps>=2000 poison_seen=0 cpus=4` |
| **RB** read side masks IF | The same readers, with sections longer than one tick. | covered by RA's `poison_seen=0` |
| **RC** every CPU is waited on | RA's readers on every CPU, counted per CPU. | `PRADYOS_RCU_C seen_cpus=4` (each CPU's reader completed at least 100 sections) |
| **RD** the tick takes no `g_sched_lock` when no wake is due | Count `g_sched_lock` acquisitions made from `sched_tick` over 1,000 ticks with no timed waiter. | `PRADYOS_RCU_D tick_lock=0 ticks>=1000` |
| **RE** timed wakes still happen, on time | (a) A kernel thread blocks with a 50-tick timeout, 20 times. (b) Saturation: 100 kernel threads block with the same deadline. (e) Re-block churn: 64 threads loop on 1-tick timed waits for 500 ticks while one more thread waits 50 ticks; that thread must time out on time. (c) Re-wait: a thread is woken normally just before its deadline, at once starts a new 50-tick wait, and must time out on that new deadline, not earlier and not never. (d) Head-of-line: 32 expired waiters whose wait locks are held by the test for 20 ticks, with one more expired waiter behind them on a free lock. That waiter must still time out on time. (f) Runqueue contention: 100 waiters, all pinned to one target CPU, expire together while threads pinned to that same CPU yield in a loop, so every act's `rq_push` contends on the runqueue lock the yielders keep taking. The arm must show that the **sweep** met the contention. `lock_stat` cannot show that, because it counts contended acquisitions of a lock from every caller. So, under `-DRCU_TORTURE` only, the sweep's act path counts its own contended pushes: it tries `spin_trylock` on the target runqueue lock first, and counts a miss in `rq_contended` before it takes the lock as usual. `rq_contended` must be above 0, or the arm fails as vacuous. It is above 0, not equal to the act count: pinning makes contention likely, not certain for every act (CodeRabbit on #114). The test reports the longest sweep in TSC cycles (`sweep_max`, data, not asserted) and the worst lateness. **Every RE lateness is measured twice:** in ticks, and in TSC cycles from the deadline to the waiter running again (`late_tsc_max`). The BSP's own ISR advances `g_ticks`, so time spent inside a long sweep on the BSP does not show up in tick counts (CodeRabbit on #114). | `PRADYOS_RCU_E woke=20 late_max<=1 sat_woke=100 sat_late_max<=1 rewait_ok=20 hol_late<=1 churn_late<=1 rq_late<=2 rq_contended>0 sweep_max=<n> late_tsc_max=<n>` |
| **RF** tcb frees wait | Thread churn (create, exit, reap; 500 rounds), while the tick scan checks each node it stands on for `0xDD` and for a changed `tid` (DDR-1097's identity test). | `PRADYOS_RCU_F rounds=500 bad_node=0 deferred_max=<n> ring_len_max=<n>` |

FORBIDDEN: `[rcu] stall`, `RCU FAIL`.

**Mutants, each a one-change commit, each expected red on its own arm:**

| mutant | change | red on |
|---|---|---|
| U1 | `synchronize_rcu` returns at once | RA (`poison_seen>0`) |
| U2 | `rcu_read_lock` does not mask IF | RB/RA (a tick lands inside a section, the counter moves, the writer frees under a reader) |
| U3 | `synchronize_rcu` skips the highest ticking CPU | RC/RA (the reader on that CPU sees poison) |
| U4 | the tick sweep keeps `g_sched_lock` around the scan (today's code) | RD (`tick_lock>0`) |
| U5 | the act step drops the `block_deadline == d_seen` and `block_lk == lk` checks (§5.2 point 3) | RE (c) (`rewait_ok<20`). Not certain to be red; see §8 |
| U6 | the reaper frees with `kfree` instead of `rcu_defer_free` | RF (`bad_node>0`) |
| U8 | a per-sweep cap of 32 timeouts is restored | RE (b) (`sat_late_max>1`) and RE (e) (`churn_late>1`) |
| U9 | the walk is anchored on `current_thread` again, not `idle0` | RF with exit churn: the gate hangs or the walk never returns. Expected to survive unless the zombie window is hit; RF widens it with an `RCU_TORTURE` pause in `switch_wait_offcpu_sched`. Stated **possibly not gated** (§8) |
| U7 | the scan reads `block_deadline` before `state`, with no acquire (§5.1) | RE, but expected to survive under TCG: stated not gated (§8) |

## 8. What is not gated, stated before any code

- **The locked increment (§3).** A mutant that makes it a plain `++` is the store-to-load reordering case. TCG's memory model is unlikely to show it, so it would probably survive. It is stated **not gated**, and the argument for it is §3's.
- **U7** (§5.1, scan ordering): same reason as the locked increment; stated not gated.
- **U9** (§5.3, the walk's anchor): it needs the exit/reap window of §5.3 to land under a tick. RF widens that window but cannot force it, so U9 may survive. If it does, it is recorded as not gated.
- **U5** may survive: the race it opens needs a normal wake and a new wait to land exactly between the scan and the act. Arm RE (c) widens that window but cannot force it. If it survives, it is recorded as not gated before merge, as 6655226539 requires.
- **Real hardware:** none of this is verified there. Grace-period latency under TCG says nothing about real-hardware latency (CLAUDE.md non-negotiable 15).
- **Contention:** RD counts acquisitions; it is not a speedup claim. Any performance claim needs the re-measurement of §1 with a denominator.

## 9. Order of work, and who decides

1. **This record**, docs only. It merges under the standing docs-only rule (6647074754 item 2).
2. **The code PR** (`kernel/proc/rcu.[ch]`, `sched.c`, the reaper, `kernel/main.c`'s torture test, `Makefile` and the CI workflow for `smoke-rcu`). **It touches the kernel, so it waits for the operator** (6655226539; and the operator's instruction for this item). It also corrects the stale comment of §1.1.
3. If the operator approves it, it follows the five conditions of 6594914166 (6647074754 item 4): mutants U1–U6 run in CI and shown red on the final head, or recorded as not gated.

## 10. Amendments to other records (this PR)

- **DDR-1181 §2, row P5:** "the scheme is chosen in DDR-1200 (kernel, IF-off read sides, tick-counter grace periods); its code waits for the operator".
- **`docs/V1_MASTER_CHECKLIST.md`, C1.4 RCU row:** designed in DDR-1200; not ticked until the code merges.

---

## Amendment 1 (2026-11-12): two code parts, P5a and P5b

**Authority.** #45 6761846242 item 3: "The RCU code (DDR-1200), only after #115 has merged. Gate smoke-rcu, mutants U1–U9; record any survivor as not gated." #115 merged as #118 `baac4dd`. §9's "waits for the operator" is now answered by that item.

**Why two parts.** §5's consumer changes the timed-wait protocol on every timed waiter, not only the sweep. That means a new `block_lk`, atomic accesses to `state` and `block_deadline`, and the waker discipline on the five callers of §5.2 point 4. A red run on one PR holding all of it would not say which half broke. Each part below has its own arms and mutants, and each arm is checked for vacuity on its own part.

**A split that does not work.** Putting the deferred tcb free (D5) into the first part, with arm RF and mutant U6, makes U6 vacuous. Every ring reader stays under `g_sched_lock` until the sweep goes lockless, so a plain `kfree` after the unlink is still safe and RF cannot turn red. D5 therefore goes with its first lockless reader.

| part | code | arms | mutants |
|---|---|---|---|
| **P5a**, the grace-period core | `kernel/proc/rcu.[ch]`: `rcu_read_lock`/`unlock` with the nesting count and the §3 rule asserts, `rcu_dereference`, `rcu_assign_pointer`, `synchronize_rcu` with the stall line. The locked tick increment (§3). The `RCU_TORTURE` flag, the `smoke-rcu` target and its CI job. | RA, RB, RC, on a test-only `g_slot`. They do not touch the ring. | U1, U2, U3. The locked increment stays stated not gated (§8). |
| **P5b**, the first consumer | §5 and §5.1–§5.2: the lockless `idle0`-anchored sweep, `block_lk`, the atomics, the act step and the waker discipline. D5: `rcu_defer_free` and the reaper drain. | RD, RE, RF | U4, U5, U6, U7, U8, U9 |

**P5a has no product consumer.** Until P5b, `rcu.[ch]` is used only by the torture test. The locked increment is the only default-build change in P5a. It changes the instruction, not the behaviour: one `lock xadd` per tick per CPU in place of a plain add.

**Which CPUs `synchronize_rcu` waits on (§3 rule 3, checked against the code at `baac4dd`).**
- An AP sets `shoot_ready` in `smp.c:272`, one call before it arms its own timer (`smp.c:273`), with IF still clear until `sched_ap_enter`. A grace period that snapshots such a CPU waits for its first tick. That is a short wait during AP start, never an early free.
- `ticks != 0` would be the wrong test. An AP can begin a read section after `sched_ap_enter` and before its first tick. A writer that skipped it then could free under it. So the snapshot uses `shoot_ready`, as §3 says.

**Order:** P5a first, then P5b. The checklist RCU item is ticked only when P5b merges.

**P5a as built (changes from §7, stated here so the gate is read correctly).**
- **Swap count.** RA runs `RCU_T_SWAPS` = 400 swaps, not the ≥ 2,000 §7 proposed. Each swap is one grace period, and a reader holds IF clear for 1.5 ticks of TSC time per section (RB), so 2,000 swaps would take roughly 2,000 × 2.5 ticks under TCG, about 50 s before TCG's slowdown. 400 keeps the gate inside its 240 s window. RC still needs at least 100 sections per CPU, and each reader completes several hundred in 400 swaps.
- **FORBIDDEN lines.** §7's FORBIDDEN list (`[rcu] stall`, `RCU FAIL`) is enforced through arm A's line, not a `FORBIDDEN_SENTINEL`: the line carries `stalls=<n> fails=<n> aborted=<n>`, and the gate requires `stalls=0 fails=0 aborted=0` exactly. Either line during the run therefore fails the gate, and the runner's early exit (DDR-785) stays on. A `FORBIDDEN_SENTINEL` would turn it off and make every run wait out its full window.
- **Detecting a free under a reader.** A freed node is filled with 0xDD and then `kfree`d, and the next swap's `kmalloc` of the same size class usually gets the same slot back. So the reader checks both the magic word and the node's generation before and after its hold. A poisoned node or a reused one counts in `poison_seen`.
- **Rule checks.** Rule 1 is checked in `schedule()` (a switch with this CPU's nesting count above 0). Rule 2 is checked in `synchronize_rcu()` for IF and nesting only: the tree has no per-CPU count of held spinlocks to check the spinlock part against. Each prints one `RCU FAIL: …` line per kind.

**P5a evidence (#123, merged as `3f9be84`).** QEMU (TCG, 4 vCPU) only; not verified on real hardware.
- **Head `57eb09f`**, run [43395743995](https://github.com/prady4the4bady/Prady4OS/actions/runs/43395743995): `[rcu] cpus=4 swaps=400 poison_seen=0 sections_min=402 stalls=0 fails=0 aborted=0`, `PRADYOS_RCU_C seen_cpus=4`.
- **Mutants on that head.** Each failed in the gate itself, not in the build.

| mutant | change | result | run |
|---|---|---|---|
| U1 | `synchronize_rcu` returns at once | `poison_seen=3`, `seen_cpus=0` (the swaps finished before the readers ran) | [43395914395](https://github.com/prady4the4bady/Prady4OS/actions/runs/43395914395) |
| U2 | `rcu_read_lock` does not mask IF | `poison_seen=331 fails=6`, `RCU FAIL: switch inside a read section` | [43395917959](https://github.com/prady4the4bady/Prady4OS/actions/runs/43395917959) |
| U3 | the highest ticking CPU is not waited on | `poison_seen=126`, `seen_cpus=4` | [43400833607](https://github.com/prady4the4bady/Prady4OS/actions/runs/43400833607) |

- **Not gated, as §8 stated before the code:** the locked tick increment.
- **Changed after review (CodeRabbit on #123, in `57eb09f`):**
  - the stall clock is the furthest any ticking CPU has advanced since the snapshot, so a writer that moves between CPUs still reaches the stall line;
  - the torture hold loop's `rdtsc` and `pause` have `"memory"` clobbers, so the after-hold loads stay after the hold.
---

## Amendment 2 (2026-11-13): P5b decisions taken before its code

Read against the tree at `3f9be84`. Each point changes or makes precise a line of §5, §5.2 or D5. They are stated before any P5b code is written (CLAUDE.md non-negotiable 2).

1. **Only the memory is deferred, not the resource release (changes D5).**
   - `sched_free_tcb` does more than free memory. It waits for the thread to be off-CPU, removes it from any runqueue, closes every fd (`fd_free`), frees the rollback log and drops the cap-table reference. Only then does it free the kernel stack and the tcb.
   - If the whole call waited for a grace period, a reaped child's fds would stay open until the reaper's next drain. A parent that `wait4`s and then reads a pipe the child held could see end-of-file late. That is a behaviour change no consumer asked for.
   - The lockless sweep reads only fields inside the tcb: `next`, `state`, `block_deadline` and `block_lk`. It never touches the stack, the fds or the caps.
   - **So `sched_free_tcb` is split.** The release part runs at once, as today. The tcb, plus its kernel stack to keep the free path simple, goes to `rcu_defer_free` and is freed only after a grace period.
2. **Who drains.** `rcu_defer_free` pushes onto one list under a leaf lock and never waits. Both unlink paths push to it, `reaper_thread` and `sched_destroy` (and so `sys_wait4`), so `wait4` never blocks on a grace period. On every pass, `reaper_thread` takes the whole list. If it is non-empty, it calls `synchronize_rcu()` once and then frees each entry. An entry pushed after the take waits for the next pass. It was unlinked before it was pushed, so its grace period is covered.
3. **`timeout_ticks == 0`** returns `-ETIMEDOUT` at once, with `lk` still held and without blocking. Today it blocks until the next tick. §5.2 point 4's argument that deadlines strictly increase needs this.
4. **The static-lock assert (§5.2 point 2)** checks that `lk` lies in `[__rodata_end, __bss_end)`. That range is the kernel image's `.data` and `.bss` (`kernel/kernel.ld`: `.data` starts at `__rodata_end`, `.bss` ends at `__bss_end`). A failure prints `RCU FAIL: timed wait on a non-static lock` once, and the wait goes ahead. That wait is **not** safe: a sweep can load `lk` from `block_lk`, take it after the waiter has returned, and so use it after its owner frees it (§5.2 point 2). The check is a report, not a refusal, because refusing would change the waiter's behaviour; a caller with a non-static lock needs its own lifetime protocol and its own record (CodeRabbit on #125).
5. **#118's membership pass goes.** DDR-1201 made the sweep find `current_thread` on the ring from `idle0` before starting there. The designed sweep (§5) starts at `idle0->next` and stops at `idle0`, so the membership pass has nothing left to do. The `ZANCHOR_TEST` probe stays: it still forces the window, and arm B then measures the new walk. If it passes, it shows the `idle0` anchor holds without the membership pass.
6. **Gate order.** `smoke-rcu` keeps arms RA, RB and RC and gains RD, RE and RF in the same `RCU_TORTURE` build. Mutants U4–U9 are one-change commits on the final head, as before. U5, U7 and U9 are stated **possibly not gated** before they run (§8); any of them that survives is recorded as not gated.

---

## Amendment 3 (2026-11-13): P5b as built

Written with the P5b code, before its mutants run. Each point changes or makes precise a line of §5–§7 or Amendment 2. QEMU (TCG, 4 vCPU) only; not verified on real hardware.

1. **The sweep** (`sched_timed_sweep`, `sched_timed_act` in `sched.c`) is as §5 and §5.2 describe: `rcu_read_lock`, walk from `idle0->next` to `idle0` with `rcu_dereference`, `state` loaded with acquire before `block_deadline`, the act under `spin_trylock(block_lk)` with the four re-checks, no cap. DDR-1201's membership pass is gone. The `ZANCHOR_TEST` and `OPEN2_HUNT` instruments stay inside the new walk.
2. **Writers.** The ring insert publishes with `rcu_assign_pointer`; the unlink stores `p->next` with `rcu_assign_pointer` and leaves `t->next` alone. Every store of `state` in the table of §5.1 is an `__atomic` release store (`sched_block`, `sched_block_on`, `sched_block_timeout`, `schedule_locked`, `sched_exit`, the `THREAD_DONE` path). `sched_unblock` was already a CAS.
3. **The waker discipline (§5.2 point 4), checked by reading at `fc55471`.** Every waker of a timed wait sets `*done` and calls `sched_unblock` under that wait's lock: `futex.c` (wake, and `futex_kick` re-checks `fx_lock` under it), `virtio_blk.c` (completion and slot release, under `compl_lock`), `bcast.c` (`bcast_publish`, under the subscriber lock), `ipc.c` (`ipc_send`, under the endpoint lock). Every lock passed to `sched_block_timeout` is static: `g_fx`, `g_inst`, `sub_a`/`sub_b`, `g_agent_ep`, `demo_ep`.
4. **`*done` wins, and `timeout_ticks == 0`** returns `-ETIMEDOUT` at once with `lk` held (Amendment 2 point 3).
5. **The free split (Amendment 2 points 1–2).** `sched_free_tcb` releases everything at once as before, then `rcu_defer_free(t)`. `sched_tcb_free_mem` frees the stack and the tcb after the reaper's grace period. `tcb.block_lk` and `tcb.rcu_next` are new fields, both initialised in `sched_create_state` (CLAUDE.md non-negotiable 11).
6. **A ceiling found on the way (measured, not new).** `sizeof(struct tcb)` is 10,240 bytes, above kheap's 2,048-byte slab limit, so every thread takes two of kheap's `MAX_LARGE` = 256 large-allocation slots (the tcb and its 16 KiB stack). The kernel therefore holds about 128 threads at most. The first local run of RE (f) (100 pinned waiters plus 3 yielders) could create only 48 threads: `spawn_fail=52`, `ring_len_max=128`. That is this ceiling, not P5b. It is recorded here and not changed by this PR.
7. **The arms as built** (`rcu_torture_p5b` in `rcu.c`; lines in `main.c`). Every line starts with the asserted fields, computed in the kernel, and the measured values follow as data. `[kline] TRUNC` is a global FORBIDDEN, so RE's data has its own `[rcu] E` line.
   - **RD** counts `g_sched_lock` acquisitions while any CPU's sweep runs, over the whole run, not only 1,000 idle ticks. It asserts `tick_lock=0` and at least 1,000 sweeps.
   - **RE (a)** is as designed: 20 waits of 50 ticks, one thread each.
   - **RE (b)** has 80 waiters, not 100, because of point 6. It also asserts `sat_acts_ok` (one sweep acted on more than 32). With four CPUs sweeping, a cap of 32 per sweep delays nobody below 128 expiries per tick, so lateness alone cannot show U8.
   - **RE (c)** uses 10-tick waits, not 50, for time. The waker ends the first wait at its deadline tick, which races the sweep.
   - **RE (d)**: the test thread pins itself to the highest ticking CPU (never the BSP, whose tick advances `g_ticks`) and holds the 32 locks with IF clear for 20 ticks.
   - **RE (e)**: the churn runs 120 ticks, not 500; that covers the 50-tick wait.
   - **RE (f)** has 64 waiters, not 100 (point 6).
   - Lateness is measured from the waiter's own reading of `g_ticks` before it calls `sched_block_timeout`. The kernel's deadline can be one tick later if the BSP ticks in between, so the reading can overstate lateness by one tick, never understate it.
   - Between sub-arms the test waits until the ring is back to its starting length, so that one sub-arm's unreaped zombies do not count against the next one's threads (point 6). `settle_max` records the longest wait.
   - **RF** runs in two phases, because one would hide the other's mutant. Phase 1 (U6) runs 500 create/exit/reap rounds, and every sweep pauses a third of a tick on each victim node and then checks `tid` and the 0xDD poison that the torture build writes over a freed tcb. Phase 2 (U9's window) runs 20 rounds, each holding the exiting victim on its CPU, as an unlinked current ZOMBIE, for two ticks after the reaper's unlink (the `za_hold` idiom). A hold in phase 1 would delay every free past any sweep's pause and make U6 vacuous. `zhits` counts the ticks that landed in that window. The F line also carries the whole run's `stalls` and `fails`, so a stall line or an `RCU FAIL` during P5b fails the gate.
   - `smoke-rcu`'s window is 420 s, up from 240.
8. **The reaper reaps in batches (a product change, measured).** A pass now takes up to `REAP_BATCH` = 64 zombies, then waits one grace period for all of them, then yields. With one zombie per pass, as before, every zombie would wait a grace period of its own. Same `smoke-rcu` build, local QEMU TCG, waiting after each sub-arm until the ring is back to its starting length: `settle_max=77` ticks with one zombie per pass, `settle_max=3` with 64. The reaper's behaviour before P5b (one per pass, no grace period) was not measured.
9. **Local runs on this head (screening, not CI evidence).** `dosfstools`, `mtools` and `qemu-system-x86` can be installed in this container, so the gates boot locally now (CLAUDE.md updated).
   - `smoke-rcu`: every arm passes. `PRADYOS_RCU_D tick_lock=0 sweeps_ok=1 | sweeps=11073`. RE: `late_max=0 sat_late_max=0 sat_acts_max=80 hol_late=0 churn_late=0 rq_late=0 rq_contended=4 spawn_fail=0`. `PRADYOS_RCU_F rounds=500 bad_node=0 hold_rounds=20 stalls=0 fails=0 | deferred_max=64 ring_len_max=94 zhits=40`.
   - `smoke-zanchor` (the membership pass removed, Amendment 2 point 5): `hits=48 escaped=0`, `PRADYOS_ZANCHOR_A reap_hit=8 destroy_hit=8`, `PRADYOS_ZANCHOR_B escaped=0`.
   - `smoke-shell`: 5/5.
10. **Changed after review (CodeRabbit on #125).** Every point below is a hole in the torture test, through which an arm could pass without exercising its race. None changes the product code.
    - The waiters' static locks are initialised once. A reset no longer rewrites a lock that a stale sweep may still hold, and the other fields are reset under that lock.
    - A waiter that starts after its absolute deadline fails its sub-arm's setup. Before, it waited zero ticks and counted as an on-time wake.
    - RE (d) creates the free-lock waiter first, so it comes after the 32 busy-lock waiters in the walk.
    - RE (e) requires churn waits completed while the measured waiter was blocked (`churn_overlap > 0`). The waiter itself reads a global churn count just before it blocks and just after it wakes, so a delayed controller cannot count churn from after the wait (second CodeRabbit round). The churn now runs 200 ticks.
    - RF phase 2 binds each round to one tcb (second CodeRabbit round). The victim is created blocked, its tcb is published, and only then is it unblocked. The hold runs only for that tcb and sees only that tcb's unlink, which still counts if it landed before the hold started. It acknowledges with that tcb once two of its CPU's ticks have landed after the unlink, and the round counts only on that acknowledgement. A phase-1 victim still on its way out can be neither held nor counted.
    - Amendment 2 point 4 no longer says a wait on a non-static lock is safe.
    - Also: RE (f) runs up to four rounds until the sweep has met contention. Every round must wake all 64 on time (`rq_woke_ok`). In local runs, `rq_contended` was 0 in 2 of 7 runs on code whose (f) was otherwise unchanged.
11. **Mutant screening, local QEMU (TCG), not CI evidence.** Each mutant is one change on `bcbd269`, run through `smoke-rcu`. CI runs on the final head follow.
    - U4 (the sweep takes `g_sched_lock`): red on RD, `tick_lock=13762` of 13,762 sweeps.
    - U6 (`sched_tcb_free_mem` in place of `rcu_defer_free`): red on RF, `bad_node=7`.
    - U8 (a per-sweep cap of 32): red on RE (b), `sat_acts_ok=0`.
    - U9 (walk anchored on `current_thread`): red on RF phase 2. `aborted=8` and `[apfreeze]`: the first held zombie wedged its CPU in the tick, as §5.3 predicted.
    - U5 (no same-wait re-checks) and U7 (deadline read before state, no acquire) **survived**. Both were stated possibly not gated before the code (§8). They are recorded as **not gated**.

# DDR-1209: lock-order checking for kernel spinlocks (C1.4, deadlock detection)

Status: design, then code in the same PR. Checklist item C1.4 "deadlock detection (lock-order checking)"; planned as DDR-1181 §2 P6 ("lock-order checking (lockdep-style), with a mutant that inverts an order"). Built under operator order #45 6993979920. QEMU only, not verified on real hardware.

## 1. What is asked, and what constrains it

- **The ask.** Find lock-order inversions (A then B on one path, B then A on another) before they deadlock: report the pair when the second order is first seen, whether or not the two paths ever ran at the same time.
- **Constraint 1, the fast path (DDR-1047).** `spin_lock` is one test-and-set and a branch. DDR-1047 refused any always-on cost on it because OPEN-2 is timing-sensitive and an always-on probe can move the bug it measures. A checker that walks a held-lock list on every acquire is exactly that cost.
- **Constraint 2, the layout (DDR-1047).** `spinlock_t` is one byte. Growing it shifts every struct that embeds one in the shipped kernel.
- **Constraint 3, opt-in instruments (DDR-1010).** An opt-in instrument is off in CI unless a gate turns it on. So the checker needs its own CI gate that runs it over real kernel work, not only over a self-test.
- **What the kernel does today (read from the tree at `d4ba5d7`).**
  - 49 `spinlock_t` declarations. About 25 are file-scope statics; the rest live in static arrays (`g_rq[PERCPU_MAX]`, `g_hr[]`, `g_fx[64]`, `g_w[]`) or in heap objects (IPC endpoints, broadcast buses and subscribers, capability tables, `kmutex`, virtio-blk instances).
  - The tick calls `schedule()` from interrupt context whenever the quantum expires (`sched_tick`, `kernel/proc/sched.c`). A kernel thread that holds a plain `spin_lock` with interrupts enabled can therefore be switched out while it holds it, and the next thread on that CPU takes other locks.
  - `schedule()` holds no spinlock across `context_switch` (rq-2; `sched_block_on` and `sched_block_timeout` release the lock before `schedule()` and retake it after).
  - Documented orders exist only as comments: heap → PMM (`kheap.c`, "the PMM never calls back into the heap, so no cycle"), bus → subscriber (`bcast.c`), the T3 table lock → wait lock (`lapic.h`), `mm_lock` → `mnt_lock` (`shm.c`). Nothing checks them.

## 2. Decision

**D1. A build flag, `LOCKDEP=1`, off in the shipped kernel and in every shard.**
- With `LOCKDEP=0` the spinlock header, `struct tcb` and every call site compile exactly as today. The shipped `kernel.bin` is expected to be unchanged, and the gate checks the default kernel's hash before and after, as `smoke-rcu` does.
- Only `make smoke-lockdep` builds `LOCKDEP=1`. It runs in ci.yml's `flag-gates` job beside `smoke-zanchor` and `smoke-rcu` (Constraint 3).
- **Not taken: always-on checking.** It breaks Constraint 1. Linux makes the same choice (`CONFIG_PROVE_LOCKING` is a debug configuration).

**D2. The class lives in the lock, in the `LOCKDEP=1` build only.**
- `spinlock_t` becomes `{ uint8_t v; uint8_t cls; uint8_t chk; }` under `LOCKDEP=1`, with `chk == (uint8_t)~cls` for a classed lock. `SPINLOCK_INIT` still zeroes all three, so `cls == 0, chk == 0` means "not yet classed". The default build keeps one byte (Constraint 2). Offsets that assembly reads are static-asserted already, so a shift in the checking build fails to compile rather than misbehaves.
- **Why the check byte:** `cap.c` initialises only `t->lk.v` on `kmalloc` memory, which is not zeroed. Without `chk`, leftover bytes would read as some class and add edges in its name.
  - **What it catches:** random leftover bytes (all but about 1 pair in 256 fail `chk == ~cls`) and a pair torn by a racing stamp. A pair that fails is treated as untracked and reported, so the gate names the site.
  - **What it does not catch:** memory that held a classed lock, freed and reallocated with the lock at the same offset. That pair is valid, and it would read as the old lock's class (CodeRabbit on #152).
  - **The safeguard chosen: initialisers write all three bytes.** `LOCKDEP_CLASS` stamps `cls` and `chk` at the annotated sites. Assigning `SPINLOCK_INIT`, or zeroing the object, writes them as 0, which reads as unclassed. The one form that leaves them as they were is an assignment to `.v` alone. A grep for that form finds one site in today's tree, `cap.c`, and that site is annotated.
  - This is an audit of today's tree, stated and not gated: no arm reallocates a classed lock under a partial initialiser. A new `.v`-only initialiser on an unannotated heap lock would reopen the case.
  - **Not taken: making `ld_valid` reject a stale but valid pair.** It would have to know that the memory was freed in between. The heap does not know where the locks in a block are. Zeroing every block in the checking build would hide uninitialised fields that the shipped build still has.
- A class is assigned on the first acquire of a lock whose `cls` is 0:
  1. **Annotated:** `LOCKDEP_CLASS(lock, "name")` gives every lock initialised at that call site one class. It is used for heap locks and for static arrays whose elements are one kind of lock (the 64 futex buckets). It compiles to nothing with `LOCKDEP=0`.
  2. **Static storage:** a lock in `.data`/`.bss` (`[__rodata_end, __bss_end)`, the range `sched_check_static_lock` already uses) gets a class of its own, keyed by its address. Re-initialising it (`(spinlock_t)SPINLOCK_INIT`) finds the same class again by address.
  3. **Anything else** (an unannotated heap or stack lock) is **untracked**: it is counted, and the first few addresses and call sites are recorded, but it adds no edges. The gate requires the count to be 0, so a lock family the annotation missed fails the gate and gets annotated. Untracked is never silently out of scope.
- **Why not a side table keyed by address (DDR-1047's lock_stat pattern):** a heap lock's address is reused after `kfree`. A table entry would then give a new, unrelated lock the old lock's class and its edges. A class stored in the lock dies with the lock.
- **Bound:** 127 classes (`cls` 1..127). A 128th is counted as `ovf` and the gate fails. Raising the bound is an amendment.

**D3. Held locks are tracked per thread, not per CPU.**
- Each `struct tcb` gains, under `LOCKDEP=1` only and at the end of the struct, a held stack of 16 entries (lock, class, call site). `sched_create_state` initialises its depth to 0 (`kmalloc` does not zero).
- **Why per thread:** a thread preempted while holding a plain `spin_lock` keeps its locks on its own stack. Per-CPU tracking would charge them to the next thread on that CPU and invent edges (A held by the preempted thread → B taken by the next one), then report false inversions. Arm LD tests exactly this.
- **Interrupt handlers** push onto the interrupted thread's stack. That is right for ordering: a handler that takes B while the interrupted code holds A (taken with interrupts enabled) is a real A → B nesting on that CPU.
- **No current thread** (early boot, an AP before its first thread): a per-CPU fallback stack is used, and a release looks there when the thread's own stack does not hold the lock (an AP adopts its idle thread under a lock). The checker turns on just before `smp_start_aps`, after the BSP's per-CPU identity: the APs then start under it, so none can hold a lock across the switch-on, which would read as `unmatched`. (Turning it on after `sched_init` left that window: the APs are already running then, waiting to join.) An AP sets its GS base before its first spinlock (`smp_ap_entry` calls `percpu_init_cpu` first). Every hook tests the on flag before it reads `%gs`, so nothing reads per-CPU state while the GS base can still be 0 (measured: a mutant that read `this_cpu()->is_bsp` on every acquire faulted the boot before its first line).
- Stack overflow (depth 17) is counted as `ovf`; the gate fails.

**D4. One global dependency graph, kept transitively closed.**
- `direct[a]` and `reach[a]` are 128-bit rows: `direct[a]` bit b means "b was taken while a was held"; `reach[a]` bit b means a path a → … → b exists.
- On each acquire of class c, for each held class h with `direct[h]` bit c not yet set, add the edge h → c under the checker's own lock, a raw test-and-set lock the checker does not track:
  - h == c is D5's case (recursion or same-class nesting) and adds no edge.
  - **inversion** if `reach[c]` has bit h: c already reaches h, so h → c closes a cycle. It is reported and the edge is not added, so the graph stays acyclic.
  - otherwise set `direct[h]` bit c, and for every x with x == h or `reach[x]` bit h, `reach[x] |= reach[c] | {c}`.
- Known edges cost a bit test, without the lock. A racy read only sends a known edge to the locked path, which finds it set.
- **Why transitive:** P → Q and Q → R make R → P a deadlock with three CPUs, although no pair of the three is inverted directly. Arm LC tests this.
- **Why global:** the two orders of an inversion are usually taken on different CPUs. Arm LE tests this.

**D5. What else is reported.**
- **Recursion:** acquiring a lock instance the thread already holds is a certain self-deadlock. Reported as `rec`; the gate fails on it.
- **Same-class nesting** (two instances of one annotated class held together, e.g. two IPC endpoints): counted as `nest` and reported, not counted as an inversion. Without an order annotation, which this DDR does not add, it cannot be told apart from a correctly ordered nesting.
- **Trylock:** a successful `spin_trylock` pushes the lock (later acquisitions get edges from it) but adds no edge into it: a trylock never waits, so it cannot close a cycle.
- **Release of a lock not on the thread's stack:** counted as `unmatched`; the gate fails on it. It is how a lock handed between threads, which D3 does not model, would show itself.

**D6. The hooks never print.**
- The check runs before the test-and-set, so a real inversion is recorded before the CPU can spin on it.
- Reports go into a bounded record (kind, the two classes, both call sites, the CPU). A kernel thread, `ldsum`, wakes every 2 s, prints new records, and prints a summary line when anything changed.
- **Why:** printing takes the console locks. A report raised while this thread holds a console lock would deadlock in the printer. The checker's own lock is never held while anything is printed.
- Real reports print `LOCKDEP INVERSION ...` and `LOCKDEP RECURSION ...`. Self-test classes (D7) print `[lockdep] test-inversion ...`, so a self-test can never produce the line the gate forbids.
- **The freeze (CodeRabbit on #152).** The runner ends the guest with a kill, not a shutdown, so a summary printed on a timer can trail the counters: a change in the last period is never printed, and a gate reading the last line would pass on a stale one. At its 120th wake (about 240 s) `ldsum` therefore freezes the checker:
  1. It takes the checker's lock, sets `frozen`, copies the counters and the record count, and releases the lock.
  2. It prints the records up to that count and one `[lockdep] final ...` line, then prints nothing more.
  - Every counter, record, class and edge is written under that lock and only while `frozen` is 0. Recursion, unmatched releases and a full held stack were counted outside the lock before; they now go through `ld_count`. Same-class nesting now takes the lock to count, which costs nothing while `nest=0`.
  - After the freeze nothing changes, so the final line is the state at the end of the run however late the kill comes. This holds by construction and is not gated: no arm provokes an inversion after the freeze.
  - The fast path is unchanged: a known edge is still one bit test with no lock.
  - Locks taken after the freeze are not checked (§6).

**D7. Self-tests run at boot in the `LOCKDEP=1` build only,** after the SMP proofs and before init, as `rcu_torture` does. Their locks are static locks marked as test classes, and their inversions are counted in `tinv`, not in `inv`.

**D8. The shipped kernel stays byte-identical.**
- `psi_tick` and `steal_pass` (`kernel/proc/sched.c`) take a run-queue lock with `__atomic_test_and_set(&q->lock.v, ...)` and release it with `spin_unlock`. The checker would never see the acquire and would count each release as `unmatched`, so in the checking build they call `spin_trylock`.
- **Measured:** making that change unconditionally, with the futex and `kmutex` class sites, kept every function's size but changed the bytes of five (`psi_tick` 234, `steal_pass` 245, `kmutex_lock` 254, `kmutex_unlock` 345, `bucket` 17): register allocation, not behaviour. Those five sites therefore keep their old form under `LOCKDEP=0`.
- Result: the default `kernel.bin` built from this tree is identical to `dev/phase1`'s, measured twice with `cmp` on local builds: against `d4ba5d7` (sha256 `12c20ffb823fcea0…`, 1,687,946 B), and after `dev/phase1` `0ae6251` (#150, C1.5 P6) was merged in, against that (sha256 `26f99e184bfd3057…`, 1,692,042 B). Code that DDR-1047 kept off the hot path for OPEN-2's sake is not touched.

**D9. Call sites are resolved offline.** The printed call sites are kernel addresses. They are resolved against that build's `kernel.elf` with `tools/ci/sym_at.sh` (CLAUDE.md non-negotiable 14), never with awk.

## 3. Gate: `smoke-lockdep`

- `LOCKDEP=1`, `QEMU_SMP=4`, `QEMU_PROBES=thread`, so the thread, futex, `psync` and `pthr` workload runs under the checker on four CPUs. The default kernel is put back and must come back with its hash.
- `FORBIDDEN_SENTINEL`: `LOCKDEP INVERSION`, `LOCKDEP RECURSION`. Not added to `GLOBAL_FORBIDDEN`: only this build can print them, and the count stays 78.
- **Arm LA (the real kernel):**
  - It reads the `[lockdep] final` line (D6), which must exist: a missing line (the checker never froze, or the boot died first) fails the arm.
  - **Order:** that line must come after `PRADYOS_THREAD_DONE`, so the freeze came after the workload and the checked window covers all of it.
  - It must have `inv=0 rec=0 unmatched=0 untracked=0 ovf=0`.
  - Vacuity: it must also have `hp=1`, meaning the edge heap → PMM, the order `kheap.c` documents, is in the graph. That edge exists only if the hooks see real nesting in real kernel code.
  - **Why a frozen line (CodeRabbit on #152):** the first implementation read the last `[lockdep] sum` line, printed only on change.
    - In both kept local logs that line is 39 and 52 lines before `PRADYOS_THREAD_DONE` (lines 538 and 525, against 577). That is still the end state while `ldsum` is alive, because a change would print a new line within 2 s.
    - But nothing proved `ldsum` was alive, and a change in the last 2 s before the kill would never have been printed.
    - Neither log has a `final` line, so the new parse fails both.
  - **Timing:** the gate declares a FORBIDDEN list, so `boot_test.sh` never exits early and the guest runs the whole 300 s window.
    - In both kept logs the workload ends between the `[hb]` lines `t=1000` and `t=1500` (10 ms ticks), and the `[hb]` lines run on to `t=29500` and `t=30000`.
    - A freeze at about 240 s therefore lands long after the workload and well before the kill.
    - A host slow enough to push the workload past the freeze fails the order rule; it cannot pass silently.
- **Arm LB (direct inversion):** X then Y, then Y then X, on the BSP. `PRADYOS_LOCKDEP_B inv=1`.
- **Arm LC (transitive):** P → Q, Q → R, then R → P. No pair is inverted directly, so only the closure finds it. `PRADYOS_LOCKDEP_C inv=1`.
- **Arm LD (preemption):** two kernel threads pinned to CPU 1. T1 takes U with `spin_lock`, interrupts enabled, and spins until T2 has run. T2 takes and drops V while T1 still holds U. Then the BSP takes V then U.
  - `overlap=1` says T2 really ran while U was held. Without it the arm proves nothing, so the line reports it.
  - `inv=0` says no false U → V edge was recorded.
  - `PRADYOS_LOCKDEP_D overlap=1 inv=0`.
- **Arm LE (across CPUs):** a kernel thread pinned to one AP takes A then B; then a thread pinned to another AP takes B then A, each with interrupts off around the pair. `PRADYOS_LOCKDEP_E cross=1 inv=1`, where `cross=1` says the two orders were really taken on different CPUs.
- **Vacuity, read before writing:** on today's tree none of these lines exists, so every arm is red. On the `LOCKDEP=1` tree each arm's line carries the observable that would be wrong if its mechanism were missing (below).

## 4. Mutants (each must be red on its own arm)

| Mutant | Change | Expected red |
|---|---|---|
| LD1 | The cycle test never fires. | LB, LC, LE |
| LD2 | The test reads `direct` instead of `reach`, so only direct reversals are seen. | LC only |
| LD3 | The held stack is the per-CPU fallback, always (no per-thread state). | LD (LA may go red too: the same false edges appear in the real workload) |
| LD4 | The hooks do nothing off the BSP. | LE (LB and LC too when the self-test thread runs on an AP) |
| LD5 | DDR-1181 §2's "mutant that inverts an order": once at boot, take the PMM lock and then the heap lock (`kfree(NULL)` under `g_pmm_lock`), against the documented heap → PMM order. | LA (`LOCKDEP INVERSION` held=PMM take=heap) |
| LD6 | The freeze comes at `ldsum`'s first wake, before the workload has run. | LA, the order rule only (the final line precedes `PRADYOS_THREAD_DONE`) |
| LD7 | The freeze never comes. | LA, the final line is missing |

Each mutant must be red on its own arm; red on others too is allowed. LD5 runs once, at the start of the self-test, and holds the heap lock for one `kfree(NULL)`: a concurrent heap → PMM path on another CPU could meet it and hang the boot, which is red as well, so the run's evidence must show the `LOCKDEP INVERSION` line, not only a timeout.

## 5. Cost and size

- **Default build:** byte-identical to `dev/phase1` `0ae6251` (D8: sha256 `26f99e184bfd3057…`, 1,692,042 B, local build).
- **`LOCKDEP=1` build** (local, on the P6 base): `kernel.bin` 1,704,330 B, 32,374 B under the 1,736,704 B gate, the same as the `RCU_TORTURE=1` build. Its BSS grows by 16,000 B:
  - the class table, 4 KiB;
  - the three 128 × 128-bit graph matrices, 2 KiB each;
  - the per-CPU fallback stacks, 4,224 B;
  - the report record, 1 KiB.
- Each `struct tcb` grows by 264 B (16 held entries).
- `__bss_end` lands at phys 0x5EF940, 67,264 B under the PT_HI ceiling; the default build's lands at 0x5E8AC0, 95,552 B under. CI prints the figures of record in `flag-gates (smoke-lockdep)`.
- Each acquire in that build calls the checker out of line: an interrupts-off walk of the thread's held stack (depth is small) and one bit test per held lock. Under TCG this is a cost in instructions, not a measured speed claim (CLAUDE.md non-negotiable 15).

## 6. Not covered (stated, not implied)

- **Interrupt safety** (a lock taken in an interrupt handler and, elsewhere, with interrupts enabled: a single-CPU deadlock). Linux checks it separately; this DDR does not. Candidate for an amendment.
- **Sleeping locks:** `kmutex` (only its inner spinlock is seen), `mnt_lock`, futex-based user locks.
- **Ordered same-class nesting** needs an annotation this DDR does not add (D5).
- **Recursion is detected, not gated:** a test that takes a lock twice would hang the CPU after the report.
- **Locks taken after the freeze** (about 240 s into a 300 s run, D6) are not checked. The workload has ended by then, and the order rule fails the run if it has not.
- **The shipped kernel checks nothing** (D1). The checker finds orders that the gate's workload exercises; an order no gate runs stays unseen.
- QEMU (TCG, 4 vCPU) only, not verified on real hardware.

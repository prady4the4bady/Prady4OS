# DDR-1181 — Threads and synchronization (C1.4), part P1: kernel threads that share one address space

**Status:** ACCEPTED (Order 4, #45/6301435736, item 1: start C1.4 on C1.3 P1; Order 3, #45/6282431738: decisions delegated).
**Security boundary: review before merge.** P1 changes who can write an address space, and it changes the lifetime of an address space, its capability table and its memory charge.

## 1. Where the tree is, measured at `dev/phase1` = `a2ac698`

- **One tcb is one process.** `struct tcb` (`kernel/proc/sched.h:49`) carries the address-space state directly:
  - `cr3`;
  - `vma[VM_AREA_MAX]`;
  - `mmap_next`;
  - `mem_used`/`mem_limit`.
  It also carries the process state: `fdt`, `caps`, `fs_cap`, `root_mnt`, the signal fields, and the AETHER budgets. `pid == tid` (`sched.c:1294`).
- **The address space has no owner object and no refcount.** It is destroyed by `vmm_destroy_address_space(cr3)` when the tcb is reaped: in `wait4` (`sys_wait.c:77`) and in the orphan reaper (`sched.c:2711`).
- **No `clone`, no futex, no waitqueue type.** The only lock is a test-and-set `spinlock_t`. Blocking uses `sched_block_on` / `sched_block_timeout` (DDR-955) / `sched_unblock`.
- **TLB shootdown already covers threads.**
  - `tlb_shoot(cr3, …)` (`kernel/mm/tlb.c:95`) targets every CPU whose *current thread* has `t->cr3 == cr3`. Threads that share a `cr3` value are therefore shot with no change.
  - On the switching CPU, `sched.c:1696` skips the CR3 reload when `next_cr3 == prev_cr3`, so a same-CPU switch between sibling threads does **not** flush the TLB. On that CPU the protection is the local `invlpg` that `vmm_protect_range` (`vmm.c:307`) and `vmm_unmap_range_flush` (`vmm.c:331`) already issue.
- **A ring-3 fault kills the faulting tcb.** The path is `sched_exit(-1)` (`idt.c:820-905`); COW and stack-growth faults are resolved first (`idt.c:834`, `:845`). Signal delivery runs on the timer-IRQ return to ring 3 (`idt.c:636`) and on syscall exit. So a pending `SIGKILL` reaches a thread that is spinning in user space without making any syscall (DDR-1174).
- **`cap_table` has no lock** (`kernel/cap.c:15`). Each process has its own table, built by `cap_fork` at fork.
- **The AETHER budgets are per-tcb** (`bud_*`, `mem_used`, `rb_log`). Fork is already refused for a rollback task (`sys_fork.c`, DDR-1171), because a child would escape the log.

## 2. The C1.4 parts and their order

| Part | Content | Needs |
|---|---|---|
| **P1** (this DDR) | Kernel threads that share one address space: `SYS_THREAD_CREATE` / `SYS_THREAD_EXIT`, thread groups, group exit, a refcounted `struct mm`, the mm lock. Gate `smoke-thread`, including the three DDR-1177 items. | C1.3 P1 (merged `a2ac698`; the gate uses `MAP_SHARED` anon) |
| P2 | futex (`WAIT`/`WAKE`, private and shared), `CLEARTID` wake. | P1 |
| P3 | Kernel `kmutex` with priority inheritance; user mutex, semaphore, condition variable and barrier on top of futex. | P2 |
| P4 | musl pthread: build it, add a `clone.s` overlay (it hard-codes 56/60), and add `set_tid_address`, `set_robust_list`, `rt_sigprocmask`, `mprotect`. Shared fd table and shared signal dispositions (the P1 divergences in §3.6). | P2, P3 |
| P5 | RCU (one grace-period scheme, chosen in its own amendment). | P1 |
| P6 | Lock-order checking (lockdep-style), with a mutant that inverts an order. | P3 |

C1.3 P2–P4 (SysV/POSIX shm, then shared file mappings) proceed in parallel. They do not depend on C1.4, and C1.4 P1 depends only on C1.3 P1.

## 3. P1 decisions

### 3.1 `struct mm`, a refcounted address space

A new object, `kernel/mm/mm.h`:

```c
struct mm {
    uint64_t       cr3;                  /* page-table root, constant for the mm's life */
    struct vm_area vma[VM_AREA_MAX];     /* moved from struct tcb */
    uint64_t       mmap_next;            /* moved from struct tcb */
    uint64_t       mem_used, mem_limit;  /* moved: the charge is per address space */
    uint32_t       refs;                 /* tcbs holding this mm (atomic) */
    uint32_t       live;                 /* threads not yet exited (atomic) */
    struct kmutex  lock;                 /* §3.3 */
};
```

- Every user tcb holds `t->mm`. `t->cr3` stays as a cached copy, equal to `t->mm->cr3`. It is cached because `tlb_shoot`, the context switch and 27 other readers use it, and it never changes while `refs > 1` (§3.5, exec).
- `mm_put()` replaces each direct `vmm_destroy_address_space(cr3)` on the reap paths (`wait4`, the reaper, and the fork failure path). The address space is destroyed when the last reference is dropped, never before.
- The move is **P1a**: a pure refactor, with no new syscall, landing first. Its evidence is the whole existing gate set green in one CI run. The blast radius, measured: `->vma` 10 refs in 3 files, `->mmap_next` 7 refs in 4 files, `->mem_used`/`->mem_limit` 9 refs in 3 files.
- `mem_used`/`mem_limit` move with it. If they stayed per-tcb, each new thread would bring its own memory allowance, so N threads could map N times the process limit. That is an isolation bypass, and moving the charge onto the shared object is the choice safer for memory isolation (Order 4, item 2).

### 3.2 `SYS_THREAD_CREATE` (NSI 110) and `SYS_THREAD_EXIT` (NSI 111)

- `thread_create(entry, stack_top, arg, tls, clear_tid_addr)` returns the new tid, or a negative errno.
  - The new tcb shares the caller's `mm` (`refs++`, `live++`), and it starts with `rip = entry`, `rsp = stack_top`, `rdi = arg` and `fs_base = tls`.
  - `stack_top` must be 16-byte aligned and inside a mapped user range (`vmm_user_range_ok`); otherwise the call returns `-EINVAL`.
  - The caller owns the stack (it maps it with `mmap`). Stack growth on fault (ADR-038) stays limited to the main stack range.
- `tgid` is a new tcb field, initialised in `sched_create_state` (CLAUDE.md rule 11, because `kmalloc` does not zero memory).
  - It equals `pid` for a process and the leader's pid for a thread.
  - `getpid` returns `tgid`.
  - `pid` stays unique per tcb, and it is the tid.
- `thread_exit(status)` exits the calling thread only. Before the thread stops running:
  - if `clear_tid_addr` is set, the kernel writes a 32-bit 0 there, using `copyout` into the shared mm;
  - it drops `live`;
  - when `live` reaches 0, the exit is the process exit, and `status` becomes the process exit status.
- **Refused callers:**
  - a rollback task, with `-EPERM`, for the same reason as fork (DDR-1171);
  - an agent or a budgeted task (`is_agent`, or `bud_*` armed), with `-EPERM`. Their budgets are per-tcb today, so threads would multiply them. The refusal is lifted only when the budgets move to a shared object, recorded as a follow-up.

### 3.3 The mm lock

All mutations of an address space hold `mm->lock`:

- `mmap`, `munmap`, `mprotect`;
- the `MAP_SHARED` paths (DDR-1178);
- io_uring setup (it advances `mmap_next`);
- the surface and framebuffer maps;
- the COW and stack-growth fault paths;
- the copy of the address space in `fork`. A sibling thread can change the page tables while they are being copied, so the copy must hold the lock.

**The lock is a sleeping lock (`kmutex`), not a spinlock.** The holder can call `tlb_shoot`, and `tlb_shoot` waits for every target CPU to acknowledge an IPI. A waiter that spun on a spinlock with interrupts off would never acknowledge, which would deadlock the holder. A spinlock that waits with interrupts on would, in a kernel that can be preempted, spin out its quantum behind a preempted holder.

The `kmutex` therefore:

- puts waiters to sleep (`sched_block_on`), with interrupts enabled while they sleep;
- lets the holder run with interrupts off only inside `tlb_shoot`, as it does today.

The fault paths may sleep, because a ring-3 fault holds no kernel lock (`idt.c:826-830`). P1's `kmutex` has no priority inheritance; P3 adds it.

### 3.4 Group exit

- **`SYS_EXIT` (NSI 4) keeps its meaning: it ends the process.** Every existing caller is single-threaded and depends on that meaning. With threads it becomes exit-group: the kernel posts `SIGKILL` to every other live thread of the group, then exits the caller.
- **A fatal ring-3 fault, or a `SIGKILL` sent to the process, also ends the whole group.** This is POSIX: the default action of `SIGSEGV`/`SIGKILL` terminates the process. A thread spinning in ring 3 is reached on its next timer-IRQ return (§1).
- **The reap rules are:**
  - non-leader threads are reaped automatically by the reaper, which drops their mm reference;
  - the leader is a zombie that `wait4` can reap only once `mm->live == 0`;
  - the exit of the last thread wakes the leader's waiter.

### 3.5 exec and fork with threads

- `execve` while `mm->refs > 1` returns `-EBUSY`. POSIX would kill the sibling threads; that is deferred to P4, recorded. This keeps `t->cr3` constant while the address space is shared.
- `fork` from a thread copies only the calling thread (POSIX). The address-space copy holds `mm->lock` (§3.3).

### 3.6 What is shared, what is copied, and the divergences

| State | P1 | POSIX | Note |
|---|---|---|---|
| address space, VMAs, memory charge | shared (`struct mm`) | shared | §3.1 |
| capability table | **shared**, refcounted, with an internal lock added | per-process | Copying it would let one thread drop a capability while a sibling keeps it, so a privilege drop would not hold. Sharing is the choice safer for isolation. |
| `fs_cap`, `root_mnt` | copied by value at create | per-process | Neither changes after load. |
| fd table | **copied** (`fd_clone`) | shared | Divergence. A file opened in one thread is not visible in another. Fixed in P4, before musl pthread is built. |
| signal dispositions and mask | dispositions copied, mask per-thread | dispositions shared, mask per-thread | Divergence for dispositions, fixed in P4. A signal sent to a pid goes to the tcb with that pid. |
| FPU state, `fs_base` | per-thread | per-thread | |

### 3.7 User copies race a sibling's `munmap`/`mprotect` (found while building P1b)

**Measured on `a2ac698`:**
- `copyin`/`copyout` (`kernel/mm/uaccess.c:14-40`) validate the user range with `vmm_user_range_ok`, then `memcpy` through the user VA. `copyinstr` does the same one byte at a time. There is **no exception fixup**: a kernel-mode `#PF` goes to the panic path (`idt.c:1025`).
- `CR0.WP` is 1 on every CPU. DDR-1126 sets it in `cpu_enable_sse`, which runs on the BSP (`main.c:4740`) and on each AP (`smp.c:278`), and `smoke-wxkernel` enforces it. So a ring-0 write to a read-only user page **faults**. (A first draft of this section said WP was unset, after reading only the assembly entry paths. That was wrong, and it was corrected before any code was written.)

**Why this is harmless today:** one tcb is one address space, and a syscall is the only code that runs in it while the copy runs. Nothing can change the mapping between the check and the copy.

**What threads change:** a sibling on another CPU can `munmap` the buffer between the check and the copy, and the kernel faults in ring 0, which is a **panic**. Any program could then crash the kernel. A sibling that calls `mprotect(PROT_READ)` in the same window makes the kernel's write fault too, because `WP` is set, so that case is also a panic. Holding `mm->lock` across every user copy is not possible: copies run with spinlocks held and interrupts off, where a sleeping lock cannot be taken.

**Decision (P1b):**
1. **Uaccess fault fixup.** Every user copy goes through one assembly routine, `uaccess_copy(dst, src, n)` (a `rep movsb` between `stac`/`clac`). The kernel `#PF` handler, on a ring-0 fault whose RIP lies in that routine's text range, resumes at the routine's fixup label, which returns `-EFAULT`. `copyin`, `copyout` and `copyinstr` keep their up-front `vmm_user_range_ok` check as the fast refusal. The fixup covers only the race. Any other ring-0 fault still panics.
2. No `CR0` change is needed (WP is already set; see above).
3. **Gate arm G (in `smoke-thread`, P1c):** `PRADYOS_THREAD_G survived=1`, with `efault=<n>` on a `[thread] G` line. One thread loops `SYS_DMESG(buf, 4096)` (a 4 KiB `copyout`, the widest single copy any syscall makes) into a page for 1.5 s, while a sibling loops `munmap`/`mmap` of that page at the same VA.
   - Correct code survives and counts the `-EFAULT` returns.
   - **Mutant T8** (the `#PF` handler ignores the fixup range) panics. The panic banner `NEXUS KERNEL PANIC` is already in `GLOBAL_FORBIDDEN` (checked: count 77), so the gate goes red.
   - The arm's pass condition is `survived=1`; `efault` is recorded, not asserted, because the count depends on timing.

### 3.8 Known limitation of P1, recorded

A group exit posts `SIGKILL` to every sibling. A sibling that is spinning in ring 3, or polling through `yield` (DDR-1090), dies at once. A sibling blocked in `sched_block` (a pipe read with no writer, for example) does not wake for `SIGKILL`; this is the same as for a single-threaded process today. The leader then stays a zombie that `wait4` cannot reap until that sibling wakes. Interruptible sleeps for `SIGKILL` belong to P2 (futex), which reworks every blocking wait. `smoke-thread` never blocks a sibling in the kernel, so it does not depend on this.

## 4. Gate `smoke-thread`

A FAT-rooted probe, `user/threadtest.c`, opt-in (`probe_enabled("thread")`), run with `QEMU_SMP=4`. The probe is strict.

- **Arms D, E, M and F run in a forked child** (the test process). The parent learns the results through a `MAP_SHARED` anonymous page (C1.3 P1) and through `wait4`.
- **Bounded waits.** Every wait in the parent is bounded; on a timeout the parent sends the child `SIGKILL` and prints what it saw. This matters because a mutant that leaves a stale TLB entry also leaves the child alive and never faulting.
- **Measuring after the fact.** Every "after" measurement reads a flag that is set after the operation returns. The pattern is `f = shm->done; access; if (f) count++`: a count can only come from an access that started after the operation returned.

| Arm | Sentinel | What it shows |
|---|---|---|
| A | `PRADYOS_THREAD_A shared=1 joined=1` | A store by the thread is seen by the creator (one address space), and `clear_tid` is written at thread exit. |
| B | `PRADYOS_THREAD_B tls_ok=2` | Two threads with distinct `tls`; each reads its own `%fs:0` self-pointer. |
| C | `PRADYOS_THREAD_C leftover=0 zombie=0` and `PRADYOS_THREAD_C2 net_leak_ok=1` | 8 warm-up cycles, then 64 create/exit cycles. **C:** within 2 s, no `THREADTEST` tcb except the probe's own is left in the scheduler (`SYS_GETPROCS`). **C2:** the PMM free-page drop over the cycles, taken from a trial with quiet traffic around it (`pm_measure`, DDR-1185), is at most 32 pages; no clean trial is red. Printed on a `[thread] C drop= trials=` line. **Amended:** see the note after the mutant table. |
| D | `PRADYOS_THREAD_D prot_stale=0 faulted=1` | DDR-1177 arm D. A thread on another CPU writes a page in a loop; the creator calls `mprotect(PROT_READ)`. No write starts after `mprotect` returns and succeeds, and the writer faults. |
| E | `PRADYOS_THREAD_E stale=0 faulted=1` | DDR-1177 arm E. A thread on another CPU reads a page in a loop; the creator calls `munmap`. No read starts after `munmap` returns and succeeds, and the reader faults. |
| M | `PRADYOS_THREAD_M poison=0` | DDR-1177 M4. Same shape as E, counting reads that return a value other than the pattern. With the KASAN free-poison (`PMM_POISON`, on by default), a frame freed before every CPU acknowledged is read back as poison through the stale entry. |
| F | `PRADYOS_THREAD_F group_killed=1` | A thread faults deliberately while the leader spins in ring 3. The whole child dies, within the bound. |

`PRADYOS_THREAD_DONE` is printed last. `THREAD FAIL` and `thread probe FAILED to load` are forbidden.

**Repetition, because cross-CPU placement is not under the probe's control.** P1 has no affinity call. Arms D, E and M are each run as **16 trials**, and each trial starts the loop thread plus two spinner threads, so that the four vCPUs are busy and the loop thread is unlikely to share a CPU with the creator.

- A trial counts only if the loop thread was seen making progress before the operation.
- The sentinel sums the stale/poison counts over the trials.
- `faulted` is 1 only if every trial faulted.

The probability argument is not evidence. The evidence is each mutant shown red in CI, as for every gate (CLAUDE.md rule 3).

**Vacuity, checked per arm against the tree before the new syscall:**

- arms A to F cannot run on that tree at all, because `SYS_THREAD_CREATE` returns `-ENOSYS`; the probe then prints `THREAD FAIL: create`, which is forbidden;
- the meaningful vacuity test is therefore each mutant below, applied to the finished P1.

**Mutants. Each lands on a different arm and is shown red in CI before merge:**

| Mutant | Change | Arm it must turn red |
|---|---|---|
| T1 | `thread_create` gives the thread a COW copy of the mm instead of the shared one | A (`shared=0`) |
| T2 | `thread_create` ignores `tls` (inherits the caller's `fs_base`) | B |
| T3 | the reaper skips non-leader threads (their kernel stack and tcb are never freed) | C |
| T4 | `vmm_protect_range` skips `tlb_shoot` | D (`prot_stale>0`) |
| T5 | `vmm_unmap_range_flush` skips `tlb_shoot` (local `invlpg` only) | E (`stale>0`) |
| T6 | DDR-1177 M4: `vmm_unmap_range_flush` frees each frame before the shootdown | M (`poison>0`) |
| T7 | a fatal fault ends only the faulting thread | F (`group_killed=0`) |
| T8 | the kernel `#PF` handler ignores the uaccess fixup range (§3.7) | G (kernel panic) |
| T9 | `sched_free_tcb` frees a non-leader thread's tcb but not its kernel stack (DDR-1185 port) | C2 (`net_leak_ok=0`; C stays `leftover=0`) |

If T4, T5 or T6 cannot be shown red within 16 trials, the trial count is raised. The DDR is amended with the measured red rate; the arm is not dropped.

**Arm C amended (P1c).** The first version bounded the raw PMM free-page drop over the 64 cycles at 32 pages. Two runs printed the following:

| Run | Head | Free-page drop | Leftover / zombie |
|---|---|---|---|
| 39551618166 | `22666b8` | 375 | 0 / 0 |
| 40124472347 | `f702a5d` | 363 | 0 / 0 |

In the same window, the serial log shows the rest of the boot running:
- CMUSL, FPU, INIT and the PRISM shell starting;
- service restarts;
- the SMP and block probes;
- about ten SFS mount/umount cycles;
- provisioning of the persistent SFS root;
- the AETHER daemon starting.

Reading the code, every reaped thread's tcb and kernel stack go back to the PMM:
- the reaper takes a thread zombie, whose `parent_pid` is 0;
- `sched_free_tcb` frees fds, caps, the kstack and the tcb;
- a large `kfree` calls `pmm_free_pages` (`kheap.c:270-277`).

**Not verified:** which of the two explains the drop. The drop is consistent between runs, and it is close to one tcb plus one kernel stack (about 6 pages) per thread. So a real per-thread leak is not ruled out.

The arm is therefore not relaxed. It is split so that it measures the threads:
- **C** counts leftover thread tcbs exactly. T3 leaves 64 zombies.
- **C2** keeps the 32-page bound, but on the drop over the cycles **minus** the drop over an idle control window of the same length. Steady boot activity cancels, while a per-thread leak (about 380 pages for 64 threads) does not.

The next run's `[thread] C drop_cycles= drop_idle=` line decides the attribution, and the result is recorded here.

**Result: the idle control was not a sound denominator (DDR-1185).** The C2 above did not give the same answer twice on the same code:

| Run | Head | C2 line | Verdict |
|---|---|---|---|
| [40210141897](https://github.com/prady4the4bady/Prady4OS/actions/runs/40210141897) | `aceaa04` | net=470; every thread reaped (`left=0 zombie=0 reap_ms=0`) | red |
| [40212901781](https://github.com/prady4the4bady/Prady4OS/actions/runs/40212901781) | `3d5f85b` | numbers not printed (the PASS echo grepped a dead pattern); `munmap` return now counted, no other change | green |
| [40217768311](https://github.com/prady4the4bady/Prady4OS/actions/runs/40217768311) | `3b904aa` | `drop_cycles=-16 drop_idle=0 net=-16 span_ms=20`, `munmap_fail=0`, **`drop_pre_2000ms=293`** | green |

The cycles take 20 ms. In the same boot, an idle 2 s window with no thread of ours lost 293 pages. Background traffic of the same size as the "leak" arrives in bursts. Whether it lands in the cycle window or in the control window is timing, and that decided the verdict. **Mechanism:** the measurement is not attributable. It is not a per-thread leak: on `3b904aa` the cycles themselves moved −16, and `munmap` never failed. Which processes produce the bursts is **not verified**.

**C2 now** takes its drop from `pm_measure` (`user/include/pagemeter.h`, DDR-1185):
- The 64 cycles plus the reap wait are one trial.
- A trial counts only with a quiet free count before it and after it (≤ 4 pages over 250 ms) and no foreign process start or exit across it.
- `net_leak_ok = conclusive && drop ≤ 32`. The bound is unchanged.
- The idle-control subtraction is gone, because a clean trial needs no correction. **That makes the arm stricter, not weaker.**
- No clean trial within 30 s is red.
- Printed: `[thread] C drop= trials= rej_pre= rej_post= rej_procs= conclusive=`.

The ungated X/Y split diagnostic is removed. It used the same unattributable count, and its question (mmap or thread?) is answered by the cycles' own −16. **C/C2 evidence on the meter (smoke-thread, shard 2; QEMU TCG):**

| Tree | Head | Run | Verdict | Measured |
|---|---|---|---|---|
| correct | `93c1ea9` | [40222363410](https://github.com/prady4the4bady/Prady4OS/actions/runs/40222363410) | smoke-thread green | `[thread] C drop=0 trials=2 rej_pre=0 rej_post=0 rej_procs=1 conclusive=1`; `leftover=0 zombie=0`; `munmap_fail=0` |
| T9 | `c59b259` | [40222449102](https://github.com/prady4the4bady/Prady4OS/actions/runs/40222449102) | red on **C2 only** | `drop=256 trials=1 conclusive=1` (64 leaked 16 KiB kernel stacks = 256 pages); `PRADYOS_THREAD_C leftover=0 zombie=0` |
| T3 | `ecec751` | [40222447291](https://github.com/prady4the4bady/Prady4OS/actions/runs/40222447291) | red on **C** | `THREAD FAIL: create` after arm B; the C line is never printed |

**T3's mechanism differs from the one this table predicted** ("leaves 64 zombies"). Un-reaped thread zombies keep their reference on the shared mm, so `mm->live` only grows. `sys_thread.c:45` refuses a create with `-EAGAIN` at `THREAD_MAX_PER_MM` (64). Arms A and B leave 3 zombies, so a create inside C fails at the 64-thread cap and the probe stops with a forbidden `THREAD FAIL` line before C is printed. The arm is red and it is arm C; the path is the thread cap, not the leftover count. That reading is from the code, and it matches the log (no C line, a create failure); it is **not** traced in the kernel.

**T9 also printed 14 `THREAD FAIL: create` lines in later arms.** A plausible reading, **not verified**: the leaked kernel stacks stay in kheap's large-allocation registry (`MAX_LARGE` 256, `kheap.c:38`), so later large `kmalloc`s fail. C2 is the first arm to fail, which is the designed one.

The correct-code run shows the meter rejecting the first trial for `rej_procs` (a foreign process start or exit) and measuring 0 on the second, as smoke-shm did in DDR-1185 §5.

**Line integrity.** Every line the probe prints leaves in one `write`. `say()` and `line1()` build each line in a stack buffer (a probe may not carry a writable section, DDR-826). Arms D, E, M and F print right after a child dies with a kernel `[trap]` line. A line written in pieces could be torn by that line, or by another process's output. CI run 39504298130 tore arm F; DDR-1182 §3.1 shows the same failure in another probe.

### 4.1 Amendment: three arms were not sensitive to their mutants

All mutants T1–T8 were run on `675b45c` (smoke-thread, shard 2; QEMU TCG, 4 vCPUs). Correct code is green on that head (run [40234479561](https://github.com/prady4the4bady/Prady4OS/actions/runs/40234479561)).

| Mutant | Head | Run | Designed arm | Result |
|---|---|---|---|---|
| T1 | `069ebfc` | [40261324185](https://github.com/prady4the4bady/Prady4OS/actions/runs/40261324185) | A | red, **but A passed** (`shared=1 joined=1`); red via C2 (`drop=1036`) and D/E (`prot_stale=230116208`, `stale=164341344`, `faulted=0`) |
| T2 | `31e2b89` | [40261343508](https://github.com/prady4the4bady/Prady4OS/actions/runs/40261343508) | B | red on B (`tls_ok=0`) |
| T3 | `ecec751` | [40222447291](https://github.com/prady4the4bady/Prady4OS/actions/runs/40222447291) | C | red on C (see above) |
| T4 | `642f5ac` | [40261362012](https://github.com/prady4the4bady/Prady4OS/actions/runs/40261362012) | D | **green** (`prot_stale=0 faulted=1`, 16 trials) |
| T5 | `3216e2f` | [40261380557](https://github.com/prady4the4bady/Prady4OS/actions/runs/40261380557) | E | **green** (`stale=0 faulted=1`, 16 trials) |
| T6 | `975d2a7` | [40261398624](https://github.com/prady4the4bady/Prady4OS/actions/runs/40261398624) | M | not yet reported |
| T7 | `81088e0` | [40261416987](https://github.com/prady4the4bady/Prady4OS/actions/runs/40261416987) | F | red on F (`group_killed=0`) |
| T8 | `71864fe` | [40261435864](https://github.com/prady4the4bady/Prady4OS/actions/runs/40261435864) | G | **green** (`efault=10546 survived=1`) |
| T9b | `8a3eed2` | [40234544145](https://github.com/prady4the4bady/Prady4OS/actions/runs/40234544145) | C2 | red on C2 only (`drop=256`, `leftover=0`) |

A gate never seen red proves nothing, so **arms A, D, E and G prove nothing yet**, and the DDR-1177 arm D/E checklist items stay unticked. The mechanisms below are read from the probe and the kernel source. They are **not traced** at run time.

1. **A cannot see a copied address space.** The thread stores into `struct shared`, which is `MAP_SHARED` because the fork arms need it. A COW copy of the mm (T1) keeps `PTE_SW_SHMEM` pages shared, so the creator still sees the store. T1 was caught only incidentally, by C2, D and E.
2. **D and E never prove the loop thread is on another CPU.** The creator warms the loop thread up with `SYS_YIELD` and then calls `mprotect`/`munmap`. If the loop thread is on the creator's vCPU at that moment, the creator's own `invlpg` removes the entry and a missing `tlb_shoot` (T4, T5) changes nothing. The probe has no CPU pinning and no user-visible CPU id (no `IA32_TSC_AUX`, no getcpu; QEMU's default `qemu64` model). The two `t_spin` threads make a separate vCPU likely, not certain.
3. **G never reaches the fixup instruction.** The `-EFAULT`s it counts come from the `vmm_user_range_ok` pre-check in `copyout` (`uaccess.c`). The fixup is only reached when the page disappears after the check and before the copy touches it. A one-page destination that is either mapped or not gives that window almost no width.

**Redesign (implemented in the next commit):**

- **A** stores into a `MAP_PRIVATE` page that the creator mapped before the thread exists, and the creator reads it back. One shared address space shows the store; a COW copy (T1) faults the store onto a private copy, and the creator reads 0. The sentinel is unchanged.
- **D, E, M: concurrency-proven trials.** After the warm-up the creator stops making syscalls. It spins (up to 200,000 iterations) and requires the loop thread's counter to advance by at least 1000 while the vDSO clock does not move. Since the creator never yields inside it, an advancing counter means the loop thread runs on another CPU at the same time. Only then does the creator call `mprotect`/`munmap`; otherwise it yields and tries again, up to 50 times. A trial is **placed** when that check passed. Each arm prints `placed=` and requires at least 12 of 16 placed trials (`placed_ok=1`). Fewer is red, so an arm whose trials never overlap can no longer pass by default. `prot_stale`, `stale` and `poison` are summed over placed trials only.
- **G: mid-copy faults are counted.** The destination is `[A+256, B+256)` across two pages, and the sibling toggles only page B. Before each `SYS_DMESG` the copier zeroes A's tail. An `-EFAULT` that leaves A's tail written means the copy started (the pre-check passed) and faulted on B, which is the fixup path. G requires `survived=1` **and** `midcopy=` at least 1 (`midcopy_ok=1`). Under T8 that fault panics the kernel (`NEXUS KERNEL PANIC` is in `GLOBAL_FORBIDDEN`).

**Re-run plan:** correct code, then T1 (A), T4 (D), T5 (E), T6 (M) and T8 (G) on the redesigned probe. T2, T3, T7 and T9b exercise arms B, C, F and C2, whose probe code is unchanged; their reds above stand.

### 4.2 Amendment: the redesigned probe is red on correct code (`9fbd233`)

Run [40270599838](https://github.com/prady4the4bady/Prady4OS/actions/runs/40270599838), smoke-thread on shard 2, QEMU TCG with 4 vCPUs. Arms A, B, C, C2, F and M print their expected values (`shared=1 joined=1`, `tls_ok=2`, `leftover=0 zombie=0`, `net_leak_ok=1`, `group_killed=1`, `poison=0`). Three arms fail:

| Arm | Printed | Mechanism |
|---|---|---|
| G | `THREAD FAIL: G remap`, `efault=0 midcopy=0` | **Named, from the source.** `munmap` removes a whole region, found by its exact base (`vma_find` compares `base ==`, `sys_munmap_locked` ignores the length; `sys_mmap.c`). The probe mapped pages A and B as **one** 2-page region and unmapped B = base + 4096. That call returns `-EINVAL` and unmaps nothing. The `mmap` at B then overlaps, returns `-EINVAL`, and the probe fails on the first iteration. This is a probe defect. |
| D, E | `faulted=0` | **Named, from the log.** D ran from about `t=500` to `t=2000` (heartbeat ticks, 10 ms each), which is about 15 s for 16 trials, and only one D trap line was printed. The parent's `reap(pid, 1000)` timer starts at `fork`, and the placement loop (up to 50 attempts, each a spin plus a `yield`) runs inside that window. So 15 of the 16 children were killed before they reached `mprotect`/`munmap`. This is a probe defect: the bound was written for "fault within 1 s of the operation", and the redesign moved a long phase in front of the operation. |
| D, E | `placed=1 of 16`, `placed=0 of 16` | **Not named.** See below. |

**What the artefact does show about placement:**
- The vCPUs are separate host threads. QMP `info cpus` lists four distinct `thread_id`s, so QEMU runs multi-threaded TCG.
- All four CPUs are online (`[smp] cpus online=4/4`).
- The run queues are deep during D and E: `rqdepth=7` and `rqq=15` in every heartbeat from `t=1000` to `t=4000`.

**What it does not show** is why an attempt fails. The candidates below predict different counters:

- **H1:** the loop thread is not running while the creator spins. The count advances by less than 1000 and the vDSO clock does not move.
- **H2:** it runs, but the window crosses a tick. The count reaches 1000 and the clock moves.
- **H3:** `pause` is what prevents overlap. Under multi-threaded TCG, a `pause` ends the vCPU's execution loop, and the vCPU thread then retakes QEMU's global lock. The creator and both spinners execute `pause` in tight loops. The loop thread's vCPU also needs that lock each time it leaves its execution loop (every timer interrupt), so it could stall behind them. This is QEMU behaviour as I recall it, **not verified** against the QEMU source for the runner's version. H3 predicts H1's counters, and more placements when nobody executes `pause`.

**Next step: instrument, then decide.** No verdict changes. The two named defects are fixed, and the probe gains ungated diagnostic lines:

- **G:** pages A and B are mapped as two one-page regions. The bump allocator places the second directly after the first, and the probe checks `B == A + 4096` (`THREAD FAIL: G layout` otherwise).
- **D, E:** the parent waits (bounded at 3 s) until the child has called `mprotect`/`munmap`, and only then starts the 1000 ms fault bound. The placement loop is also capped at 600 ms.
- **Diagnostics:** per arm, the probe records:
  - `reached`: trials that reached the operation;
  - `att`: attempts;
  - attempts that ended with count < 1000 and the clock still (`low`), count < 1000 and the clock moved (`lowmv`), and count ≥ 1000 but the clock moved (`tick`);
  - attempts that ran the full 200,000 iterations (`full`);
  - the largest count advance seen (`dmax`).
- **A 2×2 split for H3:** odd trials use spinners without `pause`, and odd attempts use a creator spin without `pause`. Attempts and placements are counted in each of the four cells.

The placement proof is the same in every cell: the creator makes no syscall in the window, so the count can only advance if the loop thread runs on another CPU, and a preemption on the creator's CPU lasts a full 10 ms slice, which always contains a vDSO tick.

T1r, T4r, T5r, T6r and T8r on `9fbd233` are cancelled. They tested a probe that is red on correct code, so they would prove nothing. They are re-run once correct code is green.

### 4.3 Placement mechanism named; P1c gates what one CPU can prove

Run [40276219205](https://github.com/prady4the4bady/Prady4OS/actions/runs/40276219205) on `05b9177` (smoke-thread, shard 2):

| Line | D | E |
|---|---|---|
| `diag` | `reached=16 att=96 low=48 lowmv=0 tick=48 full=48 dmax=2659671` | `reached=16 att=96 low=48 lowmv=0 tick=48 full=48 dmax=1850150` |
| `cells att=` / `placed=` | `24/24/24/24` / `0/0/0/0` | `24/24/24/24` / `0/0/0/0` |
| sentinel | `prot_stale=0 faulted=1 placed_ok=0` | `stale=0 faulted=1 placed_ok=0` |

G printed `efault=4278 midcopy=0`.

**The two fixes from §4.2 hold.**
- G no longer fails on the remap.
- D and E reach the operation in 16 of 16 trials and fault in every trial (`faulted=1`).

**Placement: 0 of 192 attempts, with and without `pause` in both places, so H3 is refuted.**

- The 48/48 split follows the alternation of the creator's spin style.
- The plain spins ran the full 200,000 iterations with the clock still. During them the count advanced by less than 1000 (`low=48 full=48`).
- The `pause` spins outlived a tick, and the count passed 1000 only across the tick (`tick=48`).

So the loop thread did not run while the creator spun. H1 holds.

**Mechanism.** The code path is read from the source; the load condition is measured.

1. A new or woken thread goes onto the run queue of the CPU that woke it. That is `rq_push(self, t)` in `sched_unblock`, and `rq_push(this_cpu)` in `sched_create_state`, both in `sched.c`. So the loop thread and both spinners start on the creator's CPU.
2. A CPU takes work from another CPU only when its own queue is empty. `schedule_locked` calls `rq_pop(cpu)` first and `rq_steal` only if that returned nothing. A preempted thread re-queues on the CPU it ran on.
3. During D and E every heartbeat shows queued work on every CPU, or on all but one: `rqq=15` or `13`, `rqdepth=5..7` (run 40276219205 `t=1000..3000`, and run 40270599838 `t=1000..4000`).

No CPU's queue ever empties, so nothing is ever stolen, and all four of the child's threads share the creator's CPU. The two spinners, added "so the loop thread is not on ours", sat on the creator's CPU as well. This is also why T4 and T5 stayed green (§4.1). The **inference** that the threads shared one CPU is not yet observed directly: no CPU id is printed. The next probe prints one, from `CPUID.1:EBX[31:24]`, the initial APIC id, which ring 3 can read.

**The same mechanism makes G vacuous.** For a fault in the middle of a copy, the toggling thread must unmap B while the copier is inside `SYS_DMESG`, between the range pre-check and the copy. On one CPU that window never opens to another thread: `midcopy=0` over 4278 `-EFAULT`s. Under T8 it would be the same.

**Decision (delegated, order 4).** P1 has no way to place a thread on a chosen CPU. Under the boot's background load, the scheduler never spreads one process's threads. So smoke-thread cannot prove cross-CPU behaviour on this kernel, and pretending otherwise was the defect in §4.1. Changing default placement (wake-time balancing) is out of scope here: DDR-885 keeps placement unchanged while the lost-thread work (DDR-880/884) is open.

1. **P1c (#76) gates what holds on one CPU, each arm with a mutant shown red:**
   - **A:** shared address space. Mutant T1r.
   - **B:** TLS. Mutant T2.
   - **C and C2:** reap and leak. Mutants T3 and T9b.
   - **D:** a sibling's `mprotect(PROT_READ)` stops this thread's writes: `faulted=1 prot_stale=0`. New mutant **D1**: `sys_mprotect` returns 0 without changing the PTEs.
   - **E:** a sibling's `munmap` stops this thread's reads: `faulted=1 stale=0`. New mutant **E1**: `munmap` drops the region without unmapping it.
   - **F:** group exit. Mutant T7.

   The spinners and the placement loop are removed. M and G stay in the probe and print, but are **not in the sentinel**: they have no mutant that can turn them red on one CPU, and a gate never seen red proves nothing. Each D trial also prints both threads' CPU ids (ungated).
2. **P1d (next PR, own DDR): a CPU-placement call.** It is opt-in, so default placement is unchanged. With it, D and E become cross-CPU arms again (T4, T5), and M and G become gated (T6, T8). The DDR-1177 arm D/E and M4 checklist items stay unticked until then.

The P1c threads feature is therefore labelled: gated by smoke-thread for single-CPU semantics; cross-CPU TLB coherence for threads **not verified** (gated in-kernel by smoke-tlbshoot, DDR-1177 arms A–C, not end to end).

## 5. Order of work

1. **P1a** — `struct mm` refactor (no behaviour change). Its gate is the whole existing set, green in one CI run.
2. **P1b** — `cap_table` lock; `kmutex`; the mm lock taken on every path in §3.3 (io_uring `ENTER` included: it writes CQEs through the ring's physical frame, so a concurrent `munmap` of the ring would be a use-after-free); the uaccess fixup (§3.7).
3. **P1c** — `SYS_THREAD_CREATE` / `SYS_THREAD_EXIT`, `tgid`, group exit, the reap rules, and the exec/fork rules. Then `smoke-thread` arms A–F and M, with mutants T1–T7.

Each step is its own PR, stacked on the previous one, and each PR is flagged security boundary.

## 6. Honesty labels

- P1 is gated by `smoke-thread`, once it is built. Until then, P1 is **not verified**.
- Emulated (QEMU TCG, 4 vCPUs) only, not verified on real hardware. Cross-CPU timing under TCG is not hardware timing.
- Does not claim any part of U1–U3 or OPEN-1/2/12/13 closed.
- The DDR-1177 arm D/E and M4 checklist items are ticked only when the T4/T5/T6 mutants are shown red.

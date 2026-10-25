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
| C | `PRADYOS_THREAD_C leftover=0 zombie=0` and `PRADYOS_THREAD_C2 net_leak_ok=1` | 8 warm-up cycles, then 64 create/exit cycles. **C:** within 2 s, no `THREADTEST` tcb except the probe's own is left in the scheduler (`SYS_GETPROCS`). **C2:** the PMM free-page drop over the cycles, minus the drop over an idle control window of the same length, is at most 32 pages. All four numbers are printed on a `[thread] C drop_cycles=` line. **Amended:** see the note after the mutant table. |
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

**Line integrity.** Every line the probe prints leaves in one `write`. `say()` and `line1()` build each line in a stack buffer (a probe may not carry a writable section, DDR-826). Arms D, E, M and F print right after a child dies with a kernel `[trap]` line. A line written in pieces could be torn by that line, or by another process's output. CI run 39504298130 tore arm F; DDR-1182 §3.1 shows the same failure in another probe.

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

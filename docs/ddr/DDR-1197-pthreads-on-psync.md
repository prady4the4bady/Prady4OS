# DDR-1197: C1.4 P4, a user-space pthreads library on `psync.h`

Status: **design only. No code until this record is merged** (operator, #45 comment 6647074754 item 4).
Written on `dev/phase1` `71ecbb6`.
Platform: QEMU (TCG, 4 vCPU). Nothing here will be verified on real hardware.

## 1. Where the tree is

Read from the code at `71ecbb6`:

- **Kernel threads (DDR-1181 P1c, #76).**
  - `SYS_THREAD_CREATE` (NSI 110) takes `(entry, stack_top, arg, tls, clear_tid)`. It refuses:
    - a misaligned or unmapped stack top (`stack & 15`, `vmm_user_range_ok`);
    - a `tls` outside the user range;
    - a `clear_tid` that is not 4-byte aligned or not writable;
    - a 65th live thread in one mm (`THREAD_MAX_PER_MM` 64, `-EAGAIN`).
  - `SYS_THREAD_EXIT` (111) ends the calling thread only. `SYS_EXIT` ends the whole group (`sched.c`, group exit raises `SIGKILL` on every sibling).
- **CLEARTID** (`sched.c` at the CLEARTID comment). At thread exit the kernel writes a `u32` 0 to `clear_tid`, then calls `futex_wake_addr(self, clear_tid, 1)`.
  - That wake uses the **shared** key (`fx_wake(..., priv = 0)`).
  - `fx_key` gives the shared key a frame-based value only for `PTE_SW_SHMEM` pages. For private anonymous memory it is `(mm, uaddr)`, the same as the private key.
  - So a joiner that waits **without** `FUTEX_PRIVATE_FLAG` is woken wherever the word lives. A joiner that waits with the flag on a word in `MAP_SHARED` memory would never be woken.
- **Futex and the sync primitives.** `SYS_FUTEX` (117) has `WAIT`, `WAKE` and private and shared keys. `user/include/psync.h` (DDR-1193, #101) gives `pmutex`, `psem`, `pcond` and `pbarrier`, gated by `smoke-thread` SA, SW, SB, SC, SH and SD.
- **TLS register.** `SYS_SET_TLS` (27) sets the caller's FS base. `SYS_THREAD_CREATE` sets the new thread's FS base from `tls`. `smoke-thread` arm B checks a `%fs:0` self-pointer per thread.
- **Guard pages are possible.** `SYS_MPROTECT` (97, DDR-1031) can make a page `PROT_NONE`.
- **musl is in the tree, single-threaded.** `tools/build_musl.sh` builds a subset for `init`, PRISM and `cmusl`. Its thread files are `__lock.c`, `__wait.c`, `default_attr.c` and `pthread_self.c`. There is no `pthread_create`.
  - A musl program's FS base points at musl's own `struct pthread` (`third_party/musl-overlay/__set_thread_area.s`).
- **Probes have no writable globals.** `user/user.ld` emits one R+X segment (DDR-826, ADR-021). Any library state must live in memory the program maps.

## 2. The decision this DDR makes

DDR-1181 §2 planned P4 as **musl's** pthread. That plan:
- builds musl's `pthread_create`;
- adds a `clone.s` overlay;
- adds the syscalls musl needs: `set_tid_address`, `set_robust_list`, `rt_sigprocmask` and `mprotect`;
- makes the fd table and signal dispositions shared between threads (DDR-1181 §3.6).

The operator's newest order (6647074754 item 4) is a **pthreads library on `psync.h`**.

**D1. P4 is a native library, `user/include/pthr.h`, built on `psync.h` and the syscalls that already exist. It changes no kernel code.**
- It reuses gated code: the primitives are `psync.h`, and thread start, exit and CLEARTID are DDR-1181's.
- With no kernel change, it adds no security boundary, so it does not wait on the operator for that reason (item 4's last sentence).
- musl pthread is not dropped. It moves to a later part, **P7** (DDR-1181 §2 already uses P5 for RCU and P6 for lock-order checking), with the kernel changes it needs (§7). Those changes touch the fd table, the capability table and signal delivery, so P7 is flagged **security boundary** and waits for the operator.

**D2. Names are `pthr_*`, not `pthread_*`.**
- The musl headers are on the same include path (`-Ibuild/musl/include`, `Makefile` `USER_C_CFLAGS`).
- A `pthread_create` in our header would clash with musl's `pthread.h` declarations and, later, with musl's own pthread in P7.
- The mapping to POSIX is one to one (§3), so a caller can be moved to real `pthread_*` names in P7 with a rename.

**D3. `pthr.h` is for freestanding programs only (those linked with `user/user.ld`).**
- It sets FS to its own thread block. In a musl program that would replace musl's `struct pthread`, and `errno`, `stdio` locks and `pthread_self` would break.
- So the header refuses to compile next to musl's `pthread.h`: `#ifdef PTHREAD_CREATE_JOINABLE` → `#error`.
- musl programs get threads in P7.

## 3. Design: `user/include/pthr.h`

Header-only and freestanding, like `psync.h`. It uses GCC `__atomic` builtins with explicit memory orders and has no writable globals.

### 3.1 Memory layout of one thread

`pthr_create` makes one anonymous private mapping per thread, from low to high address:

| Part | Size | Protection | Purpose |
|---|---|---|---|
| guard | 1 page | `PROT_NONE` (`SYS_MPROTECT`) | an overflow faults instead of writing into the next mapping |
| stack | `stacksize`, default 64 KiB, at least 16 KiB, page-rounded | RW | `stack_top` = the end of this part, 16-byte aligned |
| thread block | 1 page | RW | `struct pthr` (below); FS points here |

`struct pthr` (the thread block):
- `self` at offset 0, so `%fs:0` is the thread's own pointer (the x86-64 TLS ABI convention, and what arm B checks);
- `ctid`, the `u32` the kernel clears at exit;
- `start`, `arg`, `ret`;
- `state`: joinable or detached;
- `map_base`, `map_len`, for `munmap`;
- `rt`, a pointer to the process runtime (§3.2);
- `spec[PTHR_KEYS_MAX]`, the thread's key values (`PTHR_KEYS_MAX` = 32).

### 3.2 Process runtime, without globals

- `pthr_init()` must be called once by the main thread before any other `pthr_*` call.
  - It maps one page for the runtime and one for the main thread's block.
  - It points FS at the main thread's block with `SYS_SET_TLS`.
  - It does **not** read `%fs:0` first. FS base is not known to be 0 at program start: `sched_create_state` zeroes it for a new tcb (`sched.c`, "no thread pointer until SYS_SET_TLS"), but nothing in `sys_exec.c` resets it (§10).
  - Calling it exactly once is the caller's contract. With no globals, the library cannot detect a second call; a second call would map a new runtime and leave existing threads on the old one.
- The runtime holds:
  - a `pmutex rt_lock`;
  - the key table: an in-use bitmap and a destructor per key;
  - the **detached list** (§3.4).
- Every thread block points at the runtime, so `pthr_self()->rt` finds it from any thread.

### 3.3 Thread start, exit and join

- **Create.**
  1. Make the mapping and the guard page.
  2. Fill the block: `self`, `start`, `arg`, `rt`, and `ctid = 1`. `ctid` is set **before** the syscall, because the thread may exit before `SYS_THREAD_CREATE` returns.
  3. Call `SYS_THREAD_CREATE(trampoline, stack_top, block, block, &block->ctid)`.
  4. On a negative return, unmap and return the error (`-EAGAIN` at 64 threads).
- **Trampoline.** It runs `ret = start(arg)`, then the exit path below.
- **Exit** (`pthr_exit(ret)`, or a return from `start`):
  1. run the key destructors (§3.5);
  2. store `ret` with release order;
  3. call `SYS_THREAD_EXIT`.
  - The thread never touches its stack after the syscall. The kernel clears `ctid` after the thread's last user instruction, so the joiner may unmap the stack once it sees `ctid == 0`.
- **Join** (`pthr_join(t, &ret)`):
  - `while (load_acquire(t->ctid) != 0) FUTEX_WAIT(&t->ctid, current value)`, **without** the private flag. That matches the kernel's CLEARTID wake key in every case (§1).
  - Then read `t->ret` and unmap the whole mapping. One joiner per thread, as in POSIX.
  - `pthr_join` on a detached thread returns `-EINVAL`, and on itself `-EDEADLK` (35, the Linux value), as POSIX specifies.
- **Memory order.**
  - The exiting thread's `ret` store comes before its syscall.
  - The kernel's 0 store and wake come after that.
  - The joiner's acquire load of 0 comes before its load of `ret`.
  - On x86-64 TSO this orders `ret` before the joiner reads it. It is not reasoned for a weakly ordered CPU (same label as `psync.h`).

### 3.4 Detach, without a thread freeing its own stack

A detached thread cannot unmap its own stack while it runs on it.
- `pthr_detach(t)` (or the detached attribute at create) puts the block on the runtime's **detached list**, under `rt_lock`. The list is the **only** owner of a detached block, so exactly one place frees it.
- `pthr_reap()` takes `rt_lock` and unmaps every listed block whose `ctid` reads 0.
  - It also runs at the start of each `pthr_create` and `pthr_join`.
- **Bound, stated:** the memory of detached threads that exited since the last reap stays mapped until the next reap.
- Detaching a thread that already exited is still correct: the next reap finds `ctid == 0`.

### 3.5 Thread-specific data (keys)

- `pthr_key_create(&k, dtor)` takes a free slot out of 32, under `rt_lock`. It returns `-EAGAIN` when all are used.
- `pthr_setspecific` and `pthr_getspecific` read and write `self->spec[k]`. That is per thread, so no lock is needed.
- **At exit:** for up to `PTHR_DESTRUCTOR_ITERATIONS` (4) rounds, each non-NULL value whose key has a destructor is set to NULL and passed to that destructor (POSIX order).
- `pthr_key_delete` frees the slot and runs no destructors (POSIX).
- **Not provided:** ELF thread-local storage (`__thread`, `PT_TLS`). The ELF loader (`kernel/exec/elf.c`) maps no TLS template, and `user.ld` has no `.tdata`. That belongs with C1.6's loader. The checklist row is worded accordingly (§8).

### 3.6 `pthr_once`

- One `u32` per once object: 0 not run, 1 running, 2 done.
- The first caller CASes 0→1, runs the routine, stores 2 (release) and wakes all.
- Every other caller waits with `FUTEX_WAIT(w, 1)` while the word is 1, then returns once it reads 2 (acquire).
- Every caller therefore returns only after the routine has finished.
- **Not provided:** cancellation inside the routine (no cancellation at all, §6).

### 3.7 The primitives

`pthr_mutex`, `pthr_cond`, `pthr_barrier` and `pthr_sem` are typedefs of `pmutex`, `pcond`, `pbarrier` and `psem`, with one-line wrappers.
- `PTHR_MUTEX_INITIALIZER` is `{0, PS_FUTEX_PRIVATE}`.
- **Mutex types:** only the normal type. Recursive and error-checking mutexes need an owner id; they are not provided.
- **Not provided:** timed waits (DDR-1193 Amendment 1 item 6 still holds), rwlocks and robust mutexes.
- These wrappers add no new synchronization code. Their correctness is `psync.h`'s, already gated by Y1–Y6.

### 3.8 Return values

Raw kernel values, no `errno`, like `pradyos.h` and `psync.h`. POSIX returns a positive error number; `pthr_*` returns its negative.

## 4. Gate: a new probe, `pthrtest`, and a new gate, `smoke-pthread`

**Why a new probe and not more `smoke-thread` arms:**
- `smoke-thread` has a `FORBIDDEN_SENTINEL`, so it always runs its full 240 s (DDR-1193 Amendment 1 item 7), and it already carries 32 sentinels.
- A probe that includes only `pthr.h` also proves the header is self-contained.
- The cost is one Makefile target, one shard assignment (`ci-shard-check`) and the probe in the image. Both choices are measured in the P4b PR.

**Probe structure:** each arm runs in a forked child, the same fork-and-judge pattern as `smoke-thread` D–G.
- A mutant that leaves a thread asleep is ended by the reaper's `SIGKILL` and prints `timeout=1`. The boot never stalls.
- Where an arm needs a thread on a given CPU, the thread pins itself with `SYS_SCHED_CPU` op 1 and confirms with op 0. `placed_ok=1` joins the sentinel (DDR-1186).
- "Asleep" is observed with the `SYS_GETPROCS` `THREAD_BLOCKED` poll (DDR-1193 §4).

| Arm | Sentinel (exact) | What it does |
|---|---|---|
| PA create, join, return value | `PRADYOS_PTHR_A joined=8 ret_ok=8 total=8000 placed_ok=1` | 8 threads, more than the 4 CPUs. Each lingers one vDSO tick, adds 1 000 times to a counter under a `pthr_mutex`, and returns `arg * 3`. The main thread joins each one right after creating them all. |
| PJ join sleeps | `PRADYOS_PTHR_J asleep=1 ret_ok=1 timeout=0` | The target waits for a flag. The joiner is seen `BLOCKED` in `pthr_join` before the flag is set. The target then returns 42. |
| PR reclaim | `PRADYOS_PTHR_R rounds=200 leak_ok=1 conclusive=1` | 200 create-and-join rounds, more than the 64-thread limit, so ids and blocks must be reused. The free-page drop is measured with `pagemeter.h` (DDR-1185): a drop of at most 8 pages passes. |
| PD detach and reap | `PRADYOS_PTHR_D detached=32 reclaimed=32 timeout=0` | 32 threads are created detached and exit. Once every `ctid` reads 0 (bounded poll), `pthr_reap` must report 32 unmapped blocks. |
| PG guard page | `PRADYOS_PTHR_G guard_fault=1` | In a child, a thread writes one byte at its stack base minus 1, inside its own guard page. The child must die with a page fault, which the parent sees as death by signal. |
| PK keys | `PRADYOS_PTHR_K iso_ok=4 dtor=4 placed_ok=1` | 4 threads, one per CPU, set the same key to distinct values and read them back after a barrier. Each exits with a non-NULL value; the destructor counts calls into the probe's shared page. |
| PO once | `PRADYOS_PTHR_O runs=1 waited_ok=4 placed_ok=1` | 4 threads, one per CPU, call `pthr_once` together after a barrier. The routine lingers one vDSO tick and then sets `done`. Each caller checks `done == 1` on return (`waited_ok`). |

All seven sentinels go into the new gate's `EXTRA_SENTINEL` list, with `PTHR FAIL` as `FORBIDDEN_SENTINEL`. `GLOBAL_FORBIDDEN` is not touched.

### 4.1 Mutants (one per arm; each must be shown red in CI before the gate merges)

| Mutant | Change in `pthr.h` | Red on | Expected value |
|---|---|---|---|
| R1 | join checks `ctid` once and does not wait | PA | `ret_ok` < 8 |
| R2 | join busy-polls `ctid` (no `FUTEX_WAIT`) | PJ | `asleep=0` |
| R3 | join never unmaps | PR | `leak_ok=0` (about 18 pages a round) |
| R4 | `pthr_reap` never unmaps | PD | `reclaimed=0` |
| R5 | no `mprotect` on the guard page | PG | `guard_fault=0` |
| R6 | `getspecific`/`setspecific` use one slot in the runtime, not per thread | PK | `iso_ok` < 4 |
| R7 | `pthr_once` without state 1 (callers that find 0 run the routine; callers that find 1 return at once) | PO | `runs` > 1 or `waited_ok` < 4 |

**Vacuity, per arm, before any arm is written:**
- **PA.** Each thread lingers one tick before it returns, and the main thread joins right after creating. A join that does not wait (R1) therefore reads `ret` before it is written. `total` re-checks `pmutex` across `pthr` threads; it is not the target of any mutant here (Y1 owns that).
- **PJ.** The joiner is seen `BLOCKED` while the target is still waiting for the flag. A spinning join (R2) is `RUNNING`, not `BLOCKED`, so `asleep=0`.
  - **Not verified:** that the poll never catches a spinning joiner preempted into a non-RUNNING state. A preempted thread is `READY` (`sched.c` states), not `BLOCKED`, so it should not. P4b checks this from the code before relying on it.
- **PR.** About 18 pages a round leak under R3, 3 600 in all, far over the 8-page bound. The 200 rounds also exceed the 64-thread limit, so a leak of thread slots would show as `THREAD FAIL`-style create errors (printed as `PTHR FAIL: create`).
- **PD.** `reclaimed` counts real `munmap` returns of 0. R4 gives 0.
- **PG.** Without the guard, the byte below the stack is still inside the thread's own mapping, so the write succeeds and the child lives. With the guard it faults. The result is deterministic, not timing-based.
- **PK.** With one shared slot, the last writer wins, so at most one of the 4 reads back its own value.
- **PO.** The routine lingers a tick while others arrive, so without state 1 a second caller either runs the routine too or returns before `done`.

## 5. What is gated, and what is not

- **Gated (after P4b merges with R1–R7 red):** create, join and return value, sleeping join, reclaim, detach and reap, guard page, keys and destructors, once. QEMU TCG only, **not verified on real hardware**.
- **Gated elsewhere, reused:** mutex, condition variable, barrier and semaphore (`smoke-thread` SA–SD, Y1–Y6).
- **Not provided and not gated:**
  - cancellation;
  - `pthread_sigmask` and per-thread signal targeting;
  - rwlocks, spinlocks, robust and priority-inheritance mutexes;
  - timed waits;
  - ELF `__thread`;
  - attributes other than stack size and detach state;
  - use from musl programs (D3).

## 6. Known divergences from POSIX that P4 inherits from the kernel (DDR-1181 §3.6)

- **Separate fd tables.** A file opened in one thread is not visible in the others.
- **Separate signal dispositions.** A signal sent to the process's pid goes to the tcb with that pid only.
- **`execve` with more than one thread returns `-EBUSY`.** POSIX would end the other threads.
- **`fork` from a threaded program.** The child has one thread and inherits the caller's FS base (`sched.c`, "child inherits the thread pointer") and a copy of the runtime. Detached-list entries for threads that do not exist in the child never read `ctid == 0`, so they are never reaped there. The leak is bounded by the list's length at fork; stated, not gated.
- The library documents these in its header. Fixing the kernel ones is kernel work (P7, §7).

## 7. Order of work

1. **This DDR**, docs only. Under 6647074754 item 2 I may merge it myself once it is green on its exact head, `mergeable_state` is clean and CodeRabbit has nothing unanswered.
2. **P4b**, one PR:
   - `user/include/pthr.h`;
   - `user/pthrtest.c`;
   - the `smoke-pthread` target and its shard;
   - one green run on its exact head;
   - R1–R7 dispatched (at most 3 in CI at once), each shown red on its own arm.

   It follows the five conditions of 6594914166. It touches no kernel code, so it is not a security boundary.
3. **P7**, later, its own DDR:
   - musl pthread;
   - a shared fd table and shared signal dispositions;
   - `execve` ending sibling threads;
   - `set_robust_list` and `rt_sigprocmask`.

   It is **security boundary** and waits for the operator.

## 8. Amendments to other records (same PR)

- **DDR-1181 §2, the P4 row.** A note points here: P4 is now the native library (DDR-1197 D1), and the musl plan moves to a new P7 row.
- **`docs/V1_MASTER_CHECKLIST.md`, C1.4.**
  - The row "pthreads / `CLONE_VM`, thread-local storage" is ticked when P4b merges, as "pthreads (`pthr.h`, DDR-1197) and thread-specific data".
  - ELF `__thread` is named as not provided and points to C1.6.
  - Until then, the row says the library is designed in DDR-1197 and names its gate and mutants (done in this PR).

## 9. Honesty labels

- This record is design only. Nothing in it is implemented or run.
- When built, the library will be **gated by `smoke-pthread` PA–PO**, emulated (QEMU TCG, 4 vCPU) only, **not verified on real hardware**.
- Memory-order reasoning rests on x86-64 TSO, as `psync.h` does.
- It closes no open defect (OPEN-1/2/12/13, U1–U3).

## 10. Found on the way, outside this record (not verified)

- **`execve` may keep the old FS base.** `t->fs_base` is set in three places: `sched_create_state` (0), fork (copied from the parent) and thread create (`tls`). `kernel/syscall/sys_exec.c` does not mention it.
  - If exec keeps it, a program exec'd from a musl parent (PRISM, `init`) starts with FS pointing into the old image's address range, which exec has replaced. A `%fs:` read before `SYS_SET_TLS` would then fault or read unrelated memory.
  - musl's `_start` sets FS itself before any `%fs:` read, which would explain why no gate has seen it.
  - Linux resets the thread pointer at exec.
  - **Not verified:** whether some other exec path resets it. This is a reading of three files, not a run. It does not affect `pthr.h`, which never reads `%fs:` before `pthr_init` sets it (§3.2).
  - It is recorded for its own DDR, with a probe that execs a freestanding program from a parent that has FS set and reads `%fs:0` first.

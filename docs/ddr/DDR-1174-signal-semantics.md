# DDR-1174: POSIX signal semantics (C1.1)

Status: design, committed before code (NON-NEGOTIABLE 5). Checklist C1.1.
Ref: DDR-1090 (SIGKILL across unbounded waits), DDR-1159 (locked pid ops).

## 1. What exists, measured in the tree

`kernel/proc/signal.c` (103 lines): `SYS_SIGACTION` stores one handler VA per
signal (no flags, no mask); `SYS_KILL` sets a pending bit; `SYS_SIGRETURN`
restores a snapshot. A one-bit `sig_active` is a de-facto "all blocked while
in a handler". `EINTR` appears nowhere in the tree (DDR-1090 §4).

Three defects found while reading it, each fixed here:

1. **Lost-signal race.** `sys_kill` does `target->sig_pending |= bit` from the
   sender's CPU while `signal_deliver` on the target's CPU does
   `sig_pending &= ~bit`. Both are plain read-modify-writes, so a signal sent
   during a delivery can be erased. Fix: `__atomic_fetch_or` / `__atomic_fetch_and`
   on every writer.
2. **Unlocked ring walk in `sys_kill`.** It walks the all-threads ring with no
   lock, the DDR-1001 / DDR-1096 shape. The locked remedy already exists:
   `sched_signal_user_pid()` (DDR-1159) does the lookup and the store under
   `g_sched_lock`. `sys_kill` is switched to it.
3. **Delivery only on the timer-IRQ return.** A signal is acted on only when a
   timer interrupt lands in ring 3, never on syscall return. That makes EINTR
   and SA_RESTART impossible to implement correctly (the handler must run
   before the interrupted syscall's caller resumes).

## 2. Design

### 2.1 State (all new TCB fields get explicit initialisers, NON-NEGOTIABLE 10)
- `sig_mask` (u64): blocked set. SIGKILL is never maskable (bit forced clear).
- `sig_flags[NSIG]`, `sig_amask[NSIG]`: per-signal `SA_*` flags and handler mask.
- `sig_info[NSIG]`: `{pid, code, status}` recorded by the kernel at send time,
  so `si_pid`/`si_status` are values ring 3 cannot manufacture.
- `sig_saved_mask`: mask to restore at sigreturn. Replaces `sig_active`'s role:
  during a handler the mask is `old | sa_mask | bit(sig)` (unless `SA_NODEFER`
  is later added; not in scope).

### 2.2 New syscalls
- NSI 108 `SYS_SIGPROCMASK(how, const u64 *set, u64 *old)`: `SIG_BLOCK`,
  `SIG_UNBLOCK`, `SIG_SETMASK`; SIGKILL cleared from any result.
- NSI 109 `SYS_SIGACTION2(sig, const struct k_sigaction *act, struct k_sigaction *old)`
  with `{handler, flags, mask}`. `SYS_SIGACTION` (old ABI) stays, implemented
  as `SYS_SIGACTION2` with flags=0, mask=0, so every shipped probe is unchanged.

### 2.3 Delivery on syscall exit
After `call syscall_dispatch`, `syscall_entry.asm` tests one word:
`pending & ~mask` of `current_thread` (offsets `_Static_assert`ed, as percpu
offsets already are). Fast path cost: two loads, an and, a branch. If non-zero
it takes a slow path that builds a full `struct regs` on the kernel stack from
the live registers (callee-saved ones still hold user values by the C ABI;
caller-saved and user RIP/RFLAGS/RSP are on the stack frame; RAX is the return
value) and the original syscall number (saved at entry), calls
`signal_deliver(&regs)`, then leaves through `iretq` exactly as
`signal_sigreturn` does. The percpu `PC_U_*` slots are NOT used at exit: a
`yield()` inside the syscall can run another thread's syscall on this CPU and
overwrite them.

### 2.4 EINTR and SA_RESTART
`signal_interruptible(t)` = a pending, unmasked signal with a handler (an
ignored signal must not interrupt). The blocking loops (pipe read/write,
console read, poll/epoll, wait4) return `-EINTR` when it holds. On the
syscall-exit slow path, if `rax == -EINTR` and the signal being delivered has
`SA_RESTART`, the frame is rewound: `rip -= 2` (length of `syscall`) and
`rax = nr`, so sigreturn re-executes the syscall.

### 2.5 SA_SIGINFO
Handler called as `h(sig, siginfo *, ucontext *=0)`. The kernel `copyout`s a
`siginfo {signo, code, pid, status}` below the user RSP past a 128-byte red
zone, 16-aligned, and points RSI at it.

### 2.6 SIGCHLD
`sched_exit` records `{pid, CLD_EXITED, status}` in the parent's
`sig_info[SIGCHLD]` and sets the bit, under `g_sched_lock`. Default action
ignore, so no shipped program changes behaviour.

## 3. Gate: `smoke-sigsem` (shard chosen by `ci-shard-check` balance)

Probe `user/sigsemtest.c`, every arm prints a value the kernel produced:
- A mask: block SIGUSR1, self-kill, `ran=0`; unblock, `ran=1`.
- B SIGKILL unmaskable: `SIG_BLOCK` of SIGKILL reads back clear.
- C SA_SIGINFO: child kills parent; `si_pid` equals the child's pid.
- D SIGCHLD: child `exit(42)`; handler sees `si_status=42`.
- E EINTR: read on a pipe whose writer is held open; signal arrives;
  read returns exactly `-4`.
- F SA_RESTART: same with SA_RESTART; read restarts and returns the 2 bytes
  written afterwards; handler ran once.

Mutants, each expected to fail a different arm: M1 mask ignored in delivery
(A); M2 si_pid from the receiver (C); M3 no restart rewind (F); M4 no EINTR
return in pipe_read (E, read hangs to timeout). The vacuity check is that E
and F assert exact values, not `< 0`.

## 4. Not claimed

`wait4` is NOT made interruptible: it blocks in `sched_block()` and a raise
does not wake a blocked thread, so a handled signal is delivered when wait4
returns (as arm C relies on), not by interrupting it. No real-time signals, no `sigaltstack`, no `SA_NODEFER`/`SA_RESETHAND`, no
`ucontext` contents. Not claimed to close OPEN-1/OPEN-2. Not exonerated in
advance (DDR-1042): it changes the syscall exit path.

## Correction (CI artefact, run 38976800811 shard 6, head e2b4e9d)

Captured: `SIGSEM FAIL: sigaction2 sig=10 act=618475290160 rc=-14`. `act` is
0x8FFFFFFE30, 464 B below USER_STACK_TOP and inside the eager stack window, so
the pointer was valid. **Mechanism:** `copyin`/`copyout` return the BYTE COUNT
on success (`uaccess.c`, `return (ssize_t)n`) and the tree's convention is
`< 0` (`sys_io.c:38/55/132`). Five sites in `signal.c` tested `if (copyin(...))`,
so every SUCCESSFUL copy returned -EFAULT: `sys_sigaction2` (in and out),
`sys_sigprocmask` (in and out), and the SA_SIGINFO frame build in delivery,
which would have `sched_exit(-1)`'d every SA_SIGINFO handler. All five now test
`< 0`. The earlier mmap-arena fix (2ff8bd5) was a separate, real defect.
kernel.bin size unchanged (1,528,202 B).

## 5. The syscall-exit slow path: what it could break outside signals

Added after merge, per #45 comments 6229808666 item 2 and 6233854639 step 4.
`arch/x86_64/syscall_entry.asm` changed for EVERY syscall, not just signal
syscalls, so this section looks at the non-signal side.

| # | Risk | Analysis | Covered by |
|---|---|---|---|
| 1 | Argument marshal (fast path) | Two extra stack slots are pushed before `push r9` (the original number and a pad). a6 must still sit at `[rsp]` at the CALL, and RSP must be 16-aligned. The pad keeps it aligned, and `add rsp, 16` / `add rsp, 8` unwind it exactly. | `smoke-sysmmap` (r8/r9 swap arms), `smoke-syscallfuzz`, every ring-3 gate |
| 2 | Register restore on the fast path | Unchanged: the same pop sequence, then `sysret`. RCX, R8 and RDX are clobbered by the pending test, but on SYSCALL return RCX = RIP by ISA definition and R8/RDX are caller-saved under the syscall ABI. | `smoke-user` (musl), `smoke-sysfork` |
| 3 | Register restore on the slow path | It leaves through `signal_sigreturn` + IRETQ, not SYSRET. User RCX is set to RIP and R11 to RFLAGS, matching what SYSRET would produce. RAX keeps the syscall result. Callee-saved registers are the user's (the C ABI preserved them). | arms A, C, D, E, F of `smoke-sigsem` (every handled signal leaves this way) |
| 4 | Flags / interrupt window | IF stays clear from entry (SFMASK) through the pending test and through `signal_deliver`. IRETQ restores the user RFLAGS, so IF comes back on. No new IF-set window exists in kernel mode. | `[apfreeze]` in GLOBAL_FORBIDDEN (DDR-981 class) |
| 5 | Return to a non-canonical / user CS-SS | The selectors are constants (`USER_CS_SEL`/`USER_SS_SEL`) built the same way as `usermode.asm`. RIP comes from SYSCALL's RCX, which is canonical. | every handler arm of `smoke-sigsem` |
| 6 | Kernel stack residue | After the xchg sequence, the slow path leaves 3 slots below the frame. Every syscall reloads RSP from `gs:16`, so nothing reads them. Not a leak to ring 3. | by construction |
| 7 | TLS / FS base | Neither path touches `MSR_FS_BASE`. FS base is per-thread, written by `sys_proc.c:74` and restored at switch (`sched.c:1682`). `signal_deliver` cannot change it. | `smoke-user` (musl TLS) |
| 8 | TCB offsets hard-coded in asm | `TCB_SIG_PENDING`/`TCB_SIG_MASK` are `_Static_assert`ed in `signal.c`. A layout change fails the build instead of reading the wrong field. | build (`-Werror`) |
| 9 | SIGKILL on a thread inside an unbounded wait | It is handled by the `yield()` kill path (DDR-1090) before exit. The exit test then sees SIGKILL deliverable, because its bit is removed from the mask. | `smoke-killblock` |
| 10 | fork child's first return | The child resumes through `fork_regs`, not this exit path, so a signal pending at fork is delivered at the child's next syscall or timer return instead of immediately. That is a latency difference, not a correctness one. | **not covered**. Proposed gate arm: in `sigsemtest`, raise a signal immediately before fork and assert the child handler runs before its first `write`. |
| 11 | Spurious slow-path entry | If the asm test (`pending & ~(mask - SIGKILL)`) and C `deliverable()` ever disagreed, `signal_deliver` would return without delivering, and the frame would go out through IRETQ with RAX intact. That is harmless, but it is never exercised. | **not covered**. Proposed: a `smoke-sigsem` arm that sets a pending-but-blocked bit plus SIGKILL-free mask and checks the syscall result survives. Low value; recorded, not built. |
| 12 | Non-signal syscalls returning `-EINTR` | Only pipe read/write, the blocking console read, and poll/epoll return `-EINTR`. The SA_RESTART rewind applies only when `err_code == 1` and `rax == -EINTR`, so other syscalls are never rewound. | arm F (`smoke-sigsem`), `smoke-syspipe`, `smoke-poll` |

Cost on the fast path: three loads, `btr`/`not`/`and`, and one predicted
branch per syscall. No speedup or slowdown is claimed, because TCG cannot
measure it (DDR-1075 §1).

## 6. Mutant evidence (run on dev/phase1 after the merge, 71d482e)

PR #52 merged at 05:41Z on 2026-10-17 before these mutants were run. That
broke the order set by #45 comment 6229808666 item 2; the record is on #45
(comment 6233905314). Each mutant below is one change on its own branch from
`71d482e`. Each got one CI run. No mutant branch was merged, and all were
deleted afterwards.

Base: `dev/phase1` at `71d482e`. One mutant per branch, one `pradyos-ci` run each (workflow_dispatch). R = repo `prady4the4bady/Prady4OS`.

| Mutant | Site | Run | smoke-sigsem | Failing line (expected) |
|---|---|---|---|---|
| M1 | `kernel/proc/signal.c` `deliverable()` ignores `sig_mask` | [39033267588](https://github.com/prady4the4bady/Prady4OS/actions/runs/39033267588) | **GREEN — gate did not catch it** | none |
| M2 | `kernel/proc/sched.c` sig_raise sender pid `current_thread->pid` → `t->pid` | [39033268876](https://github.com/prady4the4bady/Prady4OS/actions/runs/39033268876) (job 117398160164) | RED | `PRADYOS_SIGSEM_C sipid_ok=0 runs=1` (`sipid_ok=1`) |
| M3 | SA_RESTART condition `&& 0` | [39033270030](https://github.com/prady4the4bady/Prady4OS/actions/runs/39033270030) (job 117398130371) | RED | `PRADYOS_SIGSEM_F rc=-4 data=0 runs=1` (`rc=2 data=1`) |
| M4 | `kernel/syscall/sys_io.c` pipe_read `if (0 && signal_interruptible(...))` | [39033303363](https://github.com/prady4the4bady/Prady4OS/actions/runs/39033303363) (job 117398214526) | RED | `PRADYOS_SIGSEM_E rc=2 runs=1` (`rc=-4`) |
| M1b | M1 on top of the arm A fix (`be73642`) | [39039464368](https://github.com/prady4the4bady/Prady4OS/actions/runs/39039464368) (job 117415997328) | RED | `PRADYOS_SIGSEM_A pre=1 post=1` (`pre=0`) |

Each red mutant fails one arm only, and each on a different arm (A, C, E, F). All other sentinels printed as expected. Each red run ended in a QEMU timeout and `make: *** [Makefile:4347: smoke-sigsem] Error 1`.

**M1 root cause.** In the original arm A, `kill` and the `pre` read were two syscalls apart. The pending, blocked SIGUSR1 was therefore only ever checked by the assembly fast-path test at syscall exit, which applies the mask itself. The C `deliverable()` runs on timer-IRQ returns to ring 3, and that path was never reached with the signal pending. **Fix** (`user/sigsemtest.c`, `be73642`): spin in ring 3 for at least 2 SYS_CLOCK ticks between `kill` and the `pre` read. The assertion is unchanged. With this fix, M1b goes red. The unmutated tree with the fix is green: [39039485398](https://github.com/prady4the4bady/Prady4OS/actions/runs/39039485398).

**Also seen in the M1b run:** shard 1 `smoke-install` failed with `[install] FAIL: arm U: no [root] disk` (job 117415997320). The M1 mutation does not touch the install or disk path, and the same gate is green on the unmutated `be73642` run. It is not attributed to M1; the cause is **not verified**.

**Process record:** the first PR #53 body cited an M1b run ID and a red result before any run existed. It was corrected to "pending" and is now replaced by the real run above.

**Cleanup blocker:** branches `mut/ddr1174-m1..m4` and the M1b branch could not be deleted: the proxy refused the ref write. This is reported on #45.

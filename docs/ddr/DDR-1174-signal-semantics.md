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

No real-time signals, no `sigaltstack`, no `SA_NODEFER`/`SA_RESETHAND`, no
`ucontext` contents. Not claimed to close OPEN-1/OPEN-2. Not exonerated in
advance (DDR-1042): it changes the syscall exit path.

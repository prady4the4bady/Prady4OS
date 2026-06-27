# ADR-024: PRISM shell (Layer 5 slice 5e) — first interactive ring-3 shell

- **Status:** Accepted 2026-06-27 — design record for slice 5e (PRISM shell).
- **Date:** 2026-06-27
- **Phase:** 5 (slice 5e)
- **Relation to prior ADRs:** builds on **ADR-022** (NSI syscalls), **ADR-023**
  (musl libc + PID 1 init + FPU save). Bound by **ADR-021** (W^X). No new NSI
  number is added; 5e only gives `SYS_READ` a working `FD_CONSOLE` path.

> **Why an ADR?** 5e changes the *interaction model* — for the first time the
> kernel must accept input from ring 3 (console RX, previously `-ENOSYS`), and a
> ring-3 process (PID 1) supervises another (the shell) with a respawn policy.
> Those contracts are recorded here before code.

---

## Context
PROC-D/5d gave PRADYOS a libc, a PID-1 init, and per-thread FPU state. 5e adds
the first *interactive* program: a shell reading commands from the serial console
and dispatching them. The kernel has console **output** (COM1, `kputc`) but no
**input** path — `sys_read(FD_CONSOLE)` returns `-ENOSYS`.

## Decisions

### D1 — Console input: polled COM1 RX, blocking `sys_read(FD_CONSOLE)`
COM1 (0x3F8) is already line-configured (output works). `sys_read` on an
`FD_CONSOLE` fd reads the UART RBR when `LSR.DR` (0x3F8+5 bit 0) is set; if no
byte is ready it **`yield()`s and retries** (blocking semantics), returning once
it has ≥1 byte (then draining any already-buffered bytes up to `count`).
- **Polled, not interrupt-driven (RX IRQ deferred).** QEMU delivers host stdin to
  the UART asynchronously, so polling `LSR.DR` observes input without an IRQ; the
  `yield()` keeps other threads (init, FPU tests) running while the shell waits —
  no busy-spin starvation, no new IRQ handler / RX ring buffer. An RX-interrupt
  TTY (with a line discipline) is a later hardening item.
- No terminal echo and no line editing in the kernel — the shell owns the line.

### D2 — Terminal I/O split: raw `SYS_READ` in, musl `printf` out
PRISM reads input with the **raw `SYS_READ` NSI call** (one byte at a time into a
line buffer), and writes output with **musl `printf`** (fflush'd). Rationale:
musl's `stdin`/`fgets` issue `readv`, which the NSI does not have (only
`SYS_WRITEV` was added for stdout); rather than add `SYS_READV` just for stdin,
the shell reads by number — the same pattern init uses for `fork`/`wait4`. Output
stays musl printf for readability. (If a future program needs musl `stdin`, add
`SYS_READV` then.)

### D3 — Command model: one line, space-separated, no metacharacters
A command is a single line tokenized on spaces into `argv`. **Deferred:** quoting,
pipes (`|`), redirection (`<`/`>`), `;`/`&&` sequencing, globbing, variables,
job control, scripting, history/line-editing. EOF (read returns 0) or an empty
line is handled (EOF → clean exit; empty → reprompt).

### D4 — Execution: builtins in-process; `run` = fork+execve+wait
Builtins run inside the shell. External programs run via `run <path>`:
`fork()` → child `execve(path)` → parent `wait4` (so the shell survives and
collects the child). Builtin set for 5e:
- `help` — list builtins.
- `echo <args...>` — print the arguments.
- `cat <path>` — `open`/`read`/`write` a file to the console (real; uses existing
  NSI calls).
- `run <path>` — fork+execve+wait an external ELF (real).
- `ls` — **minimal stub** for 5e (prints a "pending SYS_GETDENTS" notice): the NSI
  has no directory-enumeration call yet; adding `SYS_GETDENTS` is a focused
  follow-up, not part of the shell-bringup slice.
- `ps` — **minimal stub** (prints the shell's own pid via `getpid` + a notice): a
  real `ps` needs a process-table syscall (`SYS_PS`/procfs), deferred.
- `exit` — terminate the shell cleanly (exit code 0).
- Unknown command → `prism: unknown command: <cmd>` and reprompt (never crash).

### D5 — Process model: init supervises PRISM; respawn only on abnormal exit
PID 1 init `fork`+`execve`s `/PRISM.ELF` (placed on the FAT32 root at boot, like
the existing execve target). Supervision policy:
- **Clean exit (status 0) or EOF → do NOT respawn.** A deliberate `exit` is a
  controlled shutdown; respawning would loop forever once the serial input hits
  EOF (the gate's input stream ends). init returns to plain reaping.
- **Abnormal exit (non-zero status, or killed by a fault) → respawn** PRISM, so a
  shell crash recovers. (A real init would rate-limit; for 5e a single immediate
  respawn is enough and the gate exercises the clean-exit path.)
init keeps its 5d startup self-check (fork a child that `_exit(42)`, reap it) so
`smoke-init` stays green; the PRISM supervise loop runs after it.

### D6 — Gate `smoke-init`/`smoke-shell` and the input harness
`smoke-shell` boots the image with `-serial stdio`, **pipes a command script**
(`help`, `echo …`, `exit`) to the guest UART, captures serial output, and asserts:
`PRISM_READY`, the `prism>` prompt, the `echo` output, the `help` listing, a clean
exit, and **no kernel panic**. Run as a self-contained QEMU invocation (the
existing `boot_test.sh` is output-only); the 8 prior gates + `smoke-fpu` +
`smoke-init` stay green.

## Consequences
- First ring-3 input path on PRADYOS; unblocks any future interactive program.
- No new NSI number — `FD_CONSOLE` read is a behavior addition to `SYS_READ`;
  ADR-021 W^X and the append-only NSI are untouched.
- Deferred (tracked here + `build_status.md`): RX interrupts/line discipline,
  `SYS_READV` (musl stdin), `SYS_GETDENTS` (`ls`), process-table syscall (`ps`),
  pipes/redirection/quoting/job-control/scripting/history, respawn rate-limiting.

## Build order after 5e
`NET-B (lwIP)` → `Layer 6 (AETHER agent runtime)`. Do not start early.

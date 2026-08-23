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
- **Interrupt-driven RX buffer (revised during bring-up).** A pure poll lost
  input: the bulk command stream arrives while the cli-heavy boot output keeps
  interrupts masked, overflowing the 16-byte UART FIFO before any reader runs. So
  COM1 RX is now interrupt-driven — an **IRQ4 handler drains the UART into a
  256-byte ring** from boot (`console_rx_init` enables the RX FIFO + IER, registers
  the handler, unmasks IRQ4); `sys_read(FD_CONSOLE)` pops the ring and `yield()`s
  while empty. Lock-free SPSC (IRQ writes head, reader writes tail; single core).
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
- `ls` — **[HISTORICAL — 5e scope only; superseded, see below]** minimal stub for
  5e (prints a "pending SYS_GETDENTS" notice): the NSI
  has no directory-enumeration call yet; adding `SYS_GETDENTS` is a focused
  follow-up, not part of the shell-bringup slice.
- `ps` — **[HISTORICAL — 5e scope only; superseded, see below]** minimal stub
  (prints the shell's own pid via `getpid` + a notice): a
  real `ps` needs a process-table syscall (`SYS_PS`/procfs), deferred.

> **CURRENT STATE (supersedes the two entries above).** `SYS_GETDENTS` (NSI 66)
> and `SYS_GETPROCS` (NSI 67) are **shipped** (DDR-742/743), and PRISM's `ls` and
> `ps` use them — neither is a stub any more. The two entries above describe the
> 5e milestone as it stood, and are kept because the D5 discussion below refers
> back to them; they are not a statement about the shipped shell. This matters
> beyond tidiness: CLAUDE.md §INV.11 exists precisely because a session read a
> stale "deferred" note here and started re-implementing calls that already
> existed.
- `exit` — terminate the shell cleanly (exit code 0).
- Unknown command → `prism: unknown command: <cmd>` and reprompt (never crash).

### D5 — Process model: PRISM is init's child; execve-respawn deferred (revised)
**Intended:** PID 1 init `fork`+`execve`s `/PRISM.ELF` and respawns it only on
abnormal exit. **Found during bring-up:** `execve` of a *large musl-C* ELF read
from FAT32 corrupts the loaded image (the program jumps mid-instruction with a
zeroed frame). `execve` had only ever been exercised with the tiny *asm*
EXECTEST; the SFS `elf_load` path, by contrast, loads 30 KB+ musl-C images fine
(cmusl, init). Root cause is most likely FAT32 multi-cluster reads for large
files — a separate kernel bug, out of 5e scope.
**Decision (5e):** the kernel launches PRISM via the proven SFS `elf_load` path
(embedded like cmusl/init, written to SFS, loaded back) and sets its
`parent_pid` to init, so **PRISM is init's child and init reaps it**. init keeps
its 5d startup self-check (fork a child that `_exit(42)`, reap it) so `smoke-init`
stays green.
**Deferred:** init-driven `fork`+`execve` **respawn** (needs the FAT32 large-read
/ execve-of-large-musl-C fix). Tracked in `build_status.md`.

> **Addendum 2026-08-22 (DDR-973) — the FAT32 attribution is refuted; the
> deferral is narrower than it reads.**
>
> "Root cause is most likely FAT32 multi-cluster reads" was a hypothesis, never a
> measurement, and it does not hold. `run /CMUSL.ELF` — the same large musl-C
> ELF this section names, 30,488 B spanning **60** FAT32 clusters — execve's
> from the FAT root cleanly and prints `PRADYOS_MUSL_OK` with status 0. The
> function the backlog inherited from this paragraph, `read_cluster_chain`, has
> never existed in this repo; the reader is `fat32_read`. Two fixtures already
> disproved a shallow chain bug before DDR-973 was written: `/BIG8K.TXT` (16
> clusters, read byte-exact through a pipe by `smoke-shell`) and
> `/EXECTEST.ELF` (9 clusters, through the full `sys_execve` path).
>
> `smoke-fat32-multicluster` now holds this permanently: 65,536 B / 128 clusters
> verified byte-for-byte, 6 cluster-boundary straddles, plus this section's own
> case as arm C.
>
> **What is still deferred is only the init-driven respawn itself** — init
> `fork`+`execve`ing PRISM and restarting it on abnormal exit. That is unbuilt
> work, not a blocked-on-a-kernel-bug item. PRISM's own `run` builtin is *not*
> disabled: `user/prism.c` dispatches it, and `smoke-shell` exercises
> `run /EXECTEST.ELF` twice plus `jobs`/`fg`.
- *Aside (general fork fix shipped here):* forked children previously resumed
  with only RIP/RSP/RAX set (other GP regs zeroed — a documented `sys_fork`
  limitation). 5e exposed it (init's non-inlined `nsi` faulted on `rbp=0`) and
  fixes it: the child now resumes with the parent's **full register frame** (the
  callee-saved snapshot captured at syscall entry, RAX=0) via `signal_sigreturn`.
  This is required for any forked C program, including PRISM's `run`.

### D6 — Gate `smoke-shell` and the input harness
`smoke-shell` boots the image with `-serial stdio`, feeds the guest UART a
command script (`echo …`, `help`, `exit`) **through a FIFO that opens only after
`PRISM_READY` appears** in the captured output, and asserts: `PRISM_READY`, the
`prism>` prompt, the `echo` output, the `help` listing, and **no kernel panic**.
Waiting for the prompt is what makes it reliable — feeding input during boot
overflows the UART before the shell reads. The FIFO lives in `/tmp` (DrvFs under
`build/` cannot host FIFOs); the serial log stays in `build/`. Self-contained
QEMU invocation (the existing `boot_test.sh` is output-only); all prior gates +
`smoke-fpu` + `smoke-init` stay green.

## Consequences
- First ring-3 input path on PRADYOS; unblocks any future interactive program.
- No new NSI number — `FD_CONSOLE` read is a behavior addition to `SYS_READ`;
  ADR-021 W^X and the append-only NSI are untouched.
- Deferred (tracked here + `build_status.md`): RX interrupts/line discipline,
  `SYS_READV` (musl stdin), ~~`SYS_GETDENTS` (`ls`), process-table syscall
  (`ps`)~~ — both shipped since, as NSI 66 / 67,
  pipes/redirection/quoting/job-control/scripting/history, respawn rate-limiting.

## Build order after 5e
`NET-B (lwIP)` → `Layer 6 (AETHER agent runtime)`. Do not start early.

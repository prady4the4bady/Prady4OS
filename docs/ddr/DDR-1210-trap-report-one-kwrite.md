# DDR-1210: the user-trap report is emitted as one kwrite

Status: accepted (2026-11-22). Scope: `kernel/idt.c` only.

## 1. The failing artefact

CI run 45011208867 (PR #158, head `247f9fd`), job `build-and-boot (shard 3)`,
gate `smoke-nethammer`: `required pattern 'net hammer spawned=2/2' not found`.

The probe itself passed: both NETHAMMER processes printed
`PRADYOS_NETHAMMER_OK ... conn_err=0`. The kernel did print the required line,
but another line was spliced into it, byte by byte:

```
[trap] user #PF page fault pid=33 name=METRIC.ELF rip=0x0000008000000151 ... bytes@rip=C7 40 40 EF BE AD DE 48 BF 86 00 00[user] EL 0F l0 8oaded0 0 (embedded);0 0 n0 et hammer spawned=2/2
```

## 2. Mechanism

- `[user] ELF loaded (embedded); net hammer spawned=2/2` is built with a `kline`
  and written by one `kwrite` (DDR-1055). `kwrite` holds `g_console_lock` for
  the whole buffer.
- The user-trap report in `kernel/idt.c` (`[trap] user ...`) is written in
  pieces. The `bytes@rip=` and `page+0=` hex dumps use bare `kputc`, and
  `kputc` takes no lock. Only `kputs`, `kwrite`, `kputhex` and `kputdec` do.
- METRIC.ELF faults on purpose on every boot (its probe expects the user kill).
  When that report runs on one CPU while another CPU is inside the `kwrite`,
  the unlocked `kputc` bytes land inside the locked line. The gate's `grep`
  then misses the pattern.
- DDR-1055's contract, "every printer in the tree takes that lock, so a kline
  is atomic against ALL of them", was false for this path.

## 3. Decision

- The user-trap report is assembled in one local 512-byte buffer and written
  with a single `kwrite`, under the same `console_line_trylock` as before.
- The bytes are unchanged: the same fields, order, `0x` + 16 upper-case hex
  digits, decimal pid and ` — killing process\r\n`. `smoke-user`'s sentinel
  (`[trap] user #PF page fault`) and `tools/ci/probe_rodata_check.sh` read the
  same text.
- **Why a local buffer, not a `kline`:** the full report is about 300 bytes,
  over `KLINE_MAX` (256), and a `kline` overflow prints `[kline] TRUNC`, which
  is in `GLOBAL_FORBIDDEN`. Every field has a bounded length (fixed hex widths;
  the exception and thread names are table entries and the scheduler's capped
  name), so 512 bytes cannot overflow.

## 4. Not changed, recorded

Two other bare `kputc` callers break the same contract, but no failing artefact
shows either one:

- `kernel/syscall/sys_io_uring.c:107`, a per-byte loop over a user buffer;
- `kernel/syscall/syscall.c:184`, a single-character syscall.

They are left as they are under the root-cause rule (non-negotiable 1). A
failure that names either one is fixed the same way.

## 5. Evidence and gating

- **This is a race.** It needs the METRIC.ELF report and a gate's `kwrite` to
  overlap on two CPUs, so no deterministic arm reproduces it. Its evidence is
  the artefact in §1.
- **After the fix:** `smoke-user` and `smoke-nethammer` pass, and the
  `[trap] user` line in their logs is byte-identical in form.
- **Not gated:** no arm reverts the fix and shows it red. Recorded, not hidden.

QEMU only, not verified on real hardware.

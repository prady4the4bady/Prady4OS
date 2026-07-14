# DDR-750 — kernel log ring buffer + `SYS_DMESG`

**Status:** proposed (pre-code)
**Layer:** console + syscall + user probe. First kernel-log-introspection slice.

## Problem

Everything the kernel prints goes straight out COM1 and is gone — there is no
in-memory record a running program can read back. Ring 3 has no `dmesg`: no way
to inspect boot/diagnostic output from within the OS. The console already funnels
every byte through one primitive (`kputc`), so a capture ring is a small, local
addition.

## Decision

**Kernel log ring (`kernel/console.c`).** A fixed 8 KiB circular buffer captures
every character emitted through `kputc` (the single funnel for `kputs`/`kwrite`/
`kputhex`/`kputdec` and direct callers — so it captures kernel logging *and*
ring-3 console writes). Capture runs under a dedicated leaf spinlock
(`klog_lock`), always taken innermost (the console lock, when held by the bulk
printers, nests outside it — consistent ordering, no deadlock). A per-char lock
is negligible against `kputc`'s existing UART busy-wait. `klog_read(dst, max)`
copies the most-recent `min(max, available)` bytes in chronological order into a
kernel buffer under the lock.

**Syscall.** `SYS_DMESG` (NSI 73) — `(char *buf, uint32_t max) -> bytes | -EFAULT`.
No capability (diagnostic log, matching the other introspection syscalls; a
sovereign-gated variant can come later if the log is deemed sensitive). The
handler caps a single call at 4 KiB, stages into a kernel stack buffer via
`klog_read` (lock held only around the in-kernel copy, never across `copyout`),
then `copyout`s.

## Gate — `smoke-dmesg` (new; 88 → 89)

Self-contained and ring-size-independent: a freestanding probe `user/dmesgtest.c`
(1) `SYS_WRITE`s a unique marker `PRADYOS_DMESG_MARKER` (which flows through
`kputc` into the ring), (2) `SYS_DMESG`s the recent log into a buffer, and (3)
searches that buffer for the marker. Found → `PRADYOS_DMESG_OK`, else
`DMESG FAIL`. This proves capture + read-back end-to-end without depending on
whether an early boot line is still resident in the 8 KiB ring. Gate asserts
`PRADYOS_DMESG_OK` (EXTRA_SENTINEL), `DMESG FAIL` forbidden.

## Non-goals

- No log levels / severity filtering, no timestamps per line, no `printk`-style
  rate limiting — a flat character ring.
- No blocking/follow (`dmesg -w`), no clear (`dmesg -c`).
- The lock is a leaf; `klog_read` never runs across `copyout` (no user fault
  under the lock).
- No PRISM `dmesg` builtin yet (keeps this off the shell); a shell surface can
  follow.

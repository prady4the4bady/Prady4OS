# DDR-809 — bound the THRE spin and drain RX inside it, so output cannot eat input

**Status:** design accepted; implemented in this slice.
**Date:** 2026-07-30
**Closes:** DDR-807 (S2 violation), OPEN-8 (console input loss).
**Relates to:** DDR-808 (mechanism), ADR-030 (`kwrite` atomicity), DDR-750
(log ring).

## The defect, restated from measurement

`kputs`/`kwrite` hold `irq_save()` across their whole loop while `kputc` spins
unboundedly on UART THRE. IRQ4 therefore cannot fire, so `console_rx_irq()`
cannot drain COM1's 16-byte RX FIFO. A burst of kernel output silently destroys
concurrent console input.

DDR-808 confirmed this over four runs: exactly one loss each, varying position
(`erun`, `ecat`, `eecho`, `eecho`), always the same signature — one character
accepted, the remainder of the command lost, the next command concatenated onto
the orphan.

## Design

Two changes to `kputc`, both inside the existing spin:

1. **Bound the wait.** `CONSOLE_THRE_MAX = 10000` iterations, then give up and
   drop the byte.
2. **Drain RX while waiting.** Call the existing `console_rx_irq()` from inside
   the spin, so bytes arriving during a TX burst reach the ring even though IRQ4
   is masked.

`kputs`/`kwrite`'s `irq_save()` … `irq_restore()` contract is untouched, so
ADR-030's atomicity guarantee — a whole buffer emits without another CPU
interleaving — still holds.

### Why not a BSP-only drain

The obvious guard is "drain only on CPU 0", making the drainer trivially the sole
producer. **It cannot be written as stated.** `kputc` emits the earliest boot
messages — `NEXUS: entered kmain` and everything before `percpu_init` — and
`this_cpu()` reads `%gs:0`. A `this_cpu()->is_bsp` test inside `kputc` would
dereference an uninitialised GS base on the very first character the kernel ever
prints, i.e. it would fault before it could protect anything.

Any CPU-identity check in `kputc` needs its own "is percpu up yet" guard, which
is a second global and a second ordering hazard, to solve a problem a lock solves
directly.

### Chosen instead: a producer lock on the RX ring

`console_rx_irq()` and the new in-spin drain both take `rx_lock` around the
`rx_head` update. This makes the producer side explicitly serialised and removes
CPU identity from the design entirely.

This **changes a documented invariant**, so it is stated plainly: `console.c`
described the ring as lock-free single-producer (IRQ4) / single-consumer
(`sys_read`). It is now multi-producer under a lock, single-consumer unchanged —
`kgetc_nb()` is still the only reader and still needs no lock.

**S6 check:** this takes a lock from ISR context, which S6 restricts to patterns
already in the tree. The pattern **is** already in the tree — `klog_putc()` takes
`klog_lock` via `spin_lock_irqsave`, and `kputc` calls it on every character
including from ISRs. So this adds no new deadlock surface class, and `rx_lock` is
a leaf lock taken under no other.

### Early-boot safety

The drain is gated on `g_rx_armed`, set at the end of `console_rx_init()`. Before
that point the UART's RX FIFO/IER are not configured and no reader exists, so
draining would be meaningless; after it, both are live. A plain global with no
percpu or LAPIC dependency, so it is safe from the first character printed.

## The bound is lossy, deliberately

At 10000 iterations the byte is dropped. That is a real trade-off and the
alternative is worse: an unbounded spin with interrupts disabled is exactly the
S2 violation being fixed, and on a genuinely stalled UART it takes the CPU — and
on the BSP, `g_ticks` with it, which silently un-bounds every `g_ticks`-deadline
wait in the tree.

10000 iterations of `inb` is far beyond any real THRE latency (QEMU sets it
within a handful) while remaining a hard ceiling. Under every configuration this
project runs, the bound is never reached, so it changes no current behaviour —
it only removes the unbounded case.

## Gate — deliberately ABSENT (S11)

A gate for this must genuinely back-pressure the UART TX, because QEMU drains
instantly and a "did not hang" assertion passes against the **broken** code too.
That needs either a QEMU serial backend that can be stalled (a pipe with no
reader) or a test hook forcing THRE low — neither is in the harness today.

Per S11 the gate is absent rather than stubbed. **`smoke-shell` is the real
regression test**: it fails 3/3 on this workstation today through exactly this
mechanism, so three consecutive passes with distinct kernel SHAs is meaningful
evidence, and DDR-808's four-run baseline is what it is measured against.

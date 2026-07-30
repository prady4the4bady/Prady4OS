# DDR-807 — `kputc` busy-waits on the UART without a bound, interrupts disabled

**Status:** defect confirmed by inspection; **fix NOT yet written** — see
"Why this is not a two-line change".
**Date:** 2026-07-30
**Found:** while investigating OPEN-1 (DDR-806). Independent of it — this stands
whether or not it turns out to be that mechanism.
**Relates to:** S2 (every wait bounded), S6 (no locks from ISR context),
DDR-750 (log ring), DDR-790 (dmesg ring eviction).

## The defect

`kernel/console.c:63`:

```c
void kputc(char c) {
    klog_putc(c);                             /* DDR-750: log ring (dmesg) */
    while ((inb(COM1 + 5) & 0x20) == 0) { }   /* wait for THRE — UNBOUNDED */
    outb(COM1, (uint8_t)c);
}
```

and `kputs` / `kwrite` wrap their loops in `irq_save()` … `irq_restore()`.

So the hot path of the most-called function in the kernel is an **unbounded spin
with interrupts disabled**. If the UART's Transmitter-Holding-Register-Empty bit
never sets — a back-pressured or unread host pipe, a full QEMU serial buffer, a
stopped consumer on the other end of `-serial file:` — the calling CPU spins
forever, IRQs off. No deadline, no escape, no diagnostic.

This is a plain S2 violation ("every wait, loop, timeout is bounded"), and it is
worse than the usual case because interrupts are masked: the stalled CPU cannot
be preempted, cannot take a timer tick, and on the BSP would take `g_ticks` with
it — which would in turn silently un-bound **every** `g_ticks`-deadline wait in
the tree, since those are only as bounded as the timer that advances them.

## Why it has never been seen

Under QEMU with `-serial file:`, the host drains the UART essentially instantly,
so THRE is set almost always and the loop exits on the first read. The defect is
invisible until something back-pressures the consumer. That is exactly the class
of bug that stays dormant for years and then presents as an unreproducible hang.

## Why this is not a two-line change

Adding a spin count is easy; deciding what happens when it expires is not, and
that decision is the whole DDR:

* **Drop the byte.** Console output becomes lossy under pressure. Every gate in
  this project asserts on serial sentinels, so a dropped byte is a spuriously
  red gate — trading a rare hang for rare, and far more confusing, false
  failures.
* **Return an error upward.** `kputc` returns `void` and is called from panic
  paths, ISRs and early boot where there is nothing sensible to do with a
  failure, and no caller is written to check one.
* **Skip the UART, keep the log ring.** `klog_putc` already ran, so `dmesg`
  retains the byte even if COM1 does not. This is the most promising direction:
  it bounds the wait, keeps the record, and degrades only the live console. But
  it means serial and `dmesg` can disagree, and every gate reads serial.

There is also an ordering question the fix must not get wrong: `klog_putc`
happens **before** the wait, so the ring is already correct in all three options.

## What the gate must prove

A discriminating gate has to actually back-pressure the UART, not simulate it —
asserting "we did not hang" against a QEMU that never stalls proves nothing and
would pass against today's code. Options to evaluate when the fix is written:
a QEMU serial backend that can be stalled (a pipe with no reader), or an
injected test hook that forces THRE low for a bounded interval.

Until that is settled the fix is not writable, because there would be no way to
show the bound is ever exercised. Per S11 the gate is absent, not stubbed.

## Explicitly deferred, and why that is safe

No code in this slice. The defect is dormant under every configuration this
project currently runs, and a careless bound would make the console lossy in the
one subsystem every gate depends on — strictly worse than the dormant hang.

**Do not fold this into a DDR-806 fix.** If the OPEN-1 stall does turn out to be
this spin, that is one more reason to fix it deliberately with its own gate,
rather than as an incidental edit inside another investigation.

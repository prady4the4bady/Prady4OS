= DDR-916 — GAP B: the RX drain must run per character, not only inside the THRE spin

**Status:** ACCEPTED. **Supersedes DDR-809's fix.** DDR-809's analysis, locking
design and THRE bound all stand; only the *placement* of the drain changes.
**Date:** 2026-08-13
**Lineage:** DDR-807 (unbounded THRE spin) -> DDR-808 (OPEN-8 mechanism: RX FIFO
overflow) -> DDR-809 (in-spin drain + THRE bound) -> **DDR-916 (this)**.

## The defect

DDR-809 correctly identified the mechanism — `kputs`/`kwrite` hold `irq_save()`
across the whole buffer, IRQ4 cannot run, COM1's 16-byte RX FIFO overflows, and
console input is lost — and added `console_rx_drain()` to close it. The fix is
real. **It just almost never executes.**

`kputc()` placed the drain *inside* the THRE busy-wait:

```c
while ((inb(COM1 + 5) & 0x20) == 0) {   /* entered ONLY if THRE is CLEAR */
    if (g_rx_armed) console_rx_drain();
    if (++spins >= CONSOLE_THRE_MAX) return;
}
outb(COM1, (uint8_t)c);
```

The loop body runs only when the transmitter is busy. On QEMU THRE is set
essentially immediately — DDR-809's own comment concedes "QEMU sets the bit
within a handful of reads" — so the first test usually succeeds, the body is
skipped entirely, and `kputc` transmits **without ever draining**.

The result: across a burst of kernel output, interrupts stay masked for the whole
buffer (the masking is required and stays — ADR-030 needs `kwrite` atomic) while
the drain that was supposed to compensate is effectively dead code. The FIFO
overflows exactly as before DDR-809.

**This is why DDR-808's signature still reproduces verbatim** — one character of
a command accepted, the remainder lost, the next command concatenated onto the
orphan:

```
prism> longrec-9x3-aaaaaecho st-fail=0
```

## Evidence

Two preserved arms, both on a tree that already contains DDR-809's fix, both
freshness-verified (`build_freshness.sh` reporting OK on a forced rebuild):

| artifact | size | reached |
|---|---|---|
| `build/artifacts/shell-20260813T111828Z.log` | 19,947 B | redirect stage; `cannot open /IN.TXT`, `longrec…echo` run-on |
| `build/artifacts/shell-20260813T113436Z.log` | 18,202 B | stopped after `uptime` |

Variable stopping distance across runs is DDR-808's own finding (loss lands
wherever an output burst coincides with the feeder writing), not evidence of a
timing desync.

## Hypotheses closed — do not re-open

- **Feeder desync / the gate's fixed sleeps.** REFUTED by DDR-808 across four
  runs. A sentinel wait cannot recover a byte the UART already dropped, and
  shipping it as the fix would leave this kernel defect wearing a green gate.
- **`/IN.TXT` missing from the fat-image fixture.** DEAD. `Makefile:1102` creates
  it at runtime (`echo in-marker-8w1 > /IN.TXT`); the `cannot open` line is a
  *downstream consequence* of the creating command being lost in transit. Adding
  it to the image would have masked the defect by making the later `cat` succeed.
- **GAP A — `CONSOLE_THRE_MAX` dropping bytes.** DEAD as a mechanism for THIS
  bug. That bound aborts a **transmit** and `klog_putc()` has already captured the
  character; it cannot affect the receive path. `10000` remains an unmeasured
  constant and deriving it is worthwhile on its own merit, but it is unrelated.
- **GAP C — another IRQ-off path without a drain.** Not tested. Not required:
  GAP B alone accounts for the observed loss.

## Decision

Drain **unconditionally, once per character**, on both sides of the transmit —
before the THRE test and after `outb` — so the inter-character window is covered
rather than only the (rare) blocked-transmit window. The in-spin drain stays for
the genuinely-blocked case.

Nothing else in `console.c` changes.

## Locking — inherited, no new surface

`g_rx_lock` (DDR-809) already serialises the two producers, is a leaf lock, and
has the documented order `g_console_lock -> g_rx_lock`, with the ISR taking only
`g_rx_lock` so no inversion is possible. Calling the same drain *more often* adds
no new lock, no new producer, and no new ordering constraint. The S6
ISR-context-lock justification is unchanged.

## Verification bar

Three freshness-verified arms with preserved artifacts, each showing the full
feeder sequence completing and the gate sentinel reached. Two arms are the
project minimum; `smoke-shell` gets three because it is the most timing-sensitive
gate in the suite and this investigation has had single-arm results mislead it
repeatedly.

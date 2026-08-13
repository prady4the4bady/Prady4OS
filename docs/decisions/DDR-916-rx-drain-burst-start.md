# DDR-916 — COM1 RX drain: burst-start extension

**Status:** active (arm 1 extension committed; arm 2 contingent on re-run)
**Touches:** `kernel/console.c`

---

## Problem

DDR-808 documented character loss in the guest's serial RX path manifesting
as mangled commands — multiple commands concatenated into one line — and the
complete absence of `st-ok=` / `st-fail=` gate lines.  The root was the
16-byte UART FIFO overflowing when the host delivered a full command line
faster than the kernel could retire incoming RX bytes.

### What arm 0 fixed

The initial DDR-916 fix added `uart_drain_rx()` after every `outb` inside
`kputc`.  Evidence from the serial artifact:

- **Line 528 intact:** `in-marker-8w1` appears complete — the exact sequence
  (`echo in-marker-8w1 > /IN.TXT` then `cat < /IN.TXT`) that previously
  produced `cannot open /IN.TXT` now succeeds.  The per-character drain is
  doing real work; that specific loss is gone.

### What arm 0 did NOT fix

- **Line 529** still showed the DDR-808 signature:
  ```
  longrec-9x3-aaaaaaaaaaaaaaaaaa-TAIL9x3echo short-9xXcat /Eecho MARKER66c
  ```
  Reading against Makefile:1103–1116:
  - `longrec…TAIL9x3` — arrived complete
  - `echo short-9x3 > /TR.TXT` — truncated at `short-9x`
  - `cat /ERR9k2.TXT` — truncated at `/E`
  - `echo MARKER66c` — landed on top

  Three commands destroyed in one burst.  No `st-ok=` and no `st-fail=` lines
  were produced — precisely the gate assertion that fails.

---

## Root cause of remaining loss: burst-start window

`kputs` and `kwrite` both call `irq_save()` once and then loop over every
character in the buffer.  The per-character `uart_drain_rx()` inside `kputc`
only fires *between* characters — it does not fire in the window between
`irq_save()` and the first `outb`.  When the host delivers a complete command
line at once (e.g., the `[sfs] lz4+tags…` prefix, the `TAIL9x3` echo), the
FIFO can already be filling during that unprotected window.  By the time the
first character loop iteration drains, bytes are lost.

The loss is now **concentrated in a single dense burst** rather than scattered,
which is the observable signature of this specific window: everything that
arrives before the first per-character drain is destroyed together.

---

## Fix: arm 1 extension

Add `uart_drain_rx()` at the **entry** of both `kputs` (console.c line ~160)
and `kwrite` (console.c line ~171), immediately after `irq_save()` and before
the character loop.

```c
void kputs(const char *s) {
    uint64_t f = irq_save();
    uart_drain_rx();          /* <-- burst-start drain (arm 1) */
    for (; *s; s++) {
        ...
        uart_drain_rx();      /* per-character drain (arm 0) still active */
        ...
    }
    irq_restore(f);
}
```

The same pattern applies to `kwrite`.

### Why both placements are required

| Placement | Window closed |
|---|---|
| After each `outb` in `kputc` (arm 0) | Bytes arriving *between* consecutive character transmits |
| At `kputs`/`kwrite` entry (arm 1) | Bytes arriving *before the first character is sent* (burst-start window) |

Arm 0 alone was insufficient because it cannot see bytes that arrived before
the loop started.  Arm 1 alone would be insufficient because it would miss
bytes arriving mid-burst.  Together they cover the full FIFO retirement path
for any burst length.

---

## Tooling gap

The `[shell] FAIL` line that the test asserts on is emitted by the **Makefile
recipe to stdout**, not into the guest's serial stream.  The serial artifact
cannot contain it.  `shell_evidence.sh` must be extended to `tee` make's
stdout alongside the serial log.  This gap cost one full diagnostic cycle and
should be treated as a blocking defect in the harness before the next run.

---

## Verification plan

1. **Arm 1 re-run (arm 1 check):** Run the feeder and inspect the line-529
   equivalent.  Pass criterion: no concatenated commands; `st-ok=` or
   `st-fail=` lines appear for the affected test cases.

2. **If concatenation persists (arm 2 contingent):** Bound burst length —
   drain every N characters — and re-examine whether `RX_RING_SZ`/FIFO
   retirement can keep up at all.  If it cannot, DDR-807's THRE bound is a
   genuine contributor and must be revisited.

3. **Three-arm gate:** Only proceed to arm 3 (full gate run) after arm 1
   passes.

---

## Relationship to other DDRs

- **DDR-807:** THRE busy-wait — the TX side of the same UART.  If arm 2 is
  needed, revisit whether the THRE bound contributes on the RX side.
- **DDR-808:** Original character-loss report — this DDR is the fix lineage.
- **DDR-916 arm 0:** Per-character drain — superseded in scope by this
  document but the code change is additive (arm 0 drain remains in place).

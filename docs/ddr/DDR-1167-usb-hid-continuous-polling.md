# DDR-1167 — USB piece 6: continuous HID keyboard polling into the key ring

Status: design committed before code. Date: 2026-10-14.

## §1 Goal

DDR-1165 read **one** boot-protocol report and stopped. A USB keyboard must
instead type into the system the way a PS/2 keyboard does:

* the ASCII ring read by NSI 46 (`SYS_INPUT_POLL`), which feeds `inputtest`, the
  compositor and the terminal window that runs PRISM;
* the structured event ring read by NSI 96 (`SYS_KEY_POLL`), which carries
  modifiers and make/break.

## §2 One decoder, not two

There are two ways to build this:

1. Write a second decoder that maps HID usages to `KEY_*` codes and pushes
   into both rings.
2. Translate each HID key transition into the **set-1 scancode bytes** a PS/2
   keyboard would have sent, and hand them to the existing `ps2kbd_feed()`.

**Option 2 is chosen.** `ps2kbd_feed` already does all the hard parts, and each
is gated:

* modifier aggregation from both sides (DDR-993);
* code/ascii identity, so make and break pair up (DDR-993 §4);
* keeping chords out of the text stream (DDR-992);
* F-keys and extended keys (DDR-991).

A second decoder would be a second copy of all of that, and two copies drift
(DDR-1037's `fd_ready_mask` reasoning). The translation table is the only new
logic.

## §3 HID report → transitions

A boot report is: a modifier byte, a reserved byte, and up to six key usages.
The driver keeps the previous report and compares the new one against it, in
this order:

1. For each modifier bit that changed, emit that modifier's make or break.
2. For each usage in the old report that is absent from the new one, emit a
   break.
3. For each usage in the new report that is absent from the old one, emit a
   make.

Modifier bits map to set-1 codes as follows:

| Bit | Key | Set-1 code |
|---|---|---|
| 0 | LCtrl | `1D` |
| 1 | LShift | `2A` |
| 2 | LAlt | `38` |
| 3 | LGUI | `E0 5B` |
| 4 | RCtrl | `E0 1D` |
| 5 | RShift | `36` |
| 6 | RAlt | `E0 38` |
| 7 | RGUI | `E0 5C` |

A break code is the make code with bit 7 set (`| 0x80`).

Usages are translated by a table covering:

* letters and digits;
* Enter, Esc, Backspace, Tab, Space;
* punctuation;
* F1–F12;
* the navigation cluster, sent `E0`-prefixed.

The rollover error code is usage 1 (`ErrorRollOver`). A report containing it is
**ignored whole**, because it carries no key state: it means "too many keys
held", not "all keys released".

## §4 Where it runs, and why that keeps the decoder single-writer

DDR-993 §9 records that `ps2kbd_feed` is unlocked. That is safe only because
exactly one CPU runs it: IRQ1 is PIC-routed to the BSP.

The USB poll runs from `timer_tick()`, which runs **only on the BSP**
(`idt.c:758`). Both IRQ1 and the timer enter through interrupt gates, which
clear IF, so neither can interrupt the other. The decoder therefore stays
single-CPU and serialized, with no lock added.

The same caveat as DDR-993 §9 applies. Moving either IRQ1 or the timer tick off
the BSP breaks this invariant, and that change must add a lock in the same
commit. This is stated at the call site.

## §5 Mechanism: one TRB outstanding

* Enumeration does not change.
* After `SET_PROTOCOL(boot)`, init queues one Normal TRB on the interrupt-IN
  ring, rings doorbell 3, and only then sets `hid_live`.
* `xhci_poll()` is called each BSP tick. It checks the cycle bit of the next
  event TRB, which is one memory read and no MMIO when idle. When a Transfer
  Event for DCI 3 is present it:
  1. consumes it;
  2. updates ERDP;
  3. processes the report;
  4. re-queues the TRB and rings the doorbell.
* Command events cannot be mistaken for transfer events. All commands are
  issued during init, before `hid_live` is set. After that the event ring has
  one consumer and nothing else posts to it except port changes, which are
  consumed and ignored.

The first non-empty report still prints `[xhci] hid-report cc= mod= k0=`.
`smoke-usbkbd`'s existing sentinels therefore keep their meaning, now printed by
the poller instead of a blocking loop.

The `usbkbd` probe key is removed. A USB keyboard is now product behaviour, not
a probe: a boot with no USB keyboard never reaches this code, and one with a
keyboard should type.

## §6 Gate

`smoke-usbkbd` gains the end-to-end arm:

* The FAT and SFS disks are attached so `INPUTTST.ELF` runs.
* The injector waits for `PRADYOS_INPUT_WAIT` and sends `b`.
* The gate requires `PRADYOS_INPUT_OK b`.

**This arm is vacuous unless a mutant proves otherwise.** q35 also has an
i8042, so if QEMU routed `sendkey` to PS/2, the arm would pass with no USB path
at all. DDR-1165 measured the routing: the `a` arrived as HID usage 4. That
covers the report, not the ring.

The mutant settles it. **M1 removes the `ps2kbd_feed` call from the poller.**
It must lose `PRADYOS_INPUT_OK b`; if it does not, this arm is vacuous and must
be redesigned.

**M2 skips the TRB re-queue.** A second distinct key must then fail to arrive.
The injector therefore sends `b` and then `c`, and the gate requires both
(`PRADYOS_INPUT_OK` lines for `b` and `c`; `inputtest` prints each key it reads).

## §7 Not claimed

* Boot-protocol keyboards only.
* No report-protocol parsing.
* No hubs, no hot-plug, and only one keyboard on the first connected port.
* No LED output reports (Caps/Num lock lights).
* Key repeat is whatever the device sends; there is no software typematic.
* Real hardware is not verified; QEMU `usb-kbd` only.

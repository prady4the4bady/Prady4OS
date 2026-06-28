# DDR-703 — PS/2 keyboard input routing to ring 3 (Layer 7)

> Layer-7 slice after the framebuffer surface (DDR-702). DDR before code. Adds the
> other half of interactivity: keyboard input reaching userspace, so a ring-3
> program can react to keys (the shell/compositor's input source).

## Context

The GPU framebuffer (ADR-028) + the ring-3 draw surface (DDR-702) let userspace
draw to the screen; nothing yet delivers input to it. The i8042 PS/2 controller is
present on q35 and IRQ1 is already unmasked, but the IRQ1 handler in `idt.c` is a
debug stub that only prints the scancode. This slice turns it into a real input
path: IRQ1 → scancode→ASCII → ring → `SYS_INPUT_POLL` → ring 3.

## Decisions

### D1 — A kernel keyboard ring, drained by a non-blocking syscall
`kernel/drivers/input/ps2kbd.c` owns a small (256-byte) ASCII ring. The IRQ1
handler (`ps2kbd_isr`) reads port `0x60`, ignores break codes (≥0x80) and modifier
bookkeeping beyond shift, translates **scancode set 1** make codes to ASCII via a
fixed table, and pushes the byte (drop on full — input is non-critical). One new
syscall:
```
SYS_INPUT_POLL(char __user *buf, int max) -> count (0 if none)   [46]
```
Non-blocking: returns immediately with however many bytes are buffered (≤max),
`copyout`'d. A ring-3 reader polls it (yielding between polls), matching the
existing console-RX ring pattern (`console.c`, IRQ4). No blocking/`read`-style
wait in this slice (keeps it simple; an epoll-able input fd is deferred).

### D2 — Shift handling only (minimal, correct)
Track left/right **Shift** make/break (0x2A/0x36 ↔ 0xAA/0xB6) to pick the
shifted/unshifted ASCII column. Caps Lock, Ctrl/Alt, the keypad, and key-repeat
tuning are deferred — not needed for the shell's basic input and easy to add to
the table later.

### D3 — IRQ1 stub replaced, EOI unchanged
`idt.c`'s inline IRQ1 case calls `ps2kbd_isr()` instead of printing; the existing
`pic_eoi` after the handler is unchanged. `ps2kbd_isr` does the minimum in IRQ
context (read port, push byte) — no allocation, like the console-RX handler.

## Gate

`smoke-input` (CI): a ring-3 reader (`user/inputtest.c`) prints
`PRADYOS_INPUT_WAIT`, then polls `SYS_INPUT_POLL`. The gate boots with an HMP
monitor on a unix socket and, once it sees `PRADYOS_INPUT_WAIT`, injects real key
presses via QEMU `sendkey` (so the i8042 raises IRQ1 — the genuine hardware path,
not an in-kernel fake). The reader collects the keys and prints
`PRADYOS_INPUT_OK <char>`; the gate greps it. (Same background-driver shape as the
`smoke-net` TCP gate.)

## Non-goals (later)
Caps/Ctrl/Alt/AltGr, key-repeat policy, an epoll-able `/dev/input` fd, mouse /
pointer (virtio-input), and IME — all deferred. This is raw keyboard bytes to
ring 3, the source the compositor's input stack will build on.

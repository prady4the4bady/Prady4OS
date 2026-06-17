# Phase 1 — PRADYOS-BOOT — Test Results

## Slice 1: Legacy BIOS MBR boot + serial sentinel

- **Date:** 2026-06-17
- **Environment:** WSL2 Ubuntu-22.04; NASM 2.15.05; QEMU 6.2.0 (SeaBIOS 1.15.0).
- **Artifact:** `boot/mbr/boot.asm` → `build/boot.bin` (512 bytes, signature `0xAA55`)
  → `build/pradyos.img` (boot sector laid down as sector 0 of a 1 MiB raw disk).

### Commands

```bash
make image    # nasm -f bin boot.asm; pad to 1 MiB raw disk
make smoke    # boots build/pradyos.img in QEMU, greps COM1 for the sentinel
```

### Expected vs actual

| | Expected | Actual |
|---|---|---|
| `make image` | builds 512-byte sector, exit 0 | **PASS** — `boot.bin` is 512 bytes, ends `55 AA` |
| `make smoke` | serial shows `PRADYOS BOOT OK`, exit 0 | **PASS** — `[smoke] PASS — saw 'PRADYOS BOOT OK'.` |

### What broke and how it was fixed (root cause, not patched over)

- **First smoke run FAILED** even though the sector printed correctly. The
  captured COM1 stream contained two interleaved copies of the string plus the
  QEMU monitor banner, so the contiguous-substring grep missed.
- **Root cause:** the runner used `qemu ... -nographic`. `-nographic` muxes the
  QEMU *monitor* onto the serial chardev being captured, and causes SeaBIOS to
  *mirror* the INT 10h BIOS-console output onto COM1. The boot sector writes the
  sentinel to both COM1 (serial) and the BIOS console (INT 10h), so both landed
  on the serial line, interleaved.
- **Fix:** `tools/qemu_runner/boot_test.sh` now uses `-display none -monitor none`
  (correct headless-serial setup) instead of `-nographic`. The guest's COM1 is
  then the only writer to the capture file. Re-ran: clean PASS. The boot sector
  keeps its INT 10h output so a *graphical* boot (real QEMU window / VirtualBox)
  still shows the sentinel on screen.

### Known limitations / next steps

- The sector ends in `hlt`; QEMU does not self-exit, so `make smoke` waits the
  full 30s watchdog before grepping. Acceptable for now. Future: add QEMU
  `-device isa-debug-exit` + a port write to exit immediately (faster CI).
- **VirtualBox not yet tested.** The protocol in the instructions calls for a
  VirtualBox boot after QEMU; deferred until the boot path does something a
  human needs to see (kernel load).

## Slice 2: Two-stage boot — A20, E820, CPUID, protected mode

- **Date:** 2026-06-17
- **Restructure:** Stage 1 is now a loader (INT 13h/AH=42h LBA read pulls 16
  sectors of Stage 2 to 0x0000:0x7E00 and jumps). Stage 2 (`boot/stage2/stage2.asm`)
  does the real work. Image = `stage1.bin` (LBA 0) + `stage2.bin` (LBA 1+),
  padded to 1 MiB. Build asserts stage1 == 512 B and stage2 <= 8 KiB.

### Commands

```bash
make image    # nasm stage1 + stage2, assert sizes, concatenate, pad to 1 MiB
make smoke    # boot + grep COM1 for PRADYOS BOOT OK
```

### Actual serial output (QEMU, AMD host)

```
PRADYOS S1: loading stage2...
PRADYOS S2: protected-mode loader
  A20: enabled (fast, port 0x92)
  E820 map entries (hex): 0x06
  CPU vendor: AuthenticAMD
  Long mode supported: yes
  switching to protected mode...
PRADYOS BOOT OK
```

`make smoke` → PASS (exit 0). Sizes: stage1 512 B, stage2 1496 B.

### What broke and how it was fixed

- **NASM warning** `uninitialized space declared in .text section` from the
  `resb` scratch buffers. Root cause: a flat `-f bin` image has no BSS, so `resb`
  at the end is ambiguous. Fixed by declaring the E820/vendor buffers as explicit
  zero-initialised data (`dw 0` / `times N db 0`). Rebuild is now warning-free.

### Honest scope notes / what is NOT done in Phase 1 yet

- **CPUID topology** is limited to the vendor string + the long-mode bit. Real
  SMT/core/package topology needs CPUID leaf 0Bh/1Fh; deferred to Phase 2c.
- **A20** is enabled but not exhaustively verified (QEMU enables it by default).
- **UEFI/OVMF path** not built (MBR chosen first).
- **Kernel-ELF load + hardware-info handoff struct** not built — there is no
  kernel to load until Phase 2a. The E820/CPUID data is gathered but not yet
  packaged for handoff.

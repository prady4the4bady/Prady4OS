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
  human needs to see (Stage 2 / kernel load).
- This is a print-and-halt sector only. Next slices: A20 + protected-mode
  transition, INT 15h E820 memory map, then CPUID vendor/topology detection.

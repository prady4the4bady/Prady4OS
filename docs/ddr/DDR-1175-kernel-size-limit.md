# DDR-1175: does `kernel.bin` have a size limit, and where does it come from?

Status: recorded. This is operator item 5 of #45 comment 6229808666. It
contains no code change.

## 1. Answer

**Yes, there are two real limits.** Neither is a chosen budget; both come from
the boot path. The Makefile enforces both after every `make image`
(`Makefile:792-795`).

| Limit | Value | Where it comes from | Measured now (`kernel.bin` 1,528,202 B, tip `71d482e`) |
|---|---|---|---|
| L1: file size | **1,572,864 B** (1.5 MiB) | The BIOS stage-2 loader reads the kernel in **48 chunks × 64 sectors × 512 B** from LBA 17 (`boot/stage2/stage2.asm`, `mov cx, 48`; DDR-733, raised by DDR-827 and then DDR-960). Bytes past the window are **never loaded**. | **44,662 B headroom** |
| L2: image + BSS | ends at **phys 0x600000** | The higher-half mapping is ONE page table (`PT_HI`, 512 × 4 KiB = 2 MiB) mapping `0xFFFFFFFF80000000 → 0x400000`, built identically in `stage2.asm` and `boot/uefi/loader.c` (§INV.13). Anything past 2 MiB from the start of the image is **unmapped**. | `__bss_end` = `0xffffffff801ab8c0`, i.e. image + BSS = 1,751,232 B; **345,920 B headroom** |

So **L1 binds first**. The UEFI path does not have L1: it loads the whole file.
It does share L2.

## 2. What happens at each limit

- **Past L1, without the gate:** under BIOS the tail of the kernel is silently
  not loaded. Code or data there reads as stale memory. The resulting failure
  would be a garbled boot or a fault far from its cause, and it would appear
  on the BIOS path only. **With the gate:** `make image` fails with a message
  that names the fix. That fix is to raise the chunk count, which is free up
  to 63 chunks within the 2 MiB disk image region.
- **Past L2:** the first access to an address beyond the mapped span page
  faults on both boot paths. The `__bss_end` check catches this before boot.

## 3. How to raise them

- **L1:** raise `mov cx, 48` toward 56. At 56 chunks, L1 = 1,835,008 B, and
  that plus the current 223,030 B of BSS still fits under L2. Past roughly 56,
  L2 becomes the binding limit.
- **L2:** add a second `PT_HI` (or 2 MiB pages) in **both** `stage2.asm` and
  `loader.c` in one commit. Then move the size gate's 0x600000 bound.

## 4. Not claimed

- No limit was invented. The 1.5 MiB figure is the loader's window, not a
  budget.
- Headroom is measured on this build only; any kernel change moves it.
- Kernel growth rate is not measured here. The most recent DDRs added
  4–12 KiB each (DDR-1171, DDR-1174), which is why L1 is the near-term
  constraint.

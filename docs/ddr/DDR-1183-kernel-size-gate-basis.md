# DDR-1183 — The kernel size gate: what it is, and a raise to 52 chunks (operator item 5)

**Status:** ACCEPTED. Operator item 5 (#45/6229808666) asked that the basis for the `kernel.bin` size gate be recorded in its own DDR. Order 3 (#45/6282431738) delegates the decision.

## 1. What the gate is, measured

The `make image` size gate (`Makefile`, `-le 1572864` until this change) is **not a design budget**. It is the BIOS stage-2 read window.

- **The read.** `boot/stage2/stage2.asm` reads the kernel from LBA 17 in a **fixed** number of 64-sector chunks (`mov cx, 48`), through a 32 KiB INT13 bounce buffer. It copies each chunk to `KERNEL_PHYS` (4 MiB) and again to `KIMG_PHYS` (8 MiB, the installer's pristine copy, DDR-1143 §10.4).
- **Why the count is fixed.** Stage 2 never learns the file size, so a `kernel.bin` larger than chunks × 32 KiB would boot with its tail missing.
- **History.** The window was 24 chunks (DDR-733), then 32 (DDR-827), then 48 (DDR-960). Each raise came when probes stopped fitting. Every embedded ring-3 probe ELF costs a page-aligned 8,192 B or more.
- **Today.** At `a2ac698` (`dev/phase1`), `kernel.bin` is 1,540,490 B, leaving 32,374 B of headroom. The two probes already built are:
  - `smoke-accmode` (#72), +8,192 B;
  - `smoke-thread` (#76), +20,480 B with its kernel code.

  Together they leave 3,702 B. One more probe after that does not fit.

**The numbers coupled to the chunk count, all re-checked for this change:**

| # | Number | Where | Before | After |
|---|---|---|---|---|
| 1 | chunk count, and `KIMG_SIZE` | `stage2.asm` | 48 | **52** |
| 2 | size gate | `Makefile` | 1,572,864 B | **1,703,936 B** |
| 3 | disk image | `Makefile` `truncate -s 2M` | 2 MiB (holds 63 chunks from LBA 17) | unchanged |
| 4 | DDR-831 scratch sector | 4095 | read ends at LBA 3089 | read ends at LBA 3345; still clear |
| 5 | installer check `17 + kernel sectors < INST_ESP_LBA` | `install.c:333`, 4096 | holds | holds (3345 < 4096) |
| 6 | KIMG copy at 8 MiB | `stage2.asm`, `kernel/boot_info.h` `KIMG_WINDOW` | 0x180000 | **0x1A0000**: inside its own 2 MiB page and below the 16 MiB PMM floor |
| 7 | PT_HI (2 MiB higher-half span) | `stage2.asm`, `boot/uefi/loader.c` | — | unchanged; see below |

**The invariant that caps the count.** The size gate must never admit an image that PT_HI cannot map, so a full window plus the BSS must be ≤ 2 MiB.

- The BSS measured at `a2ac698` is 223,414 B.
- At 52 chunks, a full window plus that BSS is **1,927,350 B, 169,802 B under the span**.
- At 56 chunks it would be 2,058,422 B, only 38,730 B under, which is too little for a BSS that grows with features.

So 52 is the largest raise that keeps the invariant with room to spare. The next raise past about 54 chunks needs PT_HI extended, in both `stage2.asm` and `loader.c` (CLAUDE.md, Boot).

**The UEFI path** loads the exact file size and is not bound by the chunk count. The size gate is a BIOS-path bound.

## 2. Decision

1. Raise the window to **52 chunks**: `mov cx, 52`, `KIMG_SIZE equ 52 * 0x8000`, `KIMG_WINDOW 0x1A0000`. The size gate becomes `-le 1703936`.
2. **Make the coupling enforced, not commented.** A new Makefile assertion, next to the size gate, reads the chunk count and `KIMG_SIZE` from `stage2.asm` and the `-le` value from the Makefile itself. The build fails unless `chunks == KIMG_SIZE chunks` and `chunks × 32,768 == gate`. Before this change, the three were kept in step only by comments.
3. Update every live size/headroom pairing (`CLAUDE.md`, `docs/PRE_LAUNCH_CHECKLIST.md`) in the same change, as `ci-docstate-check` requires. The checklist's §"Size is not the constraint" paragraph is a dated analysis. It is reworded as historical ("then-current gate"), not recomputed.

## 3. Evidence and mutation check

- **Build:** `make image` passes, and stage 2 is 1,540 B (≤ 8 KiB).
- **The new assertion has three arms, each turned red by a mutant:**

| Mutant | Change | Result |
|---|---|---|
| S1 | `mov cx, 50` (chunk count alone) | build fails: `chunk count (50), its KIMG_SIZE (52)` |
| S2 | `KIMG_SIZE equ 48 * …` (copy size alone) | build fails: `chunk count (52), its KIMG_SIZE (48)` |
| S3 | size gate back to `-le 1572864` (gate alone) | build fails: `size gate (1572864 B …)` |

  All three were measured locally, in this container's `make image`. The assertion is a build step, so the CI `build` job runs it on every PR, and the mutant branches are dispatched to CI as well.
- **Boot:** every BIOS-path smoke gate boots through the 52-chunk read, and that run is the evidence that stage 2 still loads and copies correctly.
- **What is not exercised yet:** the window's new tail, from 1,572,864 to 1,703,936 B, is first used when `kernel.bin` grows past 1,572,864 B. No current gate reads kernel bytes there. The first PR that crosses that size is the first evidence for the tail, and it must say so.
- Emulated (QEMU, SeaBIOS) only; not verified on real hardware. Real firmware INT13 extended reads of 64 sectors are what DDR-733 already relies on, and that is unchanged.

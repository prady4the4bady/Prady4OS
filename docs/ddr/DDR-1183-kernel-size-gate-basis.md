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

## Amendment 1 (2026-11-19): 52 -> 53 chunks

**Why.** The binding build, `RCU_TORTURE=1`, reached **1,704,330 B**, 394 B over the 1,703,936 B gate. That is DDR-1208 (virtio-blk bounce buffers) on top of C1.5 P5. `.text` ends 23 B past a page boundary (`0x78017`), and section padding to 4 KiB adds the rest. DDR-1188 Amendment 7 had already recorded the remedy: raise the window, do not squeeze code to fit.

**The coupled numbers, re-checked** (the table in §1):

| # | Number | Before | After |
|---|---|---|---|
| 1 | chunk count and `KIMG_SIZE` (`stage2.asm`) | 52 | **53** |
| 2 | size gate (`Makefile`) | 1,703,936 B | **1,736,704 B** |
| 3 | disk image | 2 MiB | unchanged (holds 63 chunks) |
| 4 | DDR-831 scratch sector 4095 | read ends at LBA 3345 | read ends at LBA 3409; still clear |
| 5 | installer `17 + kernel sectors < 4096` | holds | holds (3409 < 4096) |
| 6 | `KIMG_WINDOW` (`kernel/boot_info.h`) | 0x1A0000 | **0x1A8000**; 8 MiB + 0x1A8000 = 0x9A8000, still inside its 2 MiB page |
| 7 | PT_HI (2 MiB span) | — | unchanged; see below |

**PT_HI, measured, not assumed.**
- The BSS is now **324,928 B**, up from 223,414 B when this record was written.
- A full 53-chunk window plus that BSS ends at phys **0x5F7540, 35,520 B under 0x600000**.
- §1 called a 38,730 B margin "too little for a BSS that grows with features". This raise is therefore not free headroom. It spends most of the remaining span.
- What keeps it safe is that the ceiling is **build-enforced**: the Makefile check after the size gate fails any build whose image plus BSS ends past phys 0x600000.
- So the next growth of either the image or the BSS by more than about 35 KB needs PT_HI extended in `stage2.asm` and `boot/uefi/loader.c`. The build will say so; nothing silently breaks.

**The coupling assertion (§2) is unchanged.** It reads all three numbers, so S1 and S2 still apply. Checked locally on this change: `mov cx, 52` with `KIMG_SIZE` and the gate at 53 fails the build with the DDR-1183 message.

**Evidence (added after both PRs merged).**
- Every BIOS-path gate booted through the 53-chunk read: #147's head `236a273`, run [44537776868](https://github.com/prady4the4bady/Prady4OS/actions/runs/44537776868), every job green (merged as `3971031`). Its first run, on `d20619b`, died in the package-install step before any gate booted.
- Stage 2 reads and copies all 53 chunks on every BIOS boot, whatever the kernel's size: the loop count in `stage2.asm` is fixed. So #147's run already read and copied the 53rd chunk on every BIOS-path gate. What it could not show is a kernel whose own bytes lie in that chunk: on #147 the binding build was 1,700,234 B (CodeRabbit on #149).
- The first evidence for the tail is #148 (DDR-1208): its `smoke-rcu` job booted the 1,704,330 B `RCU_TORTURE=1` kernel and passed, on head `4298fde`, run [44540005938](https://github.com/prady4the4bady/Prady4OS/actions/runs/44540005938) (merged as `43d62c1`). That kernel ends 394 B into the 53rd chunk: the whole chunk is read and copied, and the first 394 B of it are kernel bytes that the boot then used.

**The UEFI path** loads the exact file size and is unaffected. QEMU only, not verified on real hardware.

## Amendment 2 (2026-11-23): 53 -> 54 chunks, and a 4 MiB kernel window (PT_HI x 2)

**Ordered by the operator on #45 (comment 7111929379, item 2).** Option 1 of 7091904894. The PR is the operator's to merge (a boot-loader PR).

### A2.1 Why, measured (`dev/phase1` = `037a9e4`)

- **The image gate binds.** The `RCU_TORTURE=1` and `LOCKDEP=1` builds are **1,733,010 B**, 3,694 B under the 1,736,704 B gate (CI run 45212716257). The next 4 KiB page crossing fails the build. C1.6 P3b and #72 both grow the image.
- **PT_HI binds almost as soon.** In the `RCU_TORTURE=1` build, measured locally here:
  - `__bss_start` is phys 0x5A71C0;
  - `__bss_end` is phys 0x5F6B80;
  - the BSS is 326,080 B;
  - so image plus BSS ends **37,952 B under 0x600000**, the end of the one 2 MiB PT_HI page.
- **So a chunk raise alone buys nothing.** A full 54-chunk window plus today's BSS ends at phys 0x5FF9C0, 1,600 B under 0x600000. The window would grow by 32 KiB while the real ceiling stayed where it is. The operator's order puts both changes in one commit for this reason.

### A2.2 Decision

1. **The read window: 54 chunks.** `mov cx, 54`, `KIMG_SIZE equ 54 * 0x8000`, and the size gate `-le 1769472`. The coupling assertion (§2) reads all three, so they move together.
2. **The kernel window: 0x400000..0x800000, 4 MiB, in two PT pages.**
   - `PD_HI[0]` points to PT_HI and `PD_HI[1]` to a new PT_HI2. PT_HI2 maps phys 0x600000..0x800000 in 4 KiB pages.
   - The window ends exactly where stage 2's pristine copy begins (`KIMG_PHYS` = 8 MiB). Neither can overrun the other: the Makefile ceiling below stops the image at 0x800000, and the copy starts there.
   - **Stage 2:** PT_HI2 is the seventh boot table, at 0x306000. It sits inside 0x300000..0x400000, which already holds the six tables and is clear of the kernel. The zeroing loop covers seven tables.
   - **UEFI loader:** a second `pt_hi` page, mapped the same way.
3. **The UEFI loader claims the whole window, not just the file.**
   - Today it claims `size` bytes at 0x400000, so the BSS above the file is memory the firmware still considers free.
   - The kernel zeroes that BSS on entry. Nothing has broken because OVMF allocates top-down, but nothing prevents it either.
   - With a larger BSS the gap gets larger. The loader now claims 1,024 pages (4 MiB) at 0x400000, refuses a file larger than the window, and still reads exactly `size` bytes.
   - The claim comes before `build_page_tables` allocates, as today, so no table can land in the window.
4. **The kernel's W^X pass covers the new span (`vmm_protect_kernel`, DDR-757 and DDR-1046).**
   - It re-stamps and audits both PT pages (1,024 entries), not one. Without this, the second 2 MiB would stay RW and executable, including any BSS there.
   - The identity alias is two 2 MiB PD entries now, `PD[2]` and `PD[3]`. Both are made NX and read-only, and both are read back and audited.
   - The `PRADYOS_WX_ALIAS` line reports both. It reads present=1 only if both are present, rw=1 if either is writable, and nx=1 only if both are NX.
5. **`kimg.c`:** the UEFI copy must lie clear of the kernel window, 0x400000..**0x800000**.
   - The old bound also excluded 0x300000..0x400000, which holds stage 2's page tables. Those tables exist only on the BIOS path; the UEFI loader allocates its own from the firmware.
   - With the whole window claimed, and OVMF owning 0x800000 up, OVMF places the copy just below 0x400000. Measured locally: base 0x25C000, size 0x1A3192.
   - The old bound refused that copy (`[kimg] refused reason=base`), and `smoke-uefi` failed. That run is what found this item.
   - The BIOS copy is still required at exactly `KIMG_PHYS`.
6. **The Makefile ceiling** after the size gate becomes `__bss_end <= phys 0x800000`.
7. `KIMG_WINDOW` becomes 0x1B0000. 8 MiB + 0x1B0000 = 0x9B0000 is still inside its own 2 MiB page and below the 16 MiB PMM floor.

**The coupled numbers, re-checked** (the table in §1):

| # | Number | Before | After |
|---|---|---|---|
| 1 | chunk count and `KIMG_SIZE` (`stage2.asm`) | 53 | **54** |
| 2 | size gate (`Makefile`) | 1,736,704 B | **1,769,472 B** |
| 3 | disk image | 2 MiB | unchanged (holds 63 chunks) |
| 4 | DDR-831 scratch sector 4095 | read ends at LBA 3409 | read ends at LBA 3473; still clear |
| 5 | installer `17 + kernel sectors < 4096` | holds | holds (3473 < 4096) |
| 6 | `KIMG_WINDOW` (`kernel/boot_info.h`) | 0x1A8000 | **0x1B0000** |
| 7 | PT_HI span (`stage2.asm`, `loader.c`, `vmm.c`, `kimg.c`, `Makefile`) | 2 MiB, ceiling 0x600000 | **4 MiB, ceiling 0x800000** |

**Headroom after this change.** The `RCU_TORTURE=1` build has 36,462 B under the new image gate. A full 54-chunk window plus today's BSS ends 2,098,752 B under the new 0x800000 ceiling. The image gate binds again first. The next raise past 54 chunks needs no PT change until about 63 chunks, where the 2 MiB disk image also binds.

### A2.3 The gate: `smoke-ptspan` (a flag gate)

No current gate touches the kernel window above 0x600000, because no build reaches it. A mutant that dropped PT_HI2 would therefore pass every existing gate, so a gate of its own is needed.

- **`PTSPAN_PROBE=1`** (default 0) adds a 1 MiB BSS array to the kernel. It is not page-aligned: `kernel.ld` places `.bss` at an unaligned address, and a 4 KiB alignment makes `ld.lld` warn. It is BSS, so `kernel.bin` does not change; `__bss_end` moves past 0x600000.
- After `vmm_protect_kernel`, the probe:
  - writes a pattern to the array's first and last page and reads it back;
  - resolves the last page's PTE in the kernel address space;
  - prints `PRADYOS_PTSPAN end=<phys of __bss_end> hi=<1 if past 0x600000> rw=<1> nx=<1> ok=<1>`.
- **The gate builds the flagged kernel and boots it twice:**
  - BIOS, from the hard-disk image;
  - UEFI, from the ESP image under OVMF.
- Each boot must print the line with `hi=1 ok=1`, and `[wx] kernel W^X OK`. The gate then puts the default kernel back and checks its hash, as `smoke-rcu` does.
- It runs in the `flag-gates` job (it rebuilds the kernel) and is listed in `shard_check.sh`'s `EXCLUDE` with that reason.
- **Vacuity:** on `037a9e4` the flagged build fails at the Makefile ceiling (image plus BSS past 0x600000). With the ceiling raised but no PT_HI2, the BIOS boot faults zeroing the BSS. So the gate cannot pass on the unfixed tree.

### A2.4 Mutants

| Mutant | Change | Expected red |
|---|---|---|
| W1 | stage 2 leaves `PD_HI[1]` empty | every BIOS boot: the W^X pass prints `[wx] kernel W^X FAIL: no PD[1]` (a forbidden pattern); `smoke-ptspan` BIOS also cannot zero its BSS |
| W2 | the UEFI loader leaves its second PT unmapped | every UEFI boot (`smoke-uefi`, `smoke-iso-x86`'s UEFI arm, `smoke-ptspan` UEFI): `no PD[1]` |
| W3 | `vmm_protect_kernel` re-stamps only the first PT | `[wx] kernel W^X FAIL` in every boot: the second PT's entries stay executable |
| W4 | the alias hardening leaves `PD[3]` RW and executable | `[wx] kernel W^X FAIL` in every boot |
| W5 | the Makefile ceiling back at 0x600000 | the `PTSPAN_PROBE=1` build fails |
| S1' | `mov cx, 53` with `KIMG_SIZE` and the gate at 54 | the build fails (the coupling assertion, §3's S1) |
| W6 | `kimg_base_ok` keeps the old 0x300000 bottom for UEFI | `smoke-uefi`, and the UEFI arm of `smoke-iso-x86`: `[kimg] refused reason=base` |

**Not gated, stated:**
- **The UEFI whole-window claim.** A revert passes unless the firmware places something in the BSS range, which OVMF does not do today.
- **The `kimg.c` bound's top (0x800000).** The copy cannot land in the window once it is claimed. Its bottom moving to 0x400000 is gated: `smoke-uefi` requires `[kimg] src=uefi len=… sha=…` and forbids `[kimg] refused`. On this change it failed with the old bottom (mutant W6 below).
- **The 54th chunk's bytes.** They are first kernel bytes once `kernel.bin` passes 1,736,704 B. Stage 2 reads and copies all 54 chunks on every BIOS boot, as §3 explains. The first PR that crosses that size is the first evidence for the tail, and it must say so.

QEMU (SeaBIOS and OVMF) only, not verified on real hardware.

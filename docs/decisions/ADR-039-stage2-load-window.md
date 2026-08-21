# ADR-039 — the stage-2 load window and the PT_HI ceiling are a contract

**Status:** ACCEPTED (binding). Supersedes nothing; **records** as a standing
contract what DDR-733, DDR-827 and DDR-960 each decided ad hoc.
**Date:** 2026-08-21

## Context — why this needs an ADR at all

Three times now the kernel has outgrown the bootloader's read window, and each
time the fix was rediscovered from scratch:

| | window | what forced it |
|---|---|---|
| DDR-733 | 640 KiB flat → 768 KiB @ 4 MiB | BSS crossed into the EBDA and the first timer tick `#GP`'d |
| DDR-827 | 768 KiB → 1 MiB (24 → 32 chunks) | linking ACC's crypto stack put `kernel.bin` 12,646 B over |
| DDR-960 | 1 MiB → 1.5 MiB (32 → 48 chunks) | no embedded probe ELF fit in 3,714 B of headroom |

Each rediscovery cost the same investigation, and DDR-956 / DDR-958 /
`tools/ci/revert_rename_embed.py` record **three separate features** abandoned
or rerouted against the third instance before anyone raised the window. The
numbers lived only in DDR prose and two source comments; nothing named them as a
contract, so nothing stopped them drifting apart.

There has never been an ADR for this. ADR-033 — cited in a prior session as the
governing document — is "third-party fetch source" and is unrelated.

## Decision — four coupled numbers, and which one is the real ceiling

### The numbers, as of this ADR

| # | quantity | value | lives in |
|---|---|---|---|
| 1 | stage-2 chunk count | **48** (48 × 64 × 512 = 1,572,864 B) | `boot/stage2/stage2.asm:199` |
| 2 | Makefile size gate | **1,572,864 B** | `Makefile:585` |
| 3 | disk image size | **2 MiB** (2,088,448 B usable from LBA 17) | `Makefile` `truncate -s 2M` |
| 4 | **PT_HI span** | **2 MiB** (`0x400000..0x600000`) | `stage2.asm:528-539` **and** `boot/uefi/loader.c:81-93` |

### The contract

1. **#4 is the hard ceiling, and it bounds image + BSS, not the image alone.**
   BSS is `NOLOAD` and sits immediately above the image, so every byte of image
   growth pushes `__bss_end` up one-for-one. The Makefile asserts
   `__bss_end(phys) ≤ 0x600000`. **That assertion is the load-bearing check in
   this whole chain** and must never be relaxed to make a build pass.

2. **#1 and #2 must always be equal.** The size gate exists to fail the build
   *before* an image ships that stage 2 would silently truncate. They are two
   spellings of one number in two files; changing either alone reintroduces
   exactly the DDR-827 failure.

3. **#2 must never admit an image that #4 cannot map.** Concretely:
   `size_gate + BSS ≤ 2 MiB`. At 48 chunks with today's 151,424 B BSS that is
   1,724,288 B, leaving 372,864 B of margin. **This is the invariant that caps
   the chunk count**, not the disk and not boot time — at 56 chunks the margin
   falls to 110,720 B, and at 64 the window alone consumes the entire 2 MiB span,
   leaving nothing for BSS.

4. **#3 is not automatically coupled.** DDR-827's comment said the image "must
   move with the stage2 chunk count". That was true at the 1 MiB step and is
   false now: a 2 MiB image covers up to **63 chunks**. DDR-960 was the first
   raise that did not need it. Growing the image reflexively is harmless but
   obscures which constraint is actually binding.

5. **DDR-831's scratch sector (LBA 4095) is safe for any count ≤ 63**, which the
   disk limit already implies. The Makefile checks the kernel's on-disk extent
   against it independently.

6. **PT_HI is implemented twice.** `stage2.asm` builds it for the BIOS path and
   `boot/uefi/loader.c` builds an identical 2 MiB span for the UEFI path. **Any
   extension past 2 MiB must change both files in the same commit.** Extending
   it is not a constant edit: it needs a second page table (`PD_HI[1] → PT_HI2`)
   and the zero-loop count at `stage2.asm:512` raised from six tables to seven.

### Safety margin chosen, and why

**1.5 MiB, i.e. 519,810 B of headroom above the current 1,053,054 B kernel — room
for 63 more embedded probe ELFs at 8,192 B each.** The minimum that would have
unblocked the immediate need was 33 chunks; that buys three probes and pays a
boot-path change for it. The margin is sized so this ADR does not need revisiting
for the foreseeable growth of the gate suite, while keeping rule 3's margin
intact.

## SMP / AP constraint — checked, and there is none

Raising the chunk count was examined against the AP bringup path specifically,
because that is the sensitive one:

- The **AP trampoline lives at `0x8000`**, below the `0x10000` bounce buffer and
  far below `KERNEL_PHYS`. The copy writes only `0x400000..`, so no window size
  reaches it.
- The **boot page tables at `0x300000..0x306000`** are below `KERNEL_PHYS`; the
  copy starts above them.
- **The GDT is untouched.** `go_unreal` already re-arms the 4 GiB DS/ES limits
  once per chunk under `cli` (DDR-733), precisely so the chunk count is
  irrelevant to segment state. More chunks means more re-arms, not different ones.

Verified empirically as well as by inspection: `smoke-smpuser` **5/5 with zero
`[BUG]`/`PANIC`/`#GP`/`[trap]` lines in all five logs** at the new window.

**Boot cost is not a constraint either**: boot-to-sentinel measured **0.38 s at
both 32 and 48 chunks**, five runs each, no variance.

## The measurement that proves a raise, and the one that does not

A passing size gate proves only that the file is small enough. It cannot
distinguish a working window from a no-op, because the kernel that passes it
usually fits the *old* window too. DDR-827 stated this and DDR-960 acted on it:

> A temporary 500 KiB `.rodata` pad linked early in `console.c` pushed every
> later section — including the embedded probe ELFs — past the old mark, giving
> a 1,556,862 B kernel. At **48 chunks** `smoke`, `smoke-user` and `smoke-fs`
> all passed. At **32 chunks**, the same kernel byte-for-byte, `smoke` and
> `smoke-user` failed with *"kernel sentinel not found"* — the image does not
> boot at all.

**Binding for future raises:** a window change is not verified until a kernel
larger than the *previous* window has been booted, and ideally until the same
kernel has been shown to fail at the old count. A short read kills the tail of
the image, so any gate that only exercises early code passes against a broken
load.

## Consequences

- The next feature that needs an embedded probe just adds it; 63 fit.
- The next raise past ~56 chunks is no longer a chunk-count change — it is a
  PT_HI extension in two files, and this ADR must be superseded, not amended.
- `sfs_rename` is gated (DDR-962) because this window exists. It was the first
  thing the raise bought and is the worked example of what the ceiling costs
  when it is left where it is.

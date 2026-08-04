# DDR-831 — the block self-test writes its scratch sector *into the kernel image*

**Status:** accepted
**Date:** 2026-08-05
**Governs:** `kernel/main.c` `blk_selftest`, `Makefile` image layout
**Root cause of:** OPEN-11
**Family:** a constant that encodes a fact about another subsystem and goes stale when that subsystem grows

## The defect

`blk_selftest` writes a 512-byte `7n+3` pattern to **LBA 1500**, justified by this
comment:

> *MUST be past the kernel's on-disk image: the kernel loads from LBA 17 and the
> build caps it at 256 KiB (512 sectors), so it can occupy up to LBA ~529. QEMU
> persists writes back to the image file, so writing into the kernel region would
> corrupt the kernel for the next boot. LBA 1500 is past the kernel and inside
> the 1 MiB (2048-sector) image.*

Every clause was true when written. None of them is true now:

| the comment assumes | reality today |
|---|---|
| kernel capped at 256 KiB (512 sectors) | cap is **1,048,576 B** (2048 sectors), raised by DDR-827 |
| kernel occupies up to LBA ~529 | kernel is **844,134 B = 1,649 sectors**, LBA 17 → **1,666** |
| image is 1 MiB / 2048 sectors | image is **2 MiB / 4096 sectors** (DDR-827) |
| LBA 1500 is past the kernel | **LBA 1500 is 1,483 sectors INSIDE the kernel** |

The self-test therefore writes a data pattern directly into the kernel's on-disk
image, and **QEMU persists it to the image file**. The next boot loads a kernel
whose `.rodata` — which is where `arch/x86_64/user_image.asm` incbins every probe
ELF — contains that pattern.

## Why this presented as an unfixable heisenbug

It was never nondeterministic. It is *stateful*:

- the **first** gate run after `make image` uses a pristine image and **passes**
- that same run corrupts the image
- **every later run** boots the corrupted kernel and fails

which reads as "~1 in 3 flaky" when gates are run ad hoc, and as "fails at gate 10
of 32" in CI, where nine gates run first. It also explains why the fault *changed
shape* between runs (`#GP`, `#PF`, `ELF_E_MAGIC`, no spawn at all): the corrupted
bytes land differently depending on which probe ELF the pattern overlaps.

And it explains the trigger. Linking ACC grew the kernel past LBA 1500 for the
first time — which is why "adding one probe broke a *different* probe", and why
`fd876cd` was green while `98fd2f8` was not.

## Evidence

```
[user] SHA-256 probe elf_load FAILED rc=-2 (ELF_E_MAGIC) free_frames=0x4D95
bytes@rip = 03 0A 11 18 1F 26 ...      <- 7n+3, continuous across the page
page+0    = 13 1A 21 28 2F 36 ...      <- index 112 at offset 0
```

`7n+3` has exactly two producers; `kernel/main.c:765` is the one that writes to
disk. The probe text contained it because the probe ELF was read out of a kernel
image that had been overwritten on disk.

## Decision

1. The scratch LBA must be **derived from the image geometry**, not a literal
   chosen against a stale assumption. Place it in the **last sector of the
   image**, which is by construction past any kernel that fits in the image.
2. Add a **build-time check** that the kernel's on-disk extent cannot reach the
   scratch sector. A constant that encodes another subsystem's size must be
   enforced by a gate, or it silently goes stale exactly as this one did.

## Why not the alternatives

- **Bump 1500 to a bigger number.** That reproduces the original defect with a
  new expiry date. The number must be derived and checked, not re-guessed.
- **Stop persisting QEMU writes (`snapshot=on`).** That hides the corruption
  rather than fixing it, and would also hide genuine disk-write regressions the
  FS gates exist to catch.
- **Drop the write/read round-trip test.** It tests something real; the bug is
  *where* it writes, not that it writes.

## The rule this earns

**A constant that encodes a fact about another subsystem must be enforced by a
check, not by a comment.** This comment was accurate, detailed, and carefully
reasoned — and it was wrong within weeks, because DDR-827 changed the fact it
described and nothing failed when it did. Comments do not fail builds; gates do.

# DDR-960 — raise the stage-2 read window from 1 MiB to 1.5 MiB

Status: ACCEPTED. Written before the code it governs (R16).
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (CLAUDE.md §0.4).

**Governing prior art:** DDR-733 (relocation to 4 MiB, the 24-chunk window) and
DDR-827 (24 → 32 chunks). There is **no ADR** for the load window — ADR-033 is
"third-party fetch source" and is unrelated. The window has always been governed
by these two DDRs. ADR-039 is written after this lands, to record the ceiling as
a standing contract rather than a number in two DDRs.

## 1. Problem

`kernel.bin` is **1,044,862 B** against a **1,048,576 B** read window:
**3,714 B of headroom.** Every embedded probe ELF costs a page-aligned
**8,192 B**, so no new probe fits. DDR-958 hit this — embedding
`renametest.elf` produced 1,053,054 B, 4,478 B over — and had to route its gate
through PRISM instead, which left `sfs_rename` ungated because a shell gate
cannot reach the SFS root. DDR-956 hit the same wall first.

This is not a rename-specific problem. It blocks **every** future ring-3 probe,
which is this project's only mechanism for gating a syscall.

## 2. The six questions, answered from the tree — measured, not assumed

### Q1 — current chunk count and load limit
`boot/stage2/stage2.asm:183` — `mov cx, 32`.
32 chunks × 64 sectors × 512 B = **1,048,576 B**, read from LBA 17 via INT 13h
into the bounce buffer at `0x10000`, copied up to `KERNEL_PHYS` in unreal mode
32 KiB at a time. The count is **fixed**: stage 2 always reads and copies the
full window regardless of the actual kernel size, so today it already writes
~3.7 KiB of past-the-end garbage into the head of BSS — harmless, because
`boot.asm` zeroes BSS after the copy.

This is the **only** chunk count in the tree (`grep` over `boot/`, `arch/`).

### Q2 — the PT_HI ceiling, verified from the source, not from the citation
**2 MiB, and it is structural.** `build_page_tables`
(`boot/stage2/stage2.asm:528-539`) fills **one** page table with 512 4 KiB
entries: `PD_HI[0] -> PT_HI`, covering `0x400000..0x600000`. 512 × 4 KiB = 2 MiB
exactly. Going past it is not a constant change — it needs a **second** page
table (`PD_HI[1] -> PT_HI2`), plus the zero-loop count at line 512
(`mov ecx, 0x6000 / 4`, six tables) raised to seven.

**The same 2 MiB span is implemented a second time, independently**, in the UEFI
loader (`boot/uefi/loader.c:81-93`), whose comment says "matching stage2's PT_HI
exactly". Any future PT_HI extension must move **both**. This change does not
touch either.

### Q3 — kernel load address
`KERNEL_PHYS = 0x00400000` (4 MiB) — `stage2.asm:48`, `kernel.ld:20`
(`KERNEL_LMA`), and `boot/uefi/loader.c:17`. DDR-733's value, confirmed in all
three places.

### Q4 — headroom between the current top and PT_HI
The binding runtime quantity is **image + BSS**, not `kernel.bin` alone: BSS is
`NOLOAD` and sits immediately above the image, so image growth pushes
`__bss_end` up one-for-one.

| quantity | value |
|---|---|
| `kernel.bin` | 1,044,862 B |
| `__bss_start` phys | `0x4FF180` |
| `__bss_end` phys | `0x524100` |
| BSS size | 151,424 B |
| **image + BSS** | **1,196,288 B** |
| PT_HI top | `0x600000` |
| **headroom to PT_HI** | **900,864 B** |

So the read window (3,714 B of slack) is tighter than PT_HI (900,864 B) by two
orders of magnitude. **The window is the binding constraint and PT_HI is not.**
That is what makes this a one-line change rather than a page-table change.

### Q5 — minimum raise, and what is actually chosen
One 8 KiB probe needs 1,044,862 + 8,192 = **1,053,054 B**, so the minimum is
33 chunks (1,081,344 B). That is rejected: it buys 28,290 B — three probes —
and this cost is paid in a boot-path change, which is not something to repeat
every three probes.

**Chosen: 48 chunks = 1,572,864 B (1.5 MiB).**

| | value |
|---|---|
| new window | 1,572,864 B |
| growth available | 528,002 B ≈ **64 probe ELFs** |
| image + BSS if the window were ever filled | 1,724,288 B |
| margin under PT_HI at a full window | **372,864 B** |

The last row is the invariant that matters and it is the reason for 48 rather
than more: **the size gate must never admit an image that PT_HI cannot map.**
At 56 chunks that margin falls to 110,720 B — less than one BSS growth spurt —
and at 64 chunks the window (2,097,152 B) equals the whole PT_HI span, leaving
literally zero room for BSS. 48 keeps a full window comfortably runnable.

48 is also the value DDR-733 originally proposed and DDR-827 reduced from, so
the number is not new to this boot path.

### Q6 — does this touch the GDT, page tables, or SMP AP bringup?
**No, and each is checked rather than assumed:**

- **GDT** — untouched. `go_unreal` reuses the existing `gdt_desc` / `DATA32_SEL`
  and is already re-armed once per chunk, under `cli`, precisely so chunk count
  is irrelevant to it (DDR-733). More chunks means more re-arms, not different
  ones.
- **Page tables** — untouched. Both the boot tables (`0x300000..0x306000`) and
  the copy destination are unaffected: the copy still starts at `0x400000` and
  now ends at `0x400000 + 48 × 0x8000 = 0x580000`, which is **inside** PT_HI's
  span and below `0x600000`. Nothing lives in `0x524100..0x580000`; it is below
  `PMM_MIN_PHYS` (16 MiB) so the allocator never hands it out.
- **AP trampoline** — untouched. It lives at `0x8000`, below the bounce buffer
  at `0x10000` and far below `KERNEL_PHYS`. The copy writes only `0x400000..`.
  There is no path by which a larger window reaches it.
- **Bounce buffer** — unchanged at `0x10000`, still 32 KiB per chunk.

The one genuine coupling is the **disk**, and it is the coupling DDR-827 warned
about: from LBA 17, N chunks read through LBA `17 + 64N`. At 48 that is LBA
**3089**. The image is already 2 MiB (4096 sectors) from DDR-827, so **the image
does not need to grow this time** — the first change in this chain where that is
true. It is stated explicitly so nobody assumes the two must always move together.

**DDR-831 interaction, checked:** `blk_selftest` writes a scratch sector at LBA
**4095** and QEMU persists it. 3089 < 4095, so the window never reads it. More
importantly the *worst case* is safe too: at the new size gate a maximal kernel
occupies through LBA 3089, so the Makefile's DDR-831 check cannot be tripped by
anything the new window admits. Any N ≤ 63 holds that property; 48 has margin.

## 3. Change — three numbers, all in one commit

1. `boot/stage2/stage2.asm:183` — `mov cx, 32` → `mov cx, 48`, and the comment
   block above it (lines 141-155) rewritten to the new arithmetic.
2. `Makefile:585` — size gate `1048576` → `1572864`, message updated.
3. `Makefile:604` — the `truncate -s 2M` comment, which claims the image size
   "must move with the stage2 chunk count". It must not, at this size, and
   saying so is what stops the next person growing the image needlessly.

**Not changed:** `truncate -s 2M` itself, PT_HI, the UEFI loader, the
`__bss_end ≤ 0x600000` assertion. That assertion is the real ceiling and is left
exactly as DDR-827 left it — it now becomes the *only* thing standing between a
growing kernel and an unmapped tail, so it is deliberately untouched.

## 4. Verification — DDR-827's four, plus a boot-time measurement

DDR-827 established that a size gate alone proves nothing: it only proves the
file is small enough, not that stage 2 read all of it. A short read corrupts the
**end** of the kernel, so a gate touching only early code passes against a
broken load. Its four required checks are repeated here:

1. `make image` — size gate passes at the new limit.
2. `make smoke` — boots at all.
3. `make smoke-user` + `make smoke-fs` — the tail of the image is really there.
4. A gate at `-smp 4` — the AP trampoline lives in the image too.

Two additions specific to this change:

5. **`smoke-smpuser` 5×** — the prompt's requirement, and the right one: AP
   bringup is the sensitive path and a single green cannot distinguish "works"
   from "did not fail this time".
6. **Boot-time delta, measured.** 16 extra chunks is 16 extra INT 13h round
   trips plus 512 KiB more `rep movsd`. Every gate timeout in this tree is a
   claim about wall-clock (CLAUDE.md §8), so the cost is measured rather than
   asserted to be negligible. If any gate's margin materially narrows, the
   chunk count comes down — that is the one thing that would revise §2 Q5.

## 5. Measured — all of §4, on this host

QEMU 8.2.2, TCG, no KVM. Kernel `6a254f13b9fd2b9fc9e8f2597eca9767`,
`kernel.bin` 1,044,862 B, `stage2.bin` 1,444 B (limit 8,192).

| check | result |
|---|---|
| `make image` | rc=0, 0 warnings at `-Werror`; size gate passes at 1,572,864 |
| `make smoke` | PASS |
| `make smoke-user` | PASS — 7 FS patterns |
| `make smoke-fs` | PASS — 14 FS patterns |
| `make smoke-smpuser` ×5 | **5/5 PASS**, and **0** `[BUG]`/`PANIC`/`#GP`/`[trap]` lines in all five logs |
| boot-to-sentinel, 32 chunks | 0.38 s (5 runs, no variance) |
| boot-to-sentinel, 48 chunks | **0.38 s** (5 runs, no variance) |

**Boot time did not change measurably.** 16 extra INT 13h round trips plus
512 KiB more `rep movsd` are below the resolution of the boot-to-sentinel
measurement. §4's condition for revisiting the chunk count is therefore not
met, and 48 stands.

### The check that actually matters: a >1 MiB kernel, two arms

Everything above ran on a 1,044,862 B kernel — which the **old** 32-chunk window
already covered. None of it can distinguish a working raise from a no-op. This
is exactly DDR-827's lesson ("the size gate only proves the file is small
enough"), so the raise was proved directly.

A temporary 500 KiB initialised `.rodata` pad was linked into `console.c` —
early in the link order, so it pushes every later section, **including the
embedded probe ELFs in `user_image.o`**, past the old 1 MiB mark. Resulting
kernel: **1,556,862 B** — 508,286 B past the old window, 16,002 B inside the new
one, ending at LBA 3058 (clear of DDR-831's scratch sector 4095).

| arm | chunk count | result |
|---|---|---|
| **A** | 48 | `smoke` **PASS**, `smoke-user` **PASS** (7 patterns), `smoke-fs` **PASS** (14 patterns) |
| **B** | 32 | `smoke` **FAIL**, `smoke-user` **FAIL** — *"kernel sentinel 'NEXUS KERNEL OK' not found"*, i.e. **the image does not boot at all** |

Arm B is the same kernel byte-for-byte, with only the chunk count reverted. Its
failure is DDR-827's signature reproduced deliberately: the tail is never read,
so the image is not merely degraded, it is dead. That is what makes arm A
evidence rather than an absence of evidence.

Pad removed and chunk count restored afterwards; the rebuilt kernel reproduces
md5 `6a254f13b9fd2b9fc9e8f2597eca9767` and `make smoke` is green again — a
revert is not verified until the gate is re-run.

## 6. What this does NOT do

- It does not extend PT_HI. When image + BSS eventually approaches 2 MiB, that
  is a second page table in `stage2.asm` **and** the matching change in
  `boot/uefi/loader.c` — two files, not one, and its own DDR.
- It does not make the loader ELF-aware or size-aware. Stage 2 still reads a
  fixed window. Reading only as many chunks as the kernel needs would remove the
  boot-time cost of a large window entirely, but it requires the loader to know
  the image size, which means a header stage 2 must parse. Out of scope; noted
  as the obvious next refinement if boot time ever becomes the binding cost.

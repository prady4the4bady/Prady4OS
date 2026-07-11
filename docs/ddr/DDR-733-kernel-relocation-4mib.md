# DDR-733 — Kernel relocation to 4 MiB (unreal-mode bounce load)

**Status:** proposed (pre-code)
**Layer:** boot (stage2 + link script); the growth slice DDR-730 predicted.
**Supersedes:** the flat-below-640K load geometry of ADR-005/ADR-007 (the
higher-half VMA and everything above the loader are unchanged).

## Problem — the 640 KiB wall, hit for real

The kernel is loaded flat at physical 0x10000 by real-mode INT 13h, so the
entire image + BSS must fit below the top of conventional RAM (E820 usable ends
0x9FC00; 0xA0000.. is the EBDA/VGA/ROM hole). That is a hard ~575 KiB ceiling.

DDR-732's ~4 KiB of daemon growth crossed it: `__bss_end` reached phys
**0x9FE80**, putting the last ~640 bytes of kernel BSS (late-linked scheduler /
lwIP state) inside the reserved EBDA. The first timer tick read garbage and
#GP'd in `sched_tick` — a deterministic boot panic caught by the gates. The
Makefile's 544 KiB *file* check was insufficient: the binding quantity is
file + BSS. There is no headroom left to shave; relocation is the root fix.

## Decision — load high via a low bounce buffer

**New physical home: 4 MiB (`KERNEL_PHYS/KERNEL_LMA = 0x400000`).** Everything
below 16 MiB is already outside the PMM pool (`PMM_MIN_PHYS`), so no allocator
change is needed. The boot page tables stay at 0x300000 (now *below* the kernel,
still outside its window); the AP trampoline stays at 0x8000; the transient
`KSTACK_TOP = 0x200000` stage2 stack remains in free identity-mapped RAM (the
kernel switches to its BSS stack in `boot.asm` immediately).

**Loader (stage2):** real mode cannot write above 1 MiB directly, so each
64-sector chunk is INT 13h-read into a **bounce buffer at 0x10000** (the old
kernel home — free real-mode RAM) and copied up with **unreal mode** (a.k.a.
big real mode: enter PM briefly, load DS/ES with the 4 GiB `DATA32_SEL`
descriptor, drop back to real mode — the cached descriptor limits persist; the
technique rests on the segment-descriptor cache semantics of Intel SDM Vol. 3
§9.9.2 "switching back to real-address mode"). The copy is
`a32 rep movsd` with `cli` held, and the unreal DS/ES limits are **re-armed at
every chunk** — a BIOS interrupt handler between chunks may reload segment
registers and reset their cached limits, so re-arming per chunk (a few µs)
removes any dependence on BIOS behavior. INT 13h itself runs with interrupts
enabled as before.

**Load window: 24 chunks = 768 KiB** (fits the 1 MiB disk: LBA 17 + 1536 =
1553 < 2048). The higher-half `PT_HI` still maps a 2 MiB span
(`0x400000..0x600000`), so the *runtime* ceiling becomes image + BSS ≤ 2 MiB.

**Checks (Makefile), now honest:**
- file ≤ 768 KiB (the 24-chunk read window; the next bump is a chunk-count +
  disk-size change, no longer physics), and
- `__bss_end`(phys) ≤ 0x600000 (the PT_HI span) — computed from `nm`, so BSS
  growth can never silently cross a mapping boundary again.

**Touchpoints** (complete list): `boot/stage2/stage2.asm` (KERNEL_PHYS, unreal
helper, bounce load loop, comments), `kernel/kernel.ld` (KERNEL_LMA),
`Makefile` (both checks + comments), stale comments in `arch/x86_64/boot.asm` /
`kernel/apic/smp.c` naming 0x10000. Nothing else references the load address —
verified by grep and by the PMM floor at 16 MiB.

## Gate

No new gate: **every existing gate exercises this loader** (nothing boots
without it), and the panic that motivated the slice was itself gate-caught.
The full suite (77 gates) must pass, including the SMP set (AP trampoline
below the relocated kernel) and `smoke-user`/`smoke-fs*` (identity-map reads of
kernel-adjacent buffers). DDR-732 then lands on top with `smoke-aethercfg`
(78 gates) — its ~4 KiB growth is what forced this slice.

## Non-goals

- No ELF-aware loader, no >2 MiB kernel support (extend PT_HI when needed —
  the Makefile check now names that boundary).
- No move of the AP trampoline or boot_info.
- No PMM floor change (16 MiB stays; the kernel window is below it).

# DDR-872 — XSAVE/XRSTOR in the context switch (Group 3 item 18)

**Status:** Accepted — **unblocks items 42/43's AVX paths**
**Date:** 2026-08-08
**Scope:** `kernel/proc/sched.h`, `kernel/proc/sched.c`.

## Why this had to come first

DDR-871 could not use AVX in `fast_memcpy` because the context switch saved FPU
state with **FXSAVE** — 512 bytes covering x87 and XMM0–15 only. YMM upper
halves were not saved and ZMM was not saved at all, so any kernel or ring-3 use
of the wide registers was silently corrupted across a preemption.

That is now fixed, and items 42/43 can add their vector paths.

## Decisions

**The per-thread area is sized by a ceiling that is ASSERTED, not assumed.**
`FPU_STATE_MAX` is 4096 (x87+SSE 512, AVX 576, full AVX-512 ~2688, plus
margin), and boot compares it against `CPUID.0xD.0:EBX` — the size for the
components *actually enabled* — and **panics** if the CPU needs more.

A short XSAVE area is not a smaller save. XSAVE writes past the end of the
`tcb`, corrupting whatever the allocator placed after it, and the damage would
surface anywhere except here. Halting at boot with the two numbers printed is
the only honest response.

**Alignment moved 16 → 64.** FXSAVE requires 16-byte alignment; XSAVE requires
64. The old attribute would have been silently insufficient.

**Order of operations is load-bearing.** `CR4.OSXSAVE` before `XSETBV`, and
`XCR0` set **before** the size query — because `CPUID.0xD.0:EBX` reports the
size for the components *currently enabled*. Querying first would size the area
for x87+SSE and then enable more state than it can hold, which is the corruption
case above arrived at from the other direction.

**Only components the CPU reports are enabled.** `XSETBV` `#GP`s on a bit the
CPU does not implement, which would fault the boot path. The AVX-512 trio
(opmask, ZMM_hi256, Hi16_ZMM) is enabled only if all three are present — they
are not independently useful.

**MPX and PKRU are deliberately excluded.** Nothing here uses them, and every
enabled component enlarges the save area and lengthens every switch.

**The init template is built with the same instruction the switch restores
with.** An FXSAVE image is *not* a valid XSAVE area; mixing them would XRSTOR
garbage into the first thread to run. So:

- XSAVE path → a **zeroed** area. `XSTATE_BV = 0` tells XRSTOR to put every
  component in its INIT state, and `XCOMP_BV = 0` selects the non-compacted
  format XSAVE produces. Cleaner than capturing a live image, which would bake
  in whatever the BSP happened to hold.
- FXSAVE path → a **captured FNINIT image**, because a zeroed FXSAVE area is
  *not* clean: it loads `MXCSR = 0`, unmasking every SSE exception. That
  asymmetry is the reason the two branches exist rather than one memset.

## Verification

**Both paths proven to engage** — a silent fallback everywhere would make this
item a no-op that still passes every gate:

| `-cpu` | boot line |
|---|---|
| `qemu64` | `XSAVE absent — FXSAVE (x87+SSE only)` |
| `Nehalem` | `XSAVE absent — FXSAVE (x87+SSE only)` |
| `Skylake-Client` | `XSAVE on` |
| `Skylake-Server` | `XSAVE on` |

`smoke-fpu` passes on both extremes (`qemu64`, `Skylake-Server`).

This touches every context switch, so the regression was wide — **14/14 green**:
`smoke`, `-fpu`, `-user`, `-shell`, `-cowfork`, `-sysfork`, `-syswait`, `-smp`,
`-smpsched`, `-bench`, `-ftruncate`, `-fs-sfs-rw`, `-init`, `-sysexec`.
Zero warnings under `-Werror`.

## Cost

The `tcb` grows by 3.5 KiB. Thread control blocks are `kmalloc`'d, so this is
heap rather than the low-memory BSS budget. The switch cost rises only on CPUs
that actually have the wide state — on `qemu64` the FXSAVE path is unchanged.

**Group 3 item 18 complete.** Items 42 and 43 may now use AVX2/AVX-512; the
feature bits are already probed and recorded by DDR-871.

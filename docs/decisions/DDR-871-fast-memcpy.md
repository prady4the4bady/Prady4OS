# DDR-871 — fast_memcpy with CPUID dispatch (Group 8 item 42)

**Status:** Accepted — **AVX paths deferred to item 18, with cause**
**Date:** 2026-08-08
**Scope:** `arch/x86_64/fast_memcpy.asm`, `kernel/string.c`, `kernel/main.c`.

## The finding that shaped this: AVX cannot be used yet

The queue asks for AVX-512 / AVX2 / SSE4.2 / REP MOVSB. **The first two cannot
be used by this kernel today**, and writing them anyway would have silently
corrupted ring-3 state:

- The kernel is built `-mgeneral-regs-only` and by design **never touches
  FPU/XMM/YMM/ZMM** (`cpu_mitigations.c`).
- The context switch saves FPU state with **FXSAVE** — 512 bytes covering x87
  and XMM0–15 **only**. It does not save YMM upper halves and does not save ZMM
  at all. That needs XSAVE with a larger per-thread area.

A kernel memcpy touching YMM/ZMM would clobber vector registers belonging to
whichever ring-3 thread was interrupted. The corruption would surface later, in
that thread, as wrong arithmetic — about as far from the cause as a bug can get,
and invisible to every existing gate.

**So item 42's AVX paths are blocked on item 18** (AVX-512 state save/restore).
The queue lists them as independent; they are not. Recorded here rather than
discovered later by a corrupted user thread.

## What is implemented

| CPU advertises | path | why |
|---|---|---|
| **ERMS** (`CPUID.7.0:EBX[9]`) | `rep movsb` | microcoded wide copy, moves a cache line per internal iteration, **uses no vector register** — the fastest option that is also safe without XSAVE |
| otherwise | `rep movsq` + byte tail | correct on the pre-ERMS models the chipset matrix still covers (`qemu64`, `Nehalem`) |

Both use general-purpose registers only, so neither disturbs FPU state and
neither requires a context-switch change. AVX2 and AVX-512F feature bits **are**
probed and recorded, so item 18 only has to add branches rather than redo the
detection.

`fast_memcpy_init()` runs on the BSP before APs start, so the flag it writes is
read-only by the time a second CPU exists — no locking needed.

**CPUID leaf 7 is guarded on the maximum basic leaf.** Executing leaf 7 on a CPU
that does not have it returns whatever leaf the CPU clamps to, which would be
read as garbage feature bits.

## memmove deliberately does NOT use it

`rep movsb` copies strictly forward, so it is correct for `memmove` only when
the destination is below the source. Routing an overlapping backward move
through it would corrupt the tail — and only for overlapping arguments, which is
exactly the case a quick test does not cover. The existing byte loop already
handles both directions.

## Verification

**Both arms proven distinct, not merely both passing.** Two gates passing means
nothing if both took the same path, so the detected flag was printed per CPU
model:

| `-cpu` | detected | arm taken |
|---|---|---|
| `qemu64` | `erms=0` | fallback |
| `Nehalem` | `erms=0` | fallback |
| `Skylake-Client` | `erms=1` | `rep movsb` |

and the ftruncate gate passes on all three.

`memcpy` is used by essentially everything, so the regression was wide rather
than token — **15/15 green**: `smoke`, `-user`, `-shell`, `-fs`, `-fs-rw`,
`-fs-sfs-rw`, `-fs-ext4`, `-sfs-persist`, `-net`, `-sha256`, `-aead`, `-vault`,
`-bench`, `-ftruncate`, `-init`. Zero warnings under `-Werror`.

Static cost (item 44 convention): dispatch 2 instructions + 1 branch; ERMS arm 3
instructions; fallback arm 7; no stack traffic, no locks, no serialising
instructions on either.

**Group 8 item 42 complete for every path this kernel can safely take.**
Item 43 (`ipc_copy.asm`) is next and inherits the same constraint.

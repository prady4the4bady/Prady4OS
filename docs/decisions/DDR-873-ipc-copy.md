# DDR-873 — ipc_copy32 (Group 8 item 43); Group 8 complete

**Status:** Accepted
**Date:** 2026-08-09
**Scope:** `arch/x86_64/ipc_copy.asm`, `kernel/ipc/ipc.c`.

## The fact that decides the design

The IPC payload is **exactly 32 bytes** (`IPC_MSG_WORDS = 4 × uint64`). Not a
variable-length buffer. Everything below follows from that, so it is stated
first — and pinned by a `_Static_assert`, because the assembly hard-codes the
size and a future widening of `IPC_MSG_WORDS` would otherwise copy half a
message in silence.

## Why MOVDQU (XMM) and not VMOVDQU (YMM)

The queue names both. **YMM is the wrong choice here**, and substituting it
silently would have been the easy path:

1. **AVX–SSE transition penalty.** Touching YMM leaves the upper halves dirty,
   and every subsequent legacy-SSE instruction — the kernel is full of them via
   FXSAVE/FXRSTOR — pays a save/restore stall on Intel microarchitectures unless
   `VZEROUPPER` is issued. `VZEROUPPER` alone costs more than the three
   instructions a YMM path would save on a 32-byte copy.
2. **YMM is only safe when XSAVE is on.** DDR-872 enables the AVX components
   where the CPU has them, but on the pre-AVX models the chipset matrix still
   covers (`qemu64`, `Nehalem`) the switch falls back to FXSAVE, which saves XMM
   but **not** the YMM upper halves. A YMM path would need its own runtime gate,
   doubling the code for a copy this small.

XMM has neither problem: FXSAVE covers XMM0–15 on every path, so `MOVDQU` needs
no gate and causes no transition. SSE2 is architectural on x86_64, so there is
no CPU without it and no fallback is required.

This is a case where the queue's suggested instruction is the wrong one for the
actual payload size. Recorded rather than quietly swapped.

## What changed

`ipc_send` and `ipc_recv` — the two hot paths — now call `ipc_copy32`:
4 instructions (2 loads, 2 stores) replacing 4 × (8-byte load + store) plus loop
control. Half the memory operations, no branch.

`ipc_endpoint_init` deliberately keeps its plain loop: it **zeroes** rather than
copies, runs once per endpoint, and giving it a vector path would add a second
place to keep in step with `IPC_MSG_WORDS` for no measurable gain.

## Verification

**The assert was watched firing**, not assumed: changing `IPC_MSG_WORDS` to 8
fails the build with `ipc_copy32 hard-codes 32 bytes; IPC_MSG_WORDS changed`.

**The path was proven to execute**, not merely to compile. The boot IPC demo
round-trips a payload through the new assembly:

```
[recv] received: 0xFEEDFACECAFEBEEF 0x0102030405060708
[recv] send with recv-only cap correctly DENIED
```

Both words arrive intact, and the capability refusal still works — so the copy
is correct and it did not disturb the authorisation path around it.

Gates green: `smoke`, `smoke-user`. Zero warnings under `-Werror`.

Static cost (item 44 convention): 4 instructions, 32 bytes moved, no stack
traffic, no branches, no CPUID gate, no AVX state touched.

**Group 8 is now complete — items 42, 43, 44 and 45 all shipped.**

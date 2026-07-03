# DDR-SMP-3a — SWAPGS discipline + GS-based percpu (ADR-030 stage 3a)

> DDR before code. Stage 3 (APs scheduling) needs fast per-CPU state from any
> context — `%gs`-relative — which DDR-SMP-2 deferred pending SWAPGS. Stage 3
> splits: **3a (this slice)** = SWAPGS on every ring transition + `this_cpu()`
> over `%gs:0`; **3b** = per-CPU TSS/kstack/current; **3c** = scheduler ring
> lock + APs run kernel threads.

## Decisions

### D1 — The convention (Intel SDM SWAPGS semantics)
`IA32_GS_BASE` (0xC0000101) holds the **active** base; `IA32_KERNEL_GS_BASE`
(0xC0000102) the inactive one; `swapgs` exchanges them. Convention: **in kernel,
GS base = this CPU's `struct percpu`; in user, that pointer sits parked in
KERNEL_GS_BASE** (user GS base is whatever the user set — irrelevant to us).
`percpu_init_cpu` writes `IA32_GS_BASE` directly (we are in kernel when it
runs); the DDR-713 rule holds — it runs before any user thread exists (BSP: at
APIC init, before the FS section spawns users; APs: in `smp_ap_entry`).

### D2 — The four transition sites (complete enumeration)
The `gs`-write audit (DDR-SMP-2) found no other GS touches; the ring-transition
sites are exactly:
1. **`syscall_entry.asm`** — always from CPL 3: unconditional `swapgs` first
   instruction after entry; unconditional `swapgs` immediately before `sysretq`
   (both the success path and the error path share the single exit).
2. **`isr.asm` `isr_common`** — may interrupt kernel or user: on entry, test
   the pushed frame's CS RPL (`[rsp + regs.cs] & 3`); `swapgs` iff from CPL 3.
   Mirror test before the `iretq`. A thread that context-switches away inside
   the ISR (sched_tick) resumes later at the same point with the same frame, so
   the entry/exit pairing is per-frame and stays balanced.
3. **`usermode.asm` `enter_user_mode`** — kernel → ring 3 via IRETQ (first
   entry, execve, fork-child resume): unconditional `swapgs` before `iretq`.
4. **`usermode.asm` `signal_sigreturn`** — restores a ring-3 frame via IRETQ:
   unconditional `swapgs` before `iretq`.
Paths that die in-kernel (user-kill via `sched_exit` inside an ISR) never
return through an exit site; kernel GS stays active for the next thread —
correct, since GS tracks the CPU, not the thread.

### D3 — `this_cpu()` over `%gs:0`
`struct percpu` gains `self` as its **first** member (set at init);
`this_cpu()` becomes a single `mov %gs:0, %rax` — safe now in any kernel
context on any CPU. The LAPIC-ID scan remains only inside `percpu_init_bsp`
(to find the roster index). Ring 3 reloading its `gs` selector no longer
matters: the kernel swaps to its own base at every entry (the stage-2 hazard is
closed properly).

### D4 — Regression surface
Every one of the 54 gates hammers SYSCALL + IRQ + user transitions, so the
existing suite is the SWAPGS-imbalance detector (an imbalance faults on the
first `%gs:0` access or corrupts user GS visibly). New gate `smoke-swapgs`
additionally asserts `this_cpu()` identity **from inside a syscall entered from
ring 3** — the path that only works when the swap discipline is right: the
existing `[percpu] bsp` print moves its verification into `sys_getpid`'s path?
No — a dedicated debug-free check: `smp_ap_entry` already verifies APs; the BSP
re-verifies `this_cpu()` inside `lapic_timer_100hz`'s tick context via the
timer ISR? Simplest honest probe: `sys_getpid` (hot, called by every user
program early) asserts once `this_cpu() != NULL` and prints
`[percpu] gs OK (syscall ctx)` on its first call — a from-ring-3 syscall
context read through `%gs:0`. Gate greps that plus the stage-2 sentinels.

## Gate
`smoke-swapgs` (`-smp 4`): `[percpu] gs OK (syscall ctx)` + the stage-2
`percpu OK` lines still green (the AP path now also reads via `%gs:0`).
55 CI gates total.

## Non-goals
Per-CPU TSS/kstack/current (3b); scheduler lock + AP scheduling (3c); x2APIC;
FSGSBASE instructions (CR4.FSGSBASE stays off — user cannot write GS base).

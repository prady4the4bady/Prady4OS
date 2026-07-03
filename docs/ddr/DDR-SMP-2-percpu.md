# DDR-SMP-2 — Per-CPU identity area (ADR-030 stage 2)

> DDR before code. Stage 3 (scheduling on APs) needs "which CPU am I, and where
> is my state" answerable from any context. This slice adds that foundation.

## Decisions

### D1 — LAPIC-ID-indexed percpu, not GS-based (yet)
`kernel/apic/percpu.{c,h}`: a `struct percpu { cpu_idx, apic_id }` array
(`PERCPU_MAX = 16`), populated at init; `this_cpu()` resolves the caller's entry
by reading the **LAPIC ID register** and scanning the (tiny) array.

Why not `%gs`-based now: the audit shows only one `gs` selector write
(boot `cpu.asm`), so an MSR-set GS base *would* survive normal ring
transitions — but ring 3 can legally reload its `gs` selector (user-data
descriptor, DPL 3), zeroing the base and breaking a swapgs-less kernel's
`%gs:0` on the next syscall. That would let user code break the kernel,
violating the ADR-021 isolation contract. Proper SWAPGS discipline belongs to
stage 3's syscall/interrupt-path rework; until then the uncached LAPIC-ID read
is safe, correct, and fast enough for stage-2/3 bring-up uses.

### D2 — Population and verification
`percpu_init_cpu(idx)` records `{idx, lapic_id()}` for the calling CPU: the BSP
right after `lapic_init` (idx 0's roster slot), each AP in `smp_ap_entry`.
Both then verify round-trip identity via `this_cpu()`:
BSP prints `[percpu] bsp idx=<i> id=<id>`; each AP prints
`[smp] cpu <idx> percpu OK`. Fields beyond identity (per-CPU TSS, kstack,
current) arrive with the stage that uses them — no dead members (repo rule).

## Gate
`smoke-percpu` (`-smp 4`): BSP line + all three AP `percpu OK` lines +
`cpus online=4/4`. 54 CI gates total.

## Non-goals
GS base + SWAPGS (stage 3); per-CPU TSS/GDT/kstack (stage 3); per-CPU
`current_thread` (stage 3, with the scheduler-ring lock).

# DDR-SMP-3c-cap-1 — per-CPU TSS / GDT descriptor / TR (ADR-031 D1)

> DDR before code. First sub-slice of the ADR-031 capstone. Contract-neutral:
> no scheduling change, BSP path unchanged (it is CPU 0); each AP loads and
> proves its own TSS at boot, the way stage-1 proved the subsystem locks.

## Why per-CPU
The TSS supplies **RSP0** — the stack the CPU switches to on a CPL3→CPL0 entry.
`TR` is per-logical-processor (SDM vol. 3 §7.2.1), and each CPU consults *its
own* TSS.RSP0. Today there is one global `tss`, one GDT descriptor at `0x28`,
one `LTR`. Two CPUs running ring-3 threads (cap-4) would read one shared RSP0 —
corruption. And two CPUs cannot `LTR` the same descriptor: the second sees the
busy bit set by the first → #GP. So each CPU needs its own TSS **and** its own
GDT TSS-descriptor slot.

## Decisions
- **D1 — GDT grows to `PERCPU_MAX` TSS descriptors.** `cpu.asm`'s single
  `gdt64_tss` slot (0x28) becomes 16 back-to-back 16-byte descriptors; the GDT
  limit auto-extends. CPU `i`'s TSS selector is `0x28 + i*0x10`. The BSP still
  uses `0x28`.
- **D2 — `tss[]` indexed by `cpu_idx`.** `tss_init_cpu(idx, rsp0)` fills
  `tss[idx]`, patches descriptor `idx`, and `LTR`s `0x28 + idx*0x10`.
  `tss_set_rsp0` writes `tss[this_cpu()->cpu_idx].rsp0` — the running CPU's own.
- **D3 — APs must be on the shared `gdt64` before `LTR`.** APs run on the 3-entry
  trampoline GDT (no TSS descriptors). `smp_ap_entry` now calls `gdt_init` FIRST
  (loads `gdt64`, reloads CS/DS/…/GS — zeroing the GS base, which is fine
  *before* identity is set), THEN `percpu_init_cpu` (wrmsr restores the per-CPU
  GS base), THEN `tss_init_cpu(idx, ap_stack_top)`. This ordering mirrors the
  BSP's `gdt_init → percpu_init_early` sequence (main.c:1069→1075). `gdt64` also
  carries the ring-3 user segments APs will need in cap-4.
- **D4 — the BSP migration re-homes the TSS.** `percpu_init_bsp` may move the
  BSP from slot 0 to its MADT roster index `ridx`, changing `cpu_idx`. Its `TR`
  was `LTR`'d as `0x28` (TSS[0]) at boot, so after migration `tss_set_rsp0`
  (indexing `cpu_idx=ridx`) would write `tss[ridx]` while `TR` still reads
  `tss[0]` → RSP0 writes miss the live TSS → ring-3 entry crash. The migration
  branch therefore re-homes: `tss_init_cpu(ridx, tss[0].rsp0)`, re-`LTR`ing the
  BSP onto its `ridx` TSS so `TR` and `cpu_idx` agree. (On QEMU the BSP is
  roster index 0, so this branch is dormant — but correctness must not depend on
  it.)

## Proof / gate
`smp_ap_entry` reads back `str`/its identity and prints `[smp] cpu N tss OK`
(TR nonzero and selector == `0x28 + N*0x10`) before parking. No new gate file;
the existing `smoke-smp`/`smoke-smplock`/`smoke-percpu` (`-smp 4`) boot all CPUs
and now assert the line via their EXTRA_SENTINELs. RSP0 stays unused until a
ring-3 thread runs on an AP (cap-4), so no runtime behavior changes here.

## Non-goals
Idle threads, APs entering `schedule()` (cap-2); preemption (cap-3); ring-3 on
APs (cap-4). RSP0 content beyond a placeholder AP stack.

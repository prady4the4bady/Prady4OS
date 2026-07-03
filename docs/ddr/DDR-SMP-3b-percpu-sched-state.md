# DDR-SMP-3b — current_thread + syscall kstack move into percpu (ADR-030 stage 3b)

> DDR before code. With SWAPGS live (3a), the scheduler's per-CPU state can move
> off globals: `current_thread` and the SYSCALL kernel-stack top become
> `%gs`-relative, so each CPU tracks its own running thread — the last
> prerequisite before 3c lets APs schedule.

## Decisions

### D1 — Fixed percpu layout for the asm
`struct percpu`: `self` @0, **`current` @8, `kstack_top` @16** (compile-time
asserted), then identity fields. `syscall_entry.asm`'s stack switch becomes
`mov rsp, [gs:16]` (the `swapgs` is already the first instruction, so the
kernel base is active). The `syscall_kstack_top` global and its extern are
deleted.

### D2 — `current_thread` becomes a macro
`sched.h`: `#define current_thread (this_cpu()->current)`. Every one of the
dozens of read sites and the 3 write sites (sched init, `schedule()`'s
switch-in path ×2) compile unchanged — but now resolve per-CPU. The global
definition is deleted.

### D3 — Early BSP init (the DDR-713 lesson, again)
The scheduler runs from the first PIT tick — long before APIC/ACPI init — so
`%gs`-based `current_thread` must be valid from the top of kmain:
`percpu_init_early()` claims slot 0 for the BSP (identity fields zeroed) and
loads `IA32_GS_BASE` before the IDT/PIC section. Later, `percpu_init_bsp`
fills in the LAPIC id and, if the BSP's MADT roster index isn't 0, **migrates**
the entry to its roster slot (copy, re-point GS, clear the old) so an AP whose
roster index is 0 can never collide (QEMU: BSP is 0, migration is a no-op).
`smp_ap_entry` now runs `percpu_init_cpu` first.

### D4 — Probe
The `sys_getpid` probe additionally verifies `this_cpu()->current->pid` matches
the returned pid from ring-3 syscall context: `[percpu] current OK (syscall
ctx)`. Beyond that, **every** gate exercises the moved state: each syscall
switches stacks via `gs:16`, each tick reschedules via `gs:8`.

## Gate
`smoke-percpu-sched` (`-smp 4`): the `current OK` probe + the 3a/2 sentinels
still green. 56 CI gates total.

## Non-goals
Per-CPU TSS RSP0 + per-CPU idle threads + the ring lock and reschedule IPIs —
stage 3c, where APs actually schedule.

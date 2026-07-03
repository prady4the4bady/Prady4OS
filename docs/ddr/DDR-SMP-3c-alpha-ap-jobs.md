# DDR-SMP-3c-alpha — AP wake IPI + work dispatch (ADR-030 stage 3c, first cut)

> DDR before code. Full 3c (APs inside the scheduler) needs per-CPU TSS, the
> ring lock, and preemption IPIs. This first cut delivers **real multi-core
> execution** without touching the scheduler: the BSP dispatches kernel
> functions to APs through a per-CPU mailbox + a wake IPI.

## Decisions

### D1 — Wake IPI on vector 49
`isr.asm` grows one stub (49, no error code; table → 50) and `idt.c` installs
it. The handler EOIs the LAPIC and returns — its only job is to break the AP
out of `hlt`. It cannot collide with anything: 32–47 are PIC lines (BSP-only),
48 is the APIC timer (armed only on the BSP).

### D2 — AP idle: LAPIC on, `hlt` with IF set, mailbox check per wake
`smp_ap_entry`, after its checks, software-enables this AP's LAPIC (same MMIO
window — the LAPIC is per-CPU at the same physical address), runs `sti`, and
loops: `hlt` → drain the mailbox → `hlt`. A CPL0→CPL0 interrupt uses the
current (trampoline-provided) stack — no TSS needed. The DDR-SMP-3a ISR swap
logic is CPL-conditional, so an AP interrupt (from CPL 0) correctly leaves the
AP's kernel GS in place. Known deferral (pre-existing since stage A on the
BSP): the spurious vector (0xFF) has no IDT gate; QEMU never delivers spurious
interrupts, and the real-hardware fix (a 0x?F-aligned gate) is a stage-3c
follow-up noted here.

### D3 — The mailbox + dispatch API
`struct percpu` gains `void (*job)(void)` — a single-slot mailbox (fields are
used immediately; the stage rule). `smp_run_on(cpu_idx, fn)` (in `smp.c`)
stores the pointer (release), sends the fixed-delivery IPI (vector 49) to that
AP, and returns; the AP clears the slot (acquire/release) after running the
function. `smp_job_done(cpu_idx)` polls completion. Single-producer (BSP) /
single-consumer (that AP) — no lock needed beyond the atomics.

### D4 — Proof at boot
After bring-up, kmain dispatches a test job to every AP; the job prints
`[smp] cpu <idx> job OK` (console lock is cross-CPU since stage 1) and the BSP
waits for all mailboxes to drain, printing `[smp] jobs done=<n>`.

### D5 — Root-cause fix folded in: APs must load the kernel IDT
The first gate run halted the whole machine at `online=4/4`: the wake IPI
triple-faulted the AP because the trampoline reaches long mode with the
**real-mode IDTR leftover** — the first interrupt on an AP dereferenced a
garbage descriptor. Fix: `idt_load_ap()` (loads the already-built kernel IDT)
runs in `smp_ap_entry` before the LAPIC is enabled and `sti`.

## Gate
`smoke-smpjob` (`-smp 4`): all three `job OK` lines + `jobs done=3`. 57 CI
gates total.

## Non-goals
APs in the scheduler (full 3c: per-CPU TSS/idle, ring lock, preemption IPIs);
multi-slot job queues; job arguments/results; spurious-vector gate.

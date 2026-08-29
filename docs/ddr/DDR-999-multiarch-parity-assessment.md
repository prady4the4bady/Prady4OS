# DDR-999 — can aarch64 / riscv64 / Apple reach feature parity by 2026-08-31?

**Status:** ASSESSMENT. Requested by the operator directive of 2026-08-29 §4,
which asks for a plain answer before any further x86_64-only work.
**Answer: no — and not by a small margin.** §5 says what IS achievable.

---

## 1. The question, stated so the answer can be checked

"Meaningful feature parity" was given three example rungs: **booting to a
shell**, **running the compositor**, or **remaining boot-only**. The window is
~2.5 days (this is written 2026-08-29 06:0x UTC; the deadline is 2026-08-31
23:59 UTC), and that same window also has to carry the x86_64 release, which is
not finished — `v1.0.0` is untagged, OPEN-1 is undecided (DDR-1000 decides it),
and `smoke-nethammer` has an open CI-only intermittent.

## 2. What exists today, measured

```
kernel/arch/aarch64/   3 files   115 lines   (boot.S, start.c, kernel.ld)
kernel/arch/riscv64/   3 files    89 lines
kernel/  (the x86_64 kernel)   172 files  27,217 lines
```

Both non-x86 kernels build clean right now (`make kernel-aarch64`,
`make kernel-riscv64` — verified while writing this). What they do is: reach C,
bring up one UART, print `NEXUS KERNEL OK`, and `wfe`/`wfi` forever. There is no
MMU, no physical allocator, no scheduler, no VFS, no syscall surface, no driver
of any kind, and no userspace.

The 204 lines are not a partial port. They are a boot slice, exactly as ADR-034
decision 3 says, and that ADR was explicit that it "does **not** deliver the
ported smoke-gate set."

## 3. What parity would actually require

ADR-034 **decision 2** is the load-bearing fact here: the new ports are
*additive* and "**nothing in the x86_64 path is touched**". There is no
architecture abstraction layer. The hoist was deliberately deferred until BUG-1
closed, precisely so a red gate could not be ambiguous between the bug and the
move.

So parity starts with that hoist, across a tree where:

* **50 of 172 files** contain x86-only constructs — port I/O (`inb`/`outb`),
  MSRs (`rdmsr`/`wrmsr`), `cpuid`, descriptor-table loads, `rdtsc`.
* **25 files** reference LAPIC / IOAPIC / MSI-X.
* 2 `.asm` files are nasm, which ADR-034 decision 1 already records as x86-only.

Then, per architecture, from nothing:

| Subsystem | x86_64 today | aarch64 needs | riscv64 needs |
|---|---|---|---|
| Paging | CR3, 4-level, COW, W^X, `PTE_SW_SHARED` | TTBR0/1, MAIR, TCR | Sv39/Sv48, `satp` |
| Interrupts | LAPIC + IOAPIC + MSI-X | GICv2/v3 | PLIC + CLINT |
| Timer | LAPIC timer (the scheduler tick rests on it) | generic timer (CNTP) | SBI timer |
| Syscall entry | `SYSCALL`/`SYSRET`, `MSR_SFMASK`, TSS | `SVC`/`ERET`, DAIF | `ECALL`/`SRET`, `sstatus.SIE` |
| SMP bring-up | INIT–SIPI–SIPI | PSCI | SBI HSM |
| virtio transport | virtio-**pci** | virtio-**mmio** | virtio-**mmio** |
| Userspace | ELF + musl, x86_64 ABI | aarch64 ABI | riscv64 ABI |

Then 156 gates ported.

The syscall-entry row deserves a specific note, because it is the one place this
project has already paid for the lesson: **DDR-981** cost a multi-day
investigation to find that `SYSCALL` clears `RFLAGS.IF` via `MSR_SFMASK` and the
entry path never restores it, so every ring-3 yield-spin ran with interrupts
masked. DAIF and `sstatus.SIE` have exactly analogous traps, and none of that
knowledge transfers as code — it would be re-derived, on each architecture, from
scratch.

## 4. The iteration-cost multiplier — measured, not assumed

`qemu-system-aarch64` and `qemu-system-riscv64` are **not installed in this
environment**; `make smoke-aarch64` and `make smoke-riscv64` both fail with
"not installed. (CI installs it)". Verified just now.

So every non-x86 iteration costs a **CI round trip** — the 10-shard matrix
makespan is ~20 min — against ~30 s for a local x86_64 boot. That is roughly a
**40× slower debug loop**, on the architectures where nothing is known to work
yet and therefore where the loop matters most. Any estimate that ignores this is
wrong by more than the estimate itself.

## 5. The answer

**aarch64 / riscv64 — booting to a shell or running the compositor: NOT
ACHIEVABLE by 2026-08-31.** Each is a multi-week port of paging, interrupt
controller, timer, syscall entry, SMP bring-up, and a second virtio transport,
on top of an abstraction layer that does not exist yet, debugged through a 40×
slower loop, while the same 2.5 days must also land the x86_64 release.

**Apple Silicon — NOT ACHIEVABLE, and not merely on time.** m1n1 is already in
the pre-approved deferred list. There is no CI path at all: QEMU does not model
the M-series hardware this would target, so there would be no way to test it
even with unlimited time in this window. This one is not a schedule problem.

**Recommendation: aarch64 and riscv64 stay boot-only for 1.0**, which is what
ADR-034 scoped and what the tracker already records. The Group H ISO items for
them are *packaging* of the boot-only kernel and remain achievable.

## 6. What IS achievable in the window, if non-x86 work is wanted

Ranked by cost, each a real increment over the boot slice rather than a
restructure:

1. **aarch64/riscv64 ISO packaging** (already in Group H) — U-Boot / OpenSBI
   wrapping of the existing boot-only kernel. Hours, and the deliverable is a
   bootable image.
2. **Exception vectors + a timer tick, per arch** — ~200–400 lines each, gateable
   as "N ticks observed". A genuine step toward the scheduler, and it retires the
   riskiest unknown (interrupt delivery) first.
3. **DTB parse + a bump allocator** — gives a real memory map instead of the
   hard-coded RAM bases ADR-034 lists as platform facts.

None of these is a shell. Offering them as one would be the "green tick against
work that does not exist" ADR-034 already refused to give.

## 7. What I am NOT claiming

I have not attempted the port and failed; this is an estimate from a measured
starting position, and estimates can be wrong. What is *not* an estimate: the
204 lines, the 50/172 x86-coupled files, the absent abstraction layer (ADR-034
decision 2, in writing), and the missing local QEMU binaries. Those are facts,
and they are what the conclusion rests on.

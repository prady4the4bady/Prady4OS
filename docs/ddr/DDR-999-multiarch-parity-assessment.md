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

---

## 8. CORRECTION to §6.1 — "ISO packaging, hours" was wrong

§6 ranked **"aarch64/riscv64 ISO packaging (already in Group H) — U-Boot /
OpenSBI wrapping of the existing boot-only kernel. Hours, and the deliverable is
a bootable image"** as the cheapest achievable non-x86 increment.

That estimate does not survive contact with the code, and it is corrected here
rather than left to be discovered by whoever tries it on the last day.

### 8.1 Both arch kernels are `-kernel` payloads, not bootable images

`kernel/arch/aarch64/boot.S` says so in its own header:

> "QEMU `-machine virt -kernel <ELF>` honours the ELF entry point and **enters
> with the MMU off**. Everything here is the minimum to reach C safely: park
> every core but the primary, install a stack, jump."

So the entry contract is *QEMU's ELF loader*. There is **no PE/COFF header and no
EFI stub** in either arch. `kernel/arch/riscv64/` has the same three files and
the same shape.

An ISO is not a kernel image. For firmware to boot one it must carry an EFI
System Partition holding an **EFI application**, and neither kernel is one.
Wrapping these ELFs in an ISO produces a file that no firmware will start.

### 8.2 The tooling isn't there either, and the x86 path does not generalise

Measured in this container: `/usr/lib/grub/` is absent, and there is no
`qemu-efi-aarch64` / AAVMF firmware. Note also that the **x86 ISO does not use
GRUB** — `make iso` is `xorriso -as mkisofs -b boot/pradyos.img -hard-disk-boot
-eltorito-alt-boot -e boot/esp.img`, i.e. El Torito over a raw disk image plus a
prebuilt ESP. That recipe works because x86 firmware boots raw images and the
repo already builds an ESP. Neither premise holds on aarch64 or riscv64.

### 8.3 The corrected estimate

Making these bootable as ISOs requires, per arch: a PE/COFF-wrapped EFI stub
entry, an ESP built around it, and EDK2/U-Boot firmware to test against. **That
is a port task, not packaging** — the same category §5 already ruled out for the
window, arrived at from the other direction.

### 8.4 What IS verified, and it is not nothing

Both boot gates pass, now measured **locally** rather than only in CI —
`qemu-system-aarch64` and `qemu-system-riscv64` were installed in this container
for the purpose:

```
=== smoke-aarch64 ===  PASS   PRADYOS BOOT OK / NEXUS KERNEL OK
=== smoke-riscv64 ===  PASS   [riscv64] PASS — saw 'NEXUS KERNEL OK'
```

So the boot-only slice is real and reproducible off a CI runner. What is wrong is
only the claim that turning it into a bootable ISO is hours of packaging.

### 8.5 Why this correction exists

§7 of this file is titled "What I am NOT claiming". §6.1 quietly claimed
something anyway — a deliverable ("a bootable image") and a cost ("hours") that
nobody had checked against `boot.S`. It is exactly the kind of estimate that
consumes a deadline's last day, which is why it is corrected in the file that
made it rather than in a new one.

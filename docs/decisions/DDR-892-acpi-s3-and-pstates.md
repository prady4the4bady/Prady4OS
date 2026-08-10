= DDR-892 — ACPI S3 discovery + CPU frequency scaling (Group 4 item 27)

**Status:** Accepted — discovery and P-states shipped; **S3 ENTRY deferred**,
with the reason and the path recorded
**Date:** 2026-08-10
**Scope:** `kernel/acpi/acpi.{c,h}`, `kernel/arch/x86_64/pstate.{c,h}`,
`kernel/main.c`, `smoke-power`.

## 1. The item has two halves and they carry very different risk

**CPU frequency scaling** is self-contained: read a capability bit, read and write
one MSR pair, refuse when unsupported. It ships complete.

**S3 suspend-to-RAM is a boot path.** On resume the firmware re-enters the
machine **in real mode** at `FACS.firmware_waking_vector`. Everything the
bootloader established — long mode, CR3, GDT, IDT, TSS, per-CPU MSRs — is gone
and must be rebuilt before a single line of C can run, and only then can saved
state be restored. That is a third implementation of the handoff DDR-886 §1
warns about, and it has a failure mode the other two do not: **if it is wrong,
the machine never comes back.**

## 2. What ships for S3: discovery, and a refusal that cannot hang

`\_S3_` is parsed exactly as `\_S5_` already is (the scanner is shared rather
than duplicated), giving `SLP_TYPa/b` for S3 and `acpi_s3_available()`.

`acpi_suspend_s3()` exists and **refuses**:

```
[acpi] S3 refused: no resume path (waking vector unset)
```

It refuses because entering S3 without a resume trampoline is not a bug that
produces a wrong answer — it is a machine that powers down its CPU and never
returns. In a gate that is an indistinguishable-from-hung QEMU; on hardware it is
a box that needs the power button. **A capability that would brick the run is not
something to leave enabled and hope the caller knows better.**

The refusal is not a stub standing in for the feature. The S3 *discovery* is real
and gated; the *entry* is deliberately closed until the resume path exists.

## 3. The path, so the next session does not re-derive it

`arch/x86_64/ap_boot.asm` **already does the hard part**: it takes a CPU from
real mode to long mode with our CR3 and GDT, because that is what SMP AP bring-up
requires. The S3 resume trampoline is that same code with a different tail —
instead of entering the scheduler it restores a saved register file and returns
to the suspend point.

What must be added: a low-memory trampoline copy (the AP path already places
one), `FACS.firmware_waking_vector` pointing at it, a saved-state block (GPRs,
CR0/CR3/CR4, EFER, the per-CPU MSRs listed in the AP-bring-up notes — every one
of those must be re-armed per CPU or resume faults exactly as a mis-initialised
AP does), and a QEMU gate using `-global ICH9-LPC.disable_s3=0`.

## 4. CPU frequency scaling

`IA32_PERF_CTL` (0x199) / `IA32_PERF_STATUS` (0x198), gated on **CPUID.01H:ECX
bit 7 (EIST)**.

The capability check is not optional politeness: `wrmsr` to an unimplemented MSR
raises `#GP`, so a blind write on a CPU without EIST is a kernel fault at boot.
`cpu_mitigations.c` already establishes this pattern in this codebase — check the
bit, then write.

Under QEMU/TCG, EIST is **not** advertised. So the path this build actually
exercises is the refusal, and the gate asserts that:

```
[pstate] EIST unsupported; frequency scaling unavailable
```

That is the honest outcome to gate here. Reporting a "current frequency" derived
from an MSR the CPU does not implement would be inventing a number — and a
scaling driver that appears to work while changing nothing is worse than one that
says it cannot.

## 5. Scope, stated plainly

- **Complete:** `\_S3_` discovery, `acpi_s3_available()`, EIST detection,
  `IA32_PERF_CTL` read/write behind that check, and the refusal paths for both.
- **NOT complete: S3 entry and resume.** Item 27 is therefore **partially
  delivered**, and this DDR says so rather than counting discovery as the
  feature. The remaining work is a resume trampoline built on `ap_boot.asm`.

Calling item 27 done on discovery alone would be the scope absorption this queue
asks me to name out loud.

---

## 6. What the build found

**The report ran before the scan.** `acpi_s3_available()` was first called right
after `numa_init()`, but the DSDT is scanned by `acpi_power_init()` **31 lines
later**. The boot printed `S3 not advertised` and `no _S3_ in DSDT` for a machine
whose `_S3_` the scanner had in fact parsed correctly.

The raw-occurrence counter is what exposed it:

```
[acpi] DSDT _S3_ occurrences=1 parsed=1     <- printed AFTER the S3 report
[acpi] S3 not advertised                     <- read before the scan ran
```

Two conclusions with opposite fixes — "the scanner is broken" and "the report ran
too early" — and one bit of output could not tell them apart. The counter is kept
permanently for that reason, and the gate asserts it alongside `parsed=1`.

**`QEMU_S3` was removed after being added.** It was wired on the assumption that
QEMU's ICH9 omits `_S3_` unless `disable_s3=0`. Measured: the DSDT carries `_S3_`
either way (the property affects FADT flags, not the object). A knob that changes
nothing is dead configuration, so it is gone rather than left in place looking
meaningful.

## 7. Mutation matrix

| Mutation | Applied? | Result |
|---|---|---|
| scanner accepts only `_S5_`, ignoring `_S3_` | verified yes | **killed** |
| claim EIST without checking CPUID | verified yes | **killed** |

M2 matters: claiming EIST makes the refusal line disappear and the code proceed
to `rdmsr` on an MSR this CPU does not implement. The gate catches the changed
report before the `#GP` would.

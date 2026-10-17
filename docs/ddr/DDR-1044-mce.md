# DDR-1044 — `#MC`: the machine check that was never delivered

**Status:** IMPLEMENTED + gated (`smoke-mce`, shard 4) + M1/M2/M3
**Group:** A — Kernel Completeness, the `#MC machine-check handler` row.

---

## §1 — The row was half right, and the half it named already existed

> | `#MC` machine-check handler | Panic with full register state | `smoke-mc` |

`idt.c` already knows vector 18 by name (`exc_name[18] = "#MC machine check"`)
and any CPL-0 exception below 32 already panics with a full register dump and a
frame-pointer backtrace. **The row's stated deliverable was already shipped.**

What was missing is upstream of it, and it is not a printing question.

---

## §2 — MEASURED FIRST: without `CR4.MCE` there is no exception at all

An MCE injected through QEMU's monitor into the kernel as it stood:

```
{"return": "CPU 0: MCE capability is not enabled, raising triple fault"}
```

and the serial log **stops mid-boot**:

```
[sfs] lz4+tags compress/readback/tag OK
<end of file>
```

No banner. No registers. No `#MC`. The machine simply dies. On real hardware
that is a box lost to a memory or cache fault with **zero diagnostic** — the
exact opposite of what the row asked for, and invisible because nothing had ever
tried to deliver one.

`CR4.MCE` (bit 6) is what makes the existing panic path *reachable* for vector
18. That is the feature.

---

## §3 — What was built

`cpu_enable_mce()` (`kernel/apic/smp.c`), SDM Vol.3 §15.8 order, each step
earning its place:

1. **`CPUID.1:EDX.MCE`** (bit 7) — required, or the `CR4` write `#GP`s.
2. **`CPUID.1:EDX.MCA`** (bit 14) — gates the MCG and MCi bank MSRs. Without it
   the banks do not exist and `rdmsr` on them `#GP`s, so a CPU with MCE but no
   MCA gets the `CR4` bit and no bank programming.
3. **`MCG_CTL` = all-ones**, if `MCG_CAP.CTL_P`.
4. **Per bank: `MCi_CTL` = all-ones, then `MCi_STATUS` = 0.** Clearing status is
   not tidiness — a stale `VAL` bit left by firmware would make the first `#MC`
   report a fault that happened before this kernel booted.
5. **`CR4.MCE` last**, so nothing can be delivered before the banks are sane.
   Read back, never assumed.

**Per-CPU.** `CR4` and every `MCi_*` MSR are per-logical-processor, so every AP
runs it too, beside `cpu_enable_sse` / `cpu_enable_smep` / `cpu_enable_smap`.

And the decode, in the `idt.c` panic path for vector 18 only:

```
MCE: mcg_status=0x0000000000000005 ripv=1 eipv=0 mcip=1
MCE: bank=0 status=0xBD80000000000000 addr=0x0000000000001234 misc=0x000000000000008C
```

Printed **before** the register dump, because `MCG_STATUS.RIPV` is how a reader
knows whether to trust the `RIP` that follows. Bounded by `MCG_CAP`'s own bank
count, and only banks with `STATUS.VAL` are printed — an all-banks dump on a
32-bank CPU would bury the one line that matters under 31 zeros.

---

## §4 — The gate: `smoke-mce` (shard 4), and why it is unusual

**Its pass condition is a panic.** `*** NEXUS KERNEL PANIC ***` is in
`GLOBAL_FORBIDDEN` (DDR-1009), so this gate runs QEMU directly rather than
through `boot_test.sh`.

**Coverage is not dropped**, which matters because DDR-1010 closed exactly this
gap for `smoke-shell` and it must not be reopened:

- **arm E** asserts there is *exactly one* panic banner, so a second, unexpected
  death cannot hide behind the expected one;
- **arm F** runs the full 73-pattern global scan over a copy with that *single*
  expected line removed. Every other sentinel still applies.

| arm | assertion | what it would catch |
|---|---|---|
| A | `PRADYOS_MCE cpuid=1 cr4=1 banks=[1-9]` | CR4 read back set, and banks actually found — `banks=0` would leave arm D with nothing to walk, so it is asserted rather than discovered as a silent skip |
| B | `exception: #MC machine check  vector=0x…12` | the exception was **delivered**; without CR4.MCE this line cannot exist |
| C | `MCE: mcg_status=… ripv=1 eipv=0 mcip=1` | MCG_STATUS was read from hardware, not templated |
| D | `bank=0 status=0xBD80000000000000 addr=0x…1234 misc=0x…8C` | **the round-trip** — the injected values come back byte-exact |
| E | exactly one panic banner | see above |
| F | global scan over the filtered log | see above |

**Arm D is what makes the decode a measurement.** The injected record is fixed
and known, so a decode reading plausible numbers from the wrong MSR offsets
passes a shape check and fails this one — which is precisely what M3 does.

**Readiness is polled, not slept.** `mce_inject.py` waits for the guest's own
`PRADYOS_MCE cpuid=` line before injecting, because an injection before `CR4.MCE`
is set triple-faults and the gate would then report "no `#MC`" about a kernel
that never got the chance. A fixed sleep is a guess about boot time that is wrong
on a loaded runner in whichever direction hurts; DDR-910 made the same correction
for the pointer injector.

### §4.1 — 131 s → 13 s

The first version took **131 s**: after the panic the kernel halts, so QEMU never
exits and the run burned the whole window — against shard totals of ~1400 s. The
recipe now polls for the last line the gate needs and stops there, with `timeout`
still the hard ceiling. Same shape, and the same argument, as DDR-785's early
exit: the loop can only ever stop things *earlier*.

---

## §5 — Mutants, MEASURED

Clean kernel `ec90cc611e86c0c4`, 1,175,946 B.

| | mutation | kernel.bin | result |
|---|---|---|---|
| **M1** | `cr4 \|= (1<<6)` removed | `421a302ee622288d` | **rc=2, arm A** — and the injector's own reply is the diagnosis: `"MCE capability is not enabled, raising triple fault"` |
| **M2** | report `banks` as 0 (CR4 still set) | `327206a4ee104937` | **rc=2, arm A** — *not* arm D |
| **M3** | decode reads `MCi_CTL` (`0x400+4i`) instead of `MCi_STATUS` (`0x401+4i`) | `82a088c871e1aaef` | **rc=2, arm D ALONE** |

**M2 landed on arm A, not on D**, because arm A already asserts `banks=[1-9]`.
That left arm D unproven, so M3 was added: it keeps `banks` correct and breaks
only the decode. M3's log is worth reading —

```
MCE: bank=0 status=0xFFFFFFFFFFFFFFFF addr=0x…1234 misc=0x…8C
MCE: bank=1 status=0xFFFFFFFFFFFFFFFF addr=0x0 misc=0x0
```

`MCi_CTL` is all-ones, so `STATUS.VAL` reads set on **every** bank and the
"only report banks with VAL" filter becomes useless — nine spurious lines, and
the first one is plausible enough to pass any shape check. That is the concrete
case for asserting exact values.

---

## §6 — What this does not do

- **No recovery.** Every `#MC` is fatal here, including one with
  `MCG_STATUS.RIPV=1` that the SDM says is restartable. Recovery needs a
  poisoned-page policy and a way to kill just the affected process, and there is
  no fixup path in `idt.c` (DDR-1040 §4 built a one-shot latch, not
  `__ex_table`).
- **No CMCI, no polling of correctable banks.** Only the exception path exists;
  a correctable error that never raises `#MC` is still invisible.
- **No AP-side gate.** APs call `cpu_enable_mce()`, but the gate injects into
  CPU 0 only, so the AP path is compiled and unexercised.
- **QEMU 8.2.2 TCG only.** Unlike SMEP/SMAP, the default `qemu64` model *does*
  carry `mce`/`mca`, so no `-cpu` pinning is needed — but nothing here says how a
  physical CPU's bank encoding behaves.

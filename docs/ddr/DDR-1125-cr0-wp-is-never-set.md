# DDR-1125 — KERNEL W^X's READ-ONLY HALF IS NOT ENFORCED AGAINST RING 0: `CR0.WP` IS NEVER SET

**Status:** MEASURED. **NO FIX SHIPPED IN THIS COMMIT** — design first
(§NON-NEGOTIABLE 5), and CI is in flight on two heads so a kernel change would
stack (DDR-1107's rule: a red on a stacked tree cannot be attributed).
Docs-only. `kernel.bin` NOT rebuilt in the committed tree — `854bbb38fdfe4fd2`,
1,319,306 B, reverted **bit-for-bit and verified by rebuild**, so the
size/headroom pair and `ci-docstate-check` are unaffected. `GLOBAL_FORBIDDEN`
77, 179 gates, no new sentinel, no new gate.

**NOT an OPEN-2 finding, not an `[apfreeze]`, and no open issue moves**
(OPEN-1/2/12/13 untouched).

---

## §0 — How this was reached

Queue position 2 is *"remaining `PRE_LAUNCH_CHECKLIST` items"*. Auditing §4.4 —
the one row that names something as **permanently uncoverable** — led to
`vmm_protect_kernel`, and reading its preconditions found a different thing.

---

## §1 — THE FINDING, IN TWO ADJACENT LINES OF ONE CAPTURE

`vmm.c:34` states the **NX** precondition with care: it probes
`CPUID 8000_0001h EDX[20]`, gates on it, and explains exactly what getting it
wrong would cost (*"Enabling EFER.NXE on a CPU without NX would #GP, and setting
PTE bit 63 while NXE is clear faults with a reserved-bit error — so gate both on
this probe"*). Eleven lines later, for the other half of the same claim, it says:

> `NX bits gate on g_nx_ok (identical to user W^X); text RW-clearing is
> **unconditional**.`

The RW-clearing **is** unconditional in the page tables. Its **enforcement
against ring 0 is not** — that is `CR0.WP`, and this kernel never sets it,
never reads it, and never names it. The care is asymmetric between the two
halves of one claim.

**MEASURED, not reasoned** (temporary probe, reverted; kernel
`26c3c775ce18b895`), lines 28 and 29 of one `smoke-shell` capture:

```
[wx] kernel W^X OK
[wxprobe] cr0=0x0000000080000013 wp=0 ro_write=survived byte=0x60
```

`CR0 = PG|ET|MP|PE`. **Bit 16 (WP) = 0.** And the second line is not the same
witness twice: a **ring-0 write to a page the loop one line above had just
stamped `e &= ~VMM_RW`** completed **without a fault**. The audit reports
success, and the very next line shows the protection it verified being bypassed
by the ring the kernel runs in.

**Two independent witnesses** — the configuration bit, and the consequence —
which is this project's own standard (DDR-1099's RFLAGS *and* R15; DDR-1120's
three).

### §1.1 — The write witness is NOT vacuous, checked before it was believed

The probe byte is `__text_end`. Had `.rodata` been empty, `__text_end` would
land in the `else` branch that **keeps** RW, and the write would have succeeded
for a trivial reason. Measured: `__text_end = 0xffffffff80055000`,
`__rodata_end = 0xffffffff80142000` — **~970 KiB**, so the byte is squarely in
the `va < rodata_end` branch, which clears RW. The write is also of the **same
byte read back**, so memory is bit-identical in the surviving case.

And `survived` discriminates: with WP set, the write faults in ring 0,
`isr_dispatch` panics, and the text after it never prints.

---

## §2 — NOTHING SETS IT, NOTHING READS IT

Exhaustive grep for `cr0` over `kernel/`, `boot/` and `arch/` returns four
writers and not one touches bit 16:

| site | what it does |
|---|---|
| `stage2.asm:102/104`, `:178/180` | `or eax,1` — CR0.PE |
| `stage2.asm:573-575` | `or eax,1<<31` — CR0.PG |
| `arch/x86_64/ap_boot.asm:52-54` | `or eax,0x80000001` — PG\|PE, on an AP out of reset |
| `cpu_mitigations.c:55-58` | read-modify-write clearing EM, setting MP |

Every one is a read-modify-write or an `or` that **preserves** whatever was
already there. The architectural reset value of CR0 is `0x60000010`, in which
bit 16 is **clear**. And `grep -rniE 'cr0\.wp|write[- ]protect bit|WP=0'` over
`docs/` and the root Markdown returns **zero** — this has never been recorded.

---

## §3 — WHAT IT COSTS, AND WHAT IT DOES NOT

### §3.1 — The NX half holds; the RW half does not

NX enforcement is `EFER.NXE` plus PTE bit 63 and is **independent of WP**, so
DDR-757's and DDR-1046's execute-side claims stand: execute-via-alias is
genuinely closed. DDR-1046's summary — *"RO+NX removes the
writable-executable-kernel primitive"* — is therefore **half delivered**: the
**executable** half by NX, and the **writable** half not at all, because the RO
it rests on is advisory for CPL 0.

DDR-1046 is **not withdrawn and not criticised**. Its walk, its stamping, its
read-back of the alias PDE, and its measurement that *nothing writes the kernel
image through a physical address* are all correct and all stand. What is missing
sits one level below the page tables, in the CPU configuration, and no check
that reads PTEs could have seen it — which is the same shape DDR-1046 itself
found (an audit that could not see its own case), one level down.

### §3.2 — On the CPU all 179 gates run, WP is the ONLY thing there was

From the same capture: `PRADYOS_SMEP cpuid=0 cr4=0` and
`PRADYOS_SMAP cpuid=0 cr4=0` — the default `qemu64` model advertises neither,
exactly as DDR-1040 measured. Those two are about **user** pages and would not
cover this anyway; the point is narrower and worth stating plainly: on the gate
CPU, **nothing else stands between ring 0 and a read-only kernel page.**

### §3.3 — The user-copy path is UNAFFECTED, measured rather than assumed

`vmm_user_range_ok(cr3, va, len, writable)` rejects on `!(e & VMM_RW)` in
**software** (`vmm.c`), so `copyout` to a COW or read-only user page returns
`-EFAULT` whatever WP is doing — and the shipped arm
**`[uaccess] copyout RO page EFAULT OK`** is present in the captures of **both**
binaries. DDR-1041 established by measurement (SMAP forced on, 19 gates) that
the kernel *never dereferences a raw user pointer anywhere else*. **So COW is
not silently defeated**, and this DDR does not claim it is.

---

## §4 — THE VALUE IS INHERITED, NOT ESTABLISHED, AND THE TWO BOOT PATHS DIFFER IN KIND

`stage2.asm` never touches bit 16, so the BIOS path inherits the architectural
reset value — **WP=0, by the ISA**. `boot/uefi/loader.c` contains **zero** CR0
references (`grep -ciE 'cr0'` -> 0), so the UEFI path inherits **firmware** CR0,
whose WP the UEFI specification does not pin.

So the kernel's memory-protection posture on this axis is **inherited from
whatever loaded it**, and the two arms of the *same ISO* — both asserted by
`smoke-iso-x86`, which checks that each boots and nothing about CR0 — need not
agree. §INV.13's class, in the form where the property is implemented in
**neither** path rather than twice.

**The UEFI path's actual CR0 is NOT measured here** and no claim is made about
it. Note what the fix would do to that question: setting WP in the kernel
**establishes** the value instead of inheriting it, which makes the divergence
moot rather than merely measured.

---

## §5 — THE EXPERIMENT: DOES THE KERNEL BOOT WITH `WP=1`?

DDR-1046's own precedent is to **measure before claiming a change is blocked**
(*"with RW cleared the boot is line-for-line normal"*). Same here.

**Site:** `cpu_enable_sse()` (`cpu_mitigations.c`), one line —
`cr0 |= (1ull << 16);`. That is the correct single site and it was chosen by
measurement, not convenience: it is called by the **BSP** at `main.c:4001` and
by **every AP** at `smp.c:276`, it already performs a CR0 read-modify-write, and
CR0 is per-CPU so both paths need it.

**Mutant `7e2f777be9cbfb7d`**, built warning-clean at `-Werror`, carrying a
confirmation print. **That print is not decoration**: without it *"boots clean"*
and *"the mutation never applied"* are the same observation — DDR-1097's
false-clean lesson, and §INV.10's trap (the hash moved, checked).

```
PRADYOS_WX_ALIAS present=1 rw=0 nx=1
[wx] kernel W^X OK
[wpmut] cr0=0x0000000080010013 wp=1
```

`0x80010013` against the shipped `0x80000013` — bit 16 set, the mutation took.
**The boot is clean.** The PTE stamping itself runs under WP=1 without faulting
(the master tables sit at `0x300000`, PD entry 1 — a *different* 2 MiB page from
the RO alias at `0x400000`, which DDR-1046 had already checked), and the boot
proceeds through PMM, kheap, NCS 11/11, uaccess 4/4, **`[vmm] COW fork
copy-on-write OK`**, `SHAREDPTE before=0 after=0`, ACPI, PCIe, virtio, lwIP,
AETHER, the scheduler, IPC, ring-3 and the filesystem.

**GATE VERDICT: `smoke-shell` `GATE_RC=0`** — *"PASS … clean, no panic"*, with
the full builtin set (redirect, pipes, erase, quoting, `wait`, `source`,
`audit`) and the 77-pattern global-forbidden scan clean.

**And the change is narrow by construction, which that capture also shows.**
Two `[trap] user #PF … err=0x7` lines fire in it — `WXVIOL.ELF` (line 186 says
so in advance: *"spawning W^X violator (expect a clean user-kill)"*) and
`METRIC.ELF` writing the read-only metric page. Both are **user-mode** writes to
read-only pages, which the CPU enforces for CPL 3 **regardless of WP**, so they
were never affected and still behave identically. **WP moves exactly one thing:
ring-0 writes.**

**What this measurement does and does not license.** It shows the fix is **not
blocked** by anything that boot exercises. It is **one gate on one CPU**
(`[apic] up id=0 cpus=1`) and **is not a regression set**: the SMP, block-layer,
compositor and mmap/COW-heavy gates are unmeasured, and AP coverage at `-smp 4`
is covered *by construction* (both callers) and **not by measurement**.

---

## §6 — THE GATE ARM, WITH ITS VACUITY CHECKED FIRST

Twenty-second time this is caught in design text rather than after shipping.

- **VACUOUS — "assert the kernel boots with WP set."** It boots either way;
  §5 measured exactly that.
- **WEAK — "assert `CR0` bit 16 reads 1."** That proves the bit is **set**, not
  that it is **enforced**; under TCG, enforcement is a property of the emulator.
  This is precisely DDR-1046 §2.1's own correction: *"nothing crashed"* cannot
  distinguish *"the alias is read-only"* from *"the write-protect never
  applied."*
- **DISCRIMINATING — the write must now FAULT.** Arm DDR-1040's
  `fault_expect_arm(lo, hi, resume)` latch around a ring-0 write to a page
  `vmm_protect_kernel` stamped RW-clear; require `fault_expect_taken()` to
  report a consumed `#PF`, and the kernel to survive and print.

**The pre-fix tree is the control and fails that arm** — the write *succeeds*
and the latch stays armed — so **no synthetic defect is needed** (the
DDR-1066/1067/1090 form). §1's `ro_write=survived` is that control, already
measured.

**The latch's preconditions hold at that point, checked in the capture rather
than assumed.** `fault_expect_arm` refuses with an AP online or IF set; the
capture puts `[wx]` at line 28 while `enabling interrupts (sti)` is line 35 and
`[smp] cpus online=1/1` is line 87 — IF still clear, no AP yet.

---

## §7 — NOT CLAIMED

- **NO FIX IS SHIPPED HERE**, and none is claimed. §5 measures that one gate
  boots; that is not a regression set and is not a decision to ship.
- **NO EXPLOIT, AND NO ARTEFACT OF AN ACTUAL STRAY KERNEL WRITE.** The only
  ring-0 write to a read-only kernel page ever observed is **the probe's own**.
  DDR-1046's measurement that nothing writes the image through the alias stands
  and is not contradicted.
- **NO DEFECT IS ALLEGED IN `vmm_protect_kernel`** — the walk, the per-section
  stamping, the alias read-back and the audit are all correct for what they do.
  What is absent is a CPU-configuration precondition one level below them.
- **DDR-757 and DDR-1046 are NOT WITHDRAWN.** §3.1 corrects the size of one
  claim; their NX half is unaffected and their measurements stand.
- **NOTHING IS CLAIMED ABOUT THE UEFI PATH'S ACTUAL CR0** — unmeasured (§4).
- **COW IS NOT SILENTLY DEFEATED** (§3.3), measured on both binaries.
- **NO RATE**, no campaign, and this is **not** OPEN-2: no CPU froze, no panic
  occurred, and no open issue moves.
- **`kernel.bin` reverted BIT-FOR-BIT to `854bbb38fdfe4fd2`, verified by
  rebuild, not assumed**; the working tree carries this file only.

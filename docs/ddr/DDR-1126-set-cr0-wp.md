# DDR-1126 — `CR0.WP` is set: kernel W^X's read-only half is now enforced against ring 0

**Date:** 2026-09-20
**Status:** SHIPPED — one line of kernel, one gate arm.
**Design:** DDR-1125 (committed first, NON-NEGOTIABLE 5).
**Authority:** operator approval on PR #17, comment `5745738830`, verified at the
primary source (`author_association: OWNER`, 2026-09-19T22:23:23Z) rather than
taken from the notification that relayed it.

---

## 0. What shipped, and what unblocked it

Two changes:

1. `kernel/arch/x86_64/cpu_mitigations.c` — `cr0 |= (1ull << 16)` in
   `cpu_enable_sse()`.
2. `Makefile` — one required sentinel on `smoke-wxkernel`, the **discriminating**
   arm DDR-1125 §6 designed.

DDR-1125 measured the defect and deliberately did not fix it, for two stated
reasons. Both are now spent:

* **NON-NEGOTIABLE 5 (design first).** DDR-1125 *is* the design, committed at
  `eba9cb3` before this code.
* **DDR-1107's stacking rule** — *"a red on a stacked tree cannot be
  attributed"*. CI was in flight on three heads. All six `pradyos-ci` suites
  (push + `pull_request` on `17a926d`, `8dd6083`, `eba9cb3`) came back
  **SUCCESS**, all on kernel `854bbb38fdfe4fd2`. The tree is clean to stack on.

---

## 1. The site, chosen by measurement rather than convenience

`cpu_enable_sse()` is:

* the **one function both paths already run** — the BSP at `main.c:4001` and
  every AP at `smp.c:276`, which matters because **CR0 is per-CPU**;
* already performing a CR0 read-modify-write, so the cost is **zero extra
  instructions**;
* **ordered before** `vmm_protect_kernel()` (`4001 < 4006`), so WP is live by the
  time anything is stamped read-only.

**Unconditional, unlike NX, and that asymmetry is the point of DDR-1125.** WP is
architectural on every x86 from the 486 onward: no CPUID feature bit, no MSR, no
enabling sequence — so there is nothing here to gate on. Contrast `vmm.c:34`,
which must probe `CPUID 8000_0001h EDX[20]` before touching `EFER.NXE` because
enabling NXE without NX would `#GP`. The file stated one precondition carefully
and had no precondition to state for the other; what it did instead was call the
RW-clearing *"unconditional"*, which is true of the page tables and was not true
of the enforcement.

---

## 2. The proof is DISCRIMINATION, and its vacuity was checked before it was written

DDR-1125 §6 checked the two obvious arms first and rejected both:

* *"assert the kernel boots with WP set"* — **VACUOUS**; it boots either way, and
  DDR-1125 §5 measured exactly that on the mutant.
* *"assert CR0 bit 16 reads 1"* — **WEAK**; it proves the bit is SET, not that it
  is ENFORCED, and under TCG enforcement is a property of the emulator. That is
  DDR-1046 §2.1's own correction restated.

So the arm is that **the write must now fault**. `wp_selftest()` (`main.c`) arms
DDR-1040's latch around a ring-0 store to a page `vmm_protect_kernel()` stamped
`e &= ~VMM_RW`, and requires a consumed `#PF` with the kernel surviving to print.

| kernel | `cr0` | result |
|---|---|---|
| `25f4dae4a3f90bcb` **shipped** | `0x0000000080010013` `wp=1` | `PRADYOS_WP_ENFORCED vec=14 err=0x0000000000000003` |
| `f877ea4c94324deb` **control** | `0x0000000080000013` `wp=0` | `PRADYOS_WP_WRITE_ALLOWED` |

**`err=0x3` is the discriminating value, not merely "a fault happened":**
present(1) | write(2), with the **user bit clear** — a *supervisor* write to a
present *read-only* page. Asserted as that exact string (DDR-1044's discipline),
never as "some fault".

**THE CONTROL IS THE PRE-FIX TREE, NOT A SYNTHETIC DEFECT** (the DDR-1066/1067/
1090 form): the one `cr0 |=` line removed, the probe kept. And its capture
reproduces DDR-1125's finding a third time —

```
28: [wx] kernel W^X OK
71: PRADYOS_WP cr0=0x0000000080000013 wp=0 byte=0x0000000000000060 intact=1
72: PRADYOS_WP_WRITE_ALLOWED
```

— the audit reporting success on a kernel where the ring-0 write to the page it
just stamped completes.

### 2.1 The vacuity claim is MEASURED, not argued

The control kernel was run against the gate **twice**, before and after the arm
was registered:

* arm not yet registered → `GATE_RC=0` — **the gate passes on the unfixed tree**;
* arm registered → `GATE_RC=2`, *"required pattern
  `PRADYOS_WP_ENFORCED vec=14 err=0x0000000000000003` not found"*.

That pair is the whole argument for the arm existing. Without the first run,
"the gate catches it" and "the gate was always going to pass" are the same
observation.

### 2.2 `intact=1`, and why the address was checked before it was believed

The probe writes back **the byte it just read**, so memory is bit-identical on
both kernels — with WP set nothing is written at all; without it the same value
goes back. `intact=1` in both captures.

The address is `__text_end`, and that is checked rather than assumed:
`vmm_protect_kernel` clears RW for `va < text_end` **and again** for
`va < rodata_end`, and **keeps RW past `rodata_end`**. So the byte sits in an
RW-clear page only while `.rodata` is non-empty — measured at
`__text_end=0xffffffff80055000` against `__rodata_end=0xffffffff80142000`,
~970 KiB apart. Were `.rodata` ever empty the store would land in the
RW-**keeping** branch and succeed for a trivial reason, and the arm would go
quiet rather than loud. The byte is printed (`byte=0x60`, the same value
DDR-1125 measured at the same address) so a reader can see which branch it was in.

---

## 3. A REFUSAL, recorded rather than quietly shipped

I added `PRADYOS_WP_WRITE_ALLOWED` to the gate's `FORBIDDEN_SENTINEL` and then
**removed it again**. It cannot fire independently, for two measured reasons:

* `wp_selftest`'s three outcomes are `if / else if / else` — **mutually exclusive
  by construction**, so a capture can never hold both strings;
* `boot_test.sh` checks **required patterns before forbidden ones** — observed
  directly in §2.1's control run, which reported the missing required pattern and
  never reached the forbidden scan.

So it would have read as a second net while being unable to catch anything the
required arm does not. That is the dead-arm class this project keeps finding, and
adding one deliberately is worse than not adding it. The gate keeps
`FORBIDDEN_SENTINEL="kernel W^X FAIL"` exactly as before — which also means its
DDR-785 early-exit eligibility is unchanged (it already declared one, so per
DDR-1043 it already runs its full 90 s window by design).

**No new gate** (179 unchanged), **no new `GLOBAL_FORBIDDEN` entry** (77
unchanged). The arm goes on the gate that already owns the claim — the
DDR-1039/1046/1070 reasoning.

---

## 4. THE FINDING: DDR-1046's central measurement could not have detected its own case

This is a correction to a **reading**, not a defect, and DDR-1046 is not
withdrawn: its walk, its per-section stamping, its alias read-back and its audit
are all correct for what they do.

DDR-1046 cleared `VMM_RW` on the 2 MiB identity alias, booted, observed the boot
was *"line-for-line normal (423 lines, steady state t=14500, no fault)"*, and
concluded **"nothing writes the kernel image through a physical address."**

**With `CR0.WP` clear, clearing `VMM_RW` had no effect on ring 0 whatsoever.** A
stray ring-0 write through that alias would have completed silently in exactly
the same way before and after the change — so a clean boot was guaranteed
regardless of whether such a writer existed, and that measurement could not have
distinguished the two.

DDR-1046 **caught this class in its own §2.1** — *"the first measurement showed
only that nothing crashed, which cannot distinguish 'the alias is read-only' from
'the write-protect never applied'"* — and answered it with a **PTE read-back**
(`PRADYOS_WX_ALIAS present=1 rw=0 nx=1`). That read-back settles *did the stamp
apply*. It cannot settle *is the stamp enforced*, because it reads the same page
tables the stamp wrote. The question moved one level down, to CPU configuration,
and stayed there — which is DDR-1125's finding arriving at DDR-1046's own
evidence rather than only at its claim.

**So this commit's regression run is the first time that premise has been tested
with enforcement switched on.** §5 is that test.

---

## 5. The regression set — what DDR-1125 said was still owed

DDR-1125 §5 was explicit that its single `smoke-shell` pass licensed *"not
blocked"* and **not** *"ready"*: *"one gate on one CPU (`[apic] up id=0 cpus=1`)
is not a regression set, and AP coverage is by construction, not measurement."*

Eighteen gates, run sequentially (NON-NEGOTIABLE 12: never two QEMU at once),
with the kernel hash **pinned and re-checked after every gate** (DDR-1060 §9) so
a mid-campaign rebuild cannot void the run the way it voided DDR-1060's:

```
kernel_pinned=25f4dae4a3f90bcb
smoke-wxkernel          rc=0     smoke-sharedpte         rc=0
smoke-shell             rc=0     smoke-cowfork           rc=0
smoke-smp               rc=0     smoke-mprotect          rc=0
smoke-smppreempt        rc=0     smoke-fs                rc=0
smoke-rqstress          rc=0     smoke-smpuser           rc=0
smoke-blk-integrity     rc=0     smoke-compositor        rc=0
smoke-blkmq             rc=0     smoke-wmmax             rc=0
smoke-rqstress-liveness rc=0     smoke-uefi              rc=0
smoke-sysmmap           rc=0     smoke-iso-x86           rc=0
REGRESSION_DONE pinned=25f4dae4a3f90bcb
```

**18 of 18 `rc=0`**, 37 minutes, hash identical at entry and after every gate — so
no mid-campaign rebuild voided it.

**The binary those gates ran on IS the binary this commit carries.** After the
campaign one comment in `wp_selftest`'s `else` branch was corrected (it still
said *"Forbidden on `smoke-wxkernel`"*, true when written and false since §3
removed that entry — the stale-comment class DDR-1106 fixed in its own commit).
That edit rebuilds **bit-for-bit** to `25f4dae4a3f90bcb`, verified by rebuild
rather than assumed, so the evidence above is not about a different kernel.


Chosen for what WP can actually break — a ring-0 write to a page something
stamped read-only:

* **SMP / AP coverage**, which §1 could previously only claim *by construction*:
  `smoke-smp`, `smoke-smppreempt`, `smoke-rqstress`, `smoke-rqstress-liveness`,
  `smoke-smpuser`, at `-smp 4`. Every AP runs `cpu_enable_sse` at `smp.c:276`, so
  these are the first measurements that WP on an AP is survivable.
* **Block layer**: `smoke-blk-integrity`, `smoke-blkmq`.
* **mmap / COW**, the paths that manipulate PTEs most: `smoke-sysmmap`,
  `smoke-sharedpte`, `smoke-cowfork`, `smoke-mprotect`.
* **Compositor**: `smoke-compositor`, `smoke-wmmax`.
* **Filesystem and shell**: `smoke-fs`, `smoke-shell`.
* **BOTH BOOT PATHS**: `smoke-uefi` and `smoke-iso-x86` (BIOS via El Torito
  *and* UEFI via OVMF off one ISO) — see §6.

---

## 6. The boot-path divergence is CLOSED, not merely measured

DDR-1125 §4 recorded that the value was **inherited, not established**, and that
the two arms of one ISO need not have agreed:

* `stage2.asm` never touches bit 16, so the **BIOS** path took the architectural
  reset value `0x60000010` — WP **clear** — by the ISA;
* `boot/uefi/loader.c` contains **zero** CR0 references, so the **UEFI** path took
  whatever firmware left, and **the UEFI specification does not pin WP**.

That is §INV.13's class in its sharpest form — not a property implemented twice
and drifting, but one implemented in **neither** path — and `smoke-iso-x86`
asserts that each arm boots and nothing about CR0.

Setting WP in `cpu_enable_sse()` **establishes** the value on both paths, so the
divergence is moot rather than measured. **The UEFI path's pre-fix CR0 is still
not measured and no claim is made about it** — what changed is that it no longer
matters.

---

## 7. Narrow by construction, and the capture shows it

Two `[trap] user #PF ... err=0x7` kills fire in every boot — `WXVIOL.ELF` (the
W^X violator, announced one line earlier) and `METRIC.ELF` writing the read-only
metric page. Both are **CPL-3** writes to read-only pages, which the CPU enforces
**regardless of WP**, so both were always enforced and both behave identically
before and after. They appear unchanged in the shipped capture.

**WP moves exactly one thing: ring-0 writes.**

The user-copy path is likewise unaffected, measured not assumed:
`vmm_user_range_ok` rejects on `!(e & VMM_RW)` **in software**, so `copyout` to a
COW or read-only user page returns `-EFAULT` whatever WP does — the shipped arm
`[uaccess] copyout RO page EFAULT OK` is present in both binaries' captures.
**COW is not silently defeated, and that was not left as an inference.**

---

## 8. Not claimed

* **No exploit, and no artefact of an actual stray kernel write.** The only
  ring-0 write to a read-only kernel page ever observed in this project is the
  probe's own. DDR-1046's measurement stands uncontradicted — §4 says it could
  not have detected such a write, which is **not** the same as saying one exists.
* **No defect alleged in `vmm_protect_kernel`.** Its walk, per-section stamping,
  alias read-back and audit are all correct for what they do.
* **DDR-757 and DDR-1046 are not withdrawn.** One claim's SIZE was corrected by
  DDR-1125 (the RO half was advisory for CPL 0); their NX half was always real
  and is untouched.
* **Nothing is claimed about the UEFI path's pre-fix CR0** (§6).
* **`smoke-wxkernel`'s other two arms are unchanged** and still pass.
* **NOT EXONERATED IN ADVANCE (DDR-1042).** This changes CR0 on **every CPU,
  including every AP**, on a kernel whose open defect (OPEN-2) is a
  timing-sensitive AP freeze. If the OPEN-2 signature moves, this commit is a
  candidate and *"the diff is elsewhere"* is not an argument.
* **No open issue moves** — OPEN-1/2/12/13 untouched. This is not an `[apfreeze]`
  and not OPEN-2.
* `kernel.bin` `854bbb38fdfe4fd2` → `25f4dae4a3f90bcb`, **1,319,306 B — size
  UNCHANGED**, so the size/headroom pair and `ci-docstate-check` are unaffected.
  Per DDR-1097 a size comparison could not tell the two binaries apart at all;
  **only the hash discriminates**, and it moved — which is how the mutation is
  known to have applied (INV.10's trap, checked).

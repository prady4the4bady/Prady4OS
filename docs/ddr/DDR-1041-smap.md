# DDR-1041 — SMAP: turning `uaccess.h`'s contract into one the hardware checks

**Status:** IMPLEMENTED + gated (`smoke-smap`, shard 0) + enumeration measured
**Group:** A — Kernel Completeness. Closes the second half of the `SMEP / SMAP` row.
**Depends on:** DDR-1040 (the expected-fault latch, and the CPU-model finding).

---

## §1 — What SMAP is, and what it is *for* here

CR4 bit 21 makes a ring-0 **data** access through a user translation fault
unless `EFLAGS.AC` is set. `stac` sets AC, `clac` clears it.

`kernel/mm/uaccess.h` has always opened with this claim:

> The kernel NEVER dereferences a raw user pointer anywhere else — a bare
> `*user_ptr` / memcpy from a user address in a syscall handler is a defect.

That is a **documented contract**, enforced by review. SMAP is what turns it into
one the hardware checks: with SMAP on, a violation is not something a reviewer
has to notice, it is a `#PF` that names its own RIP.

So this DDR is not only "add a mitigation". It is also a **test of a claim the
tree has been making for eleven ADRs**, and §3 is that test.

---

## §2 — Why the shield is a runtime branch, not an unconditional `stac`

`stac`/`clac` are `#UD` on a CPU without SMAP, and the TCG default (`qemu64`)
does not have SMAP — measured through QMP in DDR-1040 §2, *before* any of this
was written. An unconditional pair in `copyin` would therefore have killed all
171 gates on the first boot.

`uaccess_begin()` / `uaccess_end()` branch on `g_smap_on`, published by
`cpu_enable_smap()` **only after** the CR4 bit is read back set — a flag set
ahead of the bit would be a lie in the one direction that faults. One predictable
branch per copy is the cost; the alternative is alternatives-patching, which is a
great deal of machinery for three call sites.

Placement is deliberate: the window opens **after** `vmm_user_range_ok`, so a bad
pointer is still rejected with AC clear and the validation walk never runs
shielded. In `copyinstr` the pair is around the **single byte**, not hoisted out
of the loop — hoisting would hold AC across the page-boundary revalidation, i.e.
across kernel page-table reads, which is exactly the exposure SMAP removes.

---

## §3 — THE ENUMERATION, MEASURED

The question "which kernel sites dereference a user VA?" was **not** answered by
grep. `kernel/` carries 84 `__user` annotations outside `uaccess.c`, most of them
prototypes; reading that list is how six false gaps were produced earlier in this
session, and one missed site here is a boot-time panic, not a test failure.

Instead: enable SMAP and let every unshielded site fault and name itself.

**Step 1 — a full boot, SMAP on, before any sweep.**

| | lines | end state |
|---|---|---|
| `-cpu qemu64,+smep,+smap` | 416 | `[hb] t=14500`, steady |
| default model (baseline) | 418 | `[hb] t=14500`, steady |

Same kernel, same disks; the two differ only in the CPU model. **No panic, no
`#PF`, identical steady state.**

**Step 2 — 19 gates re-run with `QEMU_CPU="qemu64,+smep,+smap"`**, chosen for
user-pointer density rather than convenience:

```
smoke-fs        smoke-user       smoke-sysio      smoke-sysfile
smoke-sysproc   smoke-sysmmap    smoke-sysexec    smoke-sysfork
smoke-syswait   smoke-cowfork    smoke-syspipe    smoke-sysepoll
smoke-syssignal smoke-sysiouring smoke-poll       smoke-mprotect
smoke-execve-argv                smoke-aether     smoke-net-lo
```

**All 19 rc=0.** Every one was verified non-vacuous by grepping its own serial
log for `PRADYOS_SMAP cpuid=1 cr4=1` — see §3.1, which is the half of this
measurement that nearly went wrong.

**Result: the `uaccess.h` contract HOLDS.** Across every path those 19 gates
exercise, there is no unshielded kernel dereference of a user page. That is a
positive finding about eleven ADRs' worth of discipline, and it is why no
`stac`/`clac` was needed anywhere outside `uaccess.c`.

### §3.1 — The vacuity check that the sweep needed

Three gates first reported "no SMAP marker": `smoke-poll`, `smoke-mprotect`,
`smoke-execve-argv`. Their `rc=0` would have been **worthless** — a gate that ran
without SMAP proves nothing about SMAP.

The cause was benign: those recipes set their own `SERIAL_LOG` (`build/poll.log`,
`build/mprotect.log`, `build/argv.log`), overriding the sweep's, so the marker was
in a different file. `QEMU_CPU` still applied and all three did run shielded —
confirmed in their own logs, `build/argv.log` even carrying
`PRADYOS_SMAP_ENFORCED`.

Recorded because **the check is the point, not the outcome**. DDR-1023 recorded a
campaign whose captures were make output rather than serial logs, making its
central grep vacuous; the same shape appeared here and was caught only because
every run was asserted to have actually had the feature on. A sweep that does not
verify its own precondition measures nothing.

---

## §4 — The gate: `smoke-smap` (shard 0)

`QEMU_CPU=qemu64,+smep,+smap`, one boot, ~8 s.

| arm | assertion | what it would catch |
|---|---|---|
| A | `PRADYOS_SMAP cpuid=1 cr4=1` | detected and set, read back from CR4 |
| B | `PRADYOS_SMAP_ENFORCED vec=14 err=0x01` | an **unshielded** ring-0 read of a user page was refused |
| C | `PRADYOS_SMAP_SHIELDED_OK` | the **`stac`-shielded** read returned the seeded byte |
| D | `PRADYOS_SMAP_ALIVE` **after** B | the latch resumed; the kernel did not die at the fault |
| E | `[uaccess] copyin good page OK` + `copyinstr OK`, forbidden-scan clean | the whole boot's real copy traffic ran under SMAP |

**B and C are independent and neither implies the other.** B proves the *hardware
refuses*; C proves the *shield works*. A kernel with SMAP on and `stac` compiled
to nothing passes B and fails C; one with a stubbed enable passes C (vacuously)
and fails B. Arm C also cannot pass silently: an unshielded read with no latch
armed panics.

`err=0x01` is the SMAP data signature — bit 0 P=1, bit 1 W/R=0 (read), bit 2
U/S=0 (supervisor), bit 4 I/D=0 (data) — and is distinct from SMEP's `0x11`, so
the two gates cannot pass on each other's evidence.

Arm E is what carries §3's enumeration into CI permanently: every `copyin` and
`copyout` the boot performs — ~65 ring-3 probes, the syscall path, the FS
self-tests — runs shielded on every run of this gate.

The probe's asm helper windows the **load instruction itself**, unlike DDR-1040's,
because a data fault reports the RIP of the access rather than of a target.

---

## §5 — Mutants, MEASURED

Clean kernel: **`5970a8506c66c115`**, 1,175,946 B. The clean tree rebuilt
bit-for-bit to that hash after the last revert.

| | mutation | kernel.bin | result |
|---|---|---|---|
| **M1** | `cr4 \|= (1<<21)` removed | `aa2e5c127abe9756` | **rc=2** — arm **A** (`cr4=0`) |
| **M2** | `uaccess_begin`/`uaccess_end` bodies emptied | `13cae5521e75272f` | **rc=2** — **kernel panic**, see below |
| **M3** | probe page mapped without `VMM_USER` | `eb0e0c749e99408c` | **rc=2** — arm **B** alone |

**M3 is the load-bearing one.** It leaves A, C, D and E passing and fails B
alone, which is what proves arm B measures **user-ness** rather than "a fault
happened at that address". Without M3 the gate could be testing the latch.

**M2's failure is more interesting than the table row, and it corrects §5's own
first draft.** The draft predicted "arm C, and `smoke-fs`/`smoke-user` would
panic in `copyin`". What actually happens:

```
[uaccess] copyin good page OK        <- PASSES: this self-test runs BEFORE
[uaccess] copyinstr OK                  cpu_enable_smap(), so it is unaffected
PRADYOS_SMAP cpuid=1 cr4=1
*** NEXUS KERNEL PANIC ***           <- arm C's shielded read, now unshielded,
                                        with no latch armed
```

So M2 never reaches arm C's assertion — it kills the boot at the read that arm C
was going to check. The gate still fails, and for the right reason, but "fails
arm C" would have been the wrong description.

**And the panic claim about real traffic was then measured rather than left as a
prediction:** `smoke-fs` re-run on the M2 kernel with `+smap` gives

```
*** NEXUS KERNEL PANIC ***
exception: #PF page fault  vector=0x0E  error=0x0000000000000001
RIP=0xFFFFFFFF80001B6C
```

`error=0x01` is the SMAP data signature. So the shield is load-bearing for the
kernel's *actual* copy traffic, not only for the probe — which is the claim worth
having, and it took one extra boot to stop guessing at it.

## §6 — WHAT THIS DOES NOT COVER — the AC-across-interrupt window

**An interrupt taken between `uaccess_begin()` and `uaccess_end()` runs its
handler with AC still set.** The CPU clears `IF` on an interrupt gate; it does
**not** clear `AC`. Inside that window SMAP is effectively off for that CPU.

Linux clears AC on kernel entry for exactly this reason. **This kernel does not,
and that is a real residual, not a theoretical one** — `copyin` of a large buffer
is a `memcpy` long enough to be preempted by the 100 Hz timer.

It is recorded rather than fixed because fixing it means touching the interrupt
entry path (`isr_common`) for every vector, and that path is load-bearing for
DDR-981, DDR-1006, DDR-1010 and the still-open OPEN-2 — three of which are
unresolved AP-freeze investigations whose evidence is entry-path behaviour.
Changing it days after a release candidate, to close a window that requires an
attacker to already be executing in ring 0, is the wrong trade. Named here so the
next person does not have to rediscover it.

## §7 — Other limits, stated

- **Single-CPU measurement.** §3 ran at the default `-smp`. `cpu_enable_smap()`
  is called from `smp_ap_entry` beside `cpu_enable_smep()`, so APs do set the
  bit, but no SMAP measurement in this DDR ran with APs live.
- **19 gates, not 172.** The sweep covers the user-pointer-dense gates. It does
  **not** cover the compositor/GPU/tablet gates, `smoke-shell` (which invokes
  QEMU directly and ignores `QEMU_CPU`), or the ISO gates. An unshielded deref on
  one of those paths would not have been found.
- **QEMU 8.2.2 TCG only.** No claim about physical hardware.
- **`stac`/`clac` are not free.** One taken branch and one instruction per copy,
  and one *pair per byte* in `copyinstr`. Not measured; a bulk string reader would
  amortise it, and none exists.

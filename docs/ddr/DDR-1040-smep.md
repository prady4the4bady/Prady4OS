# DDR-1040 — SMEP, and the expected-fault latch that makes it provable

**Status:** design (implementation follows in the same commit)
**Group:** A — Kernel Completeness (`SMEP / SMAP` row)
**Supersedes nothing. Blocks nothing. Enables DDR-1041 (SMAP).**

---

## §1 — What is missing

`CLAUDE.md` §GROUP A carries one row for two features:

> | SMEP / SMAP | `CLAC`/`STAC` around every copyin/copyout | `smoke-smep` |

Grepped, not assumed: `kernel/` contains **zero** occurrences of `SMEP`, `SMAP`,
`stac` or `clac`, and the only `CR4` writes in the tree are
`cpu_mitigations.c:59-61` (OSFXSR|OSXMMEXCPT) and `sched.c:913-916` (OSXSAVE).
Both features are genuinely unbuilt.

**SMEP** (CR4 bit 20) makes an instruction fetch from a page whose translation
has U/S = user fault at CPL 0. It is the mitigation for the oldest kernel-exploit
primitive there is: corrupt a function pointer, point it at a page the attacker
already controls in their own address space, and the kernel executes it with
full privilege. Nothing else in this kernel prevents that — `vmm_protect_kernel`
(DDR-757) audits the *kernel's own* PTEs and says nothing about user ones.

**SMAP** (CR4 bit 21) is a different and much larger job (§8). This DDR is SMEP.

---

## §2 — THE VACUITY TRAP, MEASURED BEFORE ANY CODE WAS WRITTEN

The gates run QEMU with **no `-cpu` argument**, so they get the TCG default,
`qemu64`. Queried directly through QMP `query-cpu-model-expansion`:

```
"smap": false
"smep": false
```

So on the CPU every one of the 170 gates runs on, `CPUID.(EAX=7,ECX=0):EBX`
reports **neither feature**. A correct implementation — "set CR4.SMEP if CPUID
says the CPU has it" — is therefore a **permanent no-op in CI**, and any gate
asserting "SMEP enabled" would either fail on every run or be written as a
conditional skip that never executes. That is the DDR-1012 / DDR-973 vacuity
shape: a gate whose only reachable outcome is the passing one.

`-cpu qemu64,+smep,+smap` is accepted by this QEMU (8.2.2) and the same query
then reports both `true`. **The gate therefore pins its own CPU model**
(`QEMU_CPU=qemu64,+smep`); `boot_test.sh:529` already threads `QEMU_CPU` through.
The rest of the suite keeps the default model, where the CPUID guard correctly
takes the no-op path — which the gate also asserts, so "absent" and "present"
are both covered rather than one being assumed.

**This is the point of the section:** the feature would have shipped, looked
correct, passed review, and never once executed. Measuring the CPU model was the
whole difference, and it cost one QMP query.

---

## §3 — Why SMEP is safe here, by construction

SMEP faults on a ring-0 instruction fetch through a **user translation**. Enabling
it breaks any kernel that executes from a U=1 page. This kernel does not, and the
reason is structural rather than incidental:

| mapping | built at | flags | U bit |
|---|---|---|---|
| low identity, 1 GiB of 2 MiB pages | `stage2.asm:534` | `0x83` = P\|RW\|PS | **0** |
| kernel higher-half, 512 x 4 KiB | `stage2.asm:549` | `0x3` = P\|RW | **0** |
| every intermediate table on both | `stage2.asm:532-546` | `\| 0x3` | **0** |

and the user subtree is disjoint from both: `VMM_USER_MIN = 0x8000000000`, so
`VMM_USER_MIN >> 39 == 1` — **all user mappings live in PML4 slot 1**, while the
identity map is slot 0 and the kernel is slot 511. `vmm.c:164-165` promotes an
intermediate entry to `VMM_USER` when mapping a user page, and slot 1 is the only
slot it can ever reach.

So no address the kernel executes from has U=1, on any CPU, at any time. **SMEP
cannot fault on correct kernel execution here.** Recorded as reasoning from the
page tables, not from a test — the test is §5, and it is what would catch this
being wrong.

---

## §4 — The expected-fault latch, and why the feature needs one

An arm that prints `CR4.SMEP=1` proves the bit is **set**, not that the CPU
**enforces** it. That is decoration — the ninth dead-arm instance was caught in
DDR-1039's design text two commits ago, and this is the same shape.

Proving enforcement means causing the violation and observing the fault. But a
ring-0 `#PF` in this kernel is fatal: `idt.c` has no fixup table, and the CPL-0
path falls through to the panic. So enforcement is unobservable *until the kernel
gains a way to survive one deliberate fault*.

**`kernel/idt.c` gains a one-shot expected-fault latch:**

```
fault_expect_arm(lo, hi, resume)   /* arm: faults with lo <= RIP < hi resume at `resume` */
fault_expect_taken(&err)           /* disarm; returns 1 if it fired, with the error code */
```

On a CPL-0 fault the handler consults the latch **before** the panic path. If it
is armed and the faulting RIP lies inside the armed window, it records
`vector`/`error`, sets `r->rip = resume`, disarms, and returns. Otherwise the
panic path runs exactly as today.

Four properties, each load-bearing:

1. **One-shot.** Armed explicitly, disarmed by the first matching fault. A latch
   that stayed armed would silence every later fault — the exact failure DDR-1019
   found in the panic arbitration, where a winner that could not print silenced
   every subsequent panic.
2. **RIP-windowed.** Only faults from the instruction that was supposed to fault
   are caught. A fault anywhere else still panics.
3. **Single-CPU by precondition.** The latch is one global, not per-CPU, and it
   is armed only with interrupts masked and only before `smp_start_aps()`
   (`main.c:3457`; the probe sits beside `uaccess_selftest` at `main.c:3395`).
   `fault_expect_arm` **refuses and prints** if any AP is online, so the
   precondition is enforced rather than commented. Making it per-CPU would need
   the GS-independent `percpu_by_apic_id(lapic_id())` form DDR-1010 had to use,
   and there is no caller that wants it.
4. **In BSS.** Statics, so zero-initialised by definition — §NON-NEGOTIABLE 10's
   `kmalloc`-does-not-zero trap does not arise, and no `sched_create` initialiser
   is owed.

This latch is not scaffolding for one gate. It is the mechanism DDR-1041 needs to
**enumerate** the SMAP work by measurement instead of by grep (§8).

---

## §5 — The probe

`smep_selftest()` in `main.c`, modelled on `uaccess_selftest` (`main.c:3132`),
which already builds a throwaway address space, switches `CR3` with interrupts
masked, and tears it down:

1. Allocate a frame; write `0xC3` (`ret`) into it through the identity view.
2. `vmm_map_in(as, UVA_X, frame, VMM_USER)` — **user, present, and NOT `VMM_NX`**,
   i.e. an executable user page. This is the one line that differs from every
   other mapping in the tree, and M2 mutates it.
3. `cli`, switch `CR3` to the throwaway AS.
4. Arm the latch over `[UVA_X, UVA_X+1)` and `jmp` to `UVA_X`.
5. Restore `CR3` and flags; report.

**Why the window is the TARGET address, and why the transfer is `jmp` not
`call`.** The SMEP violation is the *instruction fetch at the target*, so the
faulting RIP is `UVA_X` itself — a window around the transfer instruction would
never match, which is a defect this section carried in its first draft. And a
`call` would already have pushed its return address before faulting, so resuming
past it would leave RSP 8 bytes low. `jmp` pushes nothing, so both outcomes leave
the stack identical and both return through the same `ret`:

```
SMEP on  -> #PF at UVA_X -> latch resumes at smep_call_hi (a bare `ret`)
SMEP off -> the user page's own 0xC3 executes -> same `ret` semantics
```

The transfer lives in its own `.text` asm block (`smep_probe_jmp`) rather than
inline asm bracketed by C labels, so no compiler scheduling decision can move the
instruction out from between the addresses the latch was armed with.

Two outcomes, both reachable and both distinguishable:

| | latch | printed |
|---|---|---|
| SMEP enforcing | fires | `PRADYOS_SMEP_ENFORCED err=0x11` |
| SMEP absent / off | does not fire | `PRADYOS_SMEP_EXECUTED` — the `ret` ran and returned |

`err=0x11` is the SMEP signature: bit 0 P=1 (the page *is* present — a plain
unmapped page would read 0x10) and bit 4 I/D=1 (instruction fetch), with bit 2
U/S=0 (the access was supervisor). The error code is printed, not just its
presence, because "something faulted" and "SMEP faulted" are different claims.

---

## §6 — The gate: `smoke-smep`

| arm | assertion | what it would catch |
|---|---|---|
| A | `PRADYOS_SMEP cpuid=1 cr4=1` | feature detected and the bit actually set |
| B | `PRADYOS_SMEP_ENFORCED err=0x11` | the CPU refused the fetch, with the right error code |
| C | `PRADYOS_SMEP_ALIVE` printed **after** B | the latch RESUMED — the kernel did not merely die at the right moment |
| D | boot completes; global forbidden scan clean | no collateral: nothing else in the boot executes a user page |

Arm C is the arm this design would be wrong without. "Enforced" and "died at that
instruction" produce the same first two lines; only a witness printed afterwards
separates them. Same shape as DDR-1031's fork-and-`wait4`, where a ring-3 fault
is `sched_exit(-1)` and the parent's `st=-1` is the only evidence.

| E | a **second boot on the DEFAULT model**: `PRADYOS_SMEP cpuid=0 cr4=0` and `PRADYOS_SMEP_EXECUTED` | a build that set CR4.SMEP unconditionally (which #GPs on a CPU without it), or reported `cr4=1` beside `cpuid=0` |

Boot 1 is `QEMU_CPU=qemu64,+smep`; boot 2 is the default model. Two boots in one
gate, **sequential and never concurrent** (§NON-NEGOTIABLE 12). Arm E lives here
rather than in `smoke-selftest` so the whole feature — the enforcing path and the
no-op path — is one gate that either covers it or visibly does not. It is also
what keeps §2's measurement honest: it states, *in a gate*, that every other gate
runs on a CPU where SMEP does not exist.

Measured cost: **8 s** for both boots.

---

## §7 — Mutants, MEASURED

Clean kernel: **`6e76c5d7fc35d6f7`**, 1,175,946 B against the 1,572,864 B gate.
The clean tree rebuilt bit-for-bit to that hash after the last mutant was
reverted, so each row below differs from the clean run only in its own mutation.

| | mutation | kernel.bin | result |
|---|---|---|---|
| **M1** | `cr4 \|= (1<<20)` removed (detection kept) | `8f7d05860deaefc6` | **rc=2** — arm **A** (`cpuid=1 cr4=0`), and the log shows `PRADYOS_SMEP_EXECUTED`, so **B** would have failed too |
| **M2** | probe page mapped with flags `0` instead of `VMM_USER` | `2d622ac7ca93295b` | **rc=2** — arm **B** alone (`PRADYOS_SMEP_EXECUTED`; A passed) |
| **M3** | RIP-window check removed from `fault_expect_consume` | `4a674c4e973b15db` | **rc=0 — PASSES EVERY ARM**, as §9 predicted |

**M1 lands on A first, not B**, and the DDR's §7 table said "B". The correction
matters because the two arms are not redundant here: A catches "the bit is not
set" and B catches "the CPU did not refuse", and M1 trips both because a clear
CR4 bit produces both symptoms. It is M2 that shows the arms are **independent** —
it leaves A passing and fails B alone, which is what proves arm B measures
*user-ness* rather than "some fault happened at that address".

**M3 was predicted uncovered and is now MEASURED uncovered.** The probe arms the
latch around a fetch that does fault and nothing else faults in that window, so a
latch ignoring the window behaves identically. That is a stronger record than the
prediction was: the RIP check is live code with no test, stated rather than
assumed, in the same terms DDR-1031 used for its `invlpg` and DDR-1039 for its
column-zero guard.

### §7.1 — Gate defect found and fixed by running it

Arm B's first form was `err=0x0*11$$`, and it **failed on a correct kernel**. The
kernel prints `\r\n`, so a bare end-anchor can never match a line it emits. The
shipped form is `err=0x0*11[[:space:]]*$$`. Recorded because the failure looked
exactly like a real defect — the assertion named the right property, the log line
was present and correct, and only reading the two side by side separated them.

### §7.2 — Regression gates on the clean kernel

`hygiene_check.sh` ALL THREE PASSED. `smoke-shell` rc=0, `smoke-blkmq` rc=0,
`smoke-rqstress-liveness` rc=0, `smoke-blk-integrity` rc=0, `smoke-uaccess` rc=0,
`smoke-wxkernel` rc=0, `smoke-mitigations` rc=0, `smoke-smpuser` rc=0.

`smoke-wx` — the gate name in the `CLAUDE.md` Group A row — **does not exist**
(`No rule to make target`). The real kernel-W^X gate is **`smoke-wxkernel`**. A
gate name in the queue is not evidence a gate exists; that is the same class of
error as the six false gaps found earlier by reading row titles instead of code.

## §8 — SMAP is NOT in this DDR, and how it will be measured

SMAP faults on a ring-0 **data** access through a user translation unless
`EFLAGS.AC` is set (`stac`/`clac`). Enabling it requires shielding every kernel
dereference of a user VA. `uaccess.c` is a clean choke point — `copyin`,
`copyout`, `copyinstr`, three functions — but it is not obviously the *only* one:
`kernel/` carries 84 `__user` annotations outside it, most of them prototypes and
some of them not.

**Grepping that list would be the wrong method** — it is exactly how six "false
gaps" were produced earlier in this session by reading titles instead of code,
and a single missed site is a boot-time panic rather than a test failure.

The right method is available *because of §4*: enable SMAP with **no `stac`
anywhere**, and every unshielded kernel dereference of a user page becomes a
fault that names its own RIP. That enumerates the sites exactly and completely.
DDR-1041 does that, then adds `stac`/`clac` at what it found, then re-measures.

Note what does **not** need shielding, from §3's table: the kernel reaches
double-mapped frames (surface views, the framebuffer, the vDSO, the metric page —
DDR-1003 enumerated all nine `vmm_map_in` sites) through the **identity alias**,
whose translation has U=0. SMAP keys on the translation used, not on the frame,
so those accesses are untouched. That is a prediction, and DDR-1041's measurement
is what will confirm or refute it.

---

## §9 — What this does not do

- **No SMAP.** §8.
- **No general exception-table fixup.** The latch is one global, one shot,
  single-CPU-by-precondition. It is not `__ex_table`, and `copyin` still
  validates rather than faults-and-recovers.
- **The RIP-window check is uncovered.** M3 would pass every arm, because the
  probe arms the latch only around a call that does fault, so a latch that
  ignored the window behaves identically. Covering it needs a second fault from
  a different RIP inside the armed window, and there is nowhere safe to put one.
  Recorded, in the same terms DDR-1031 used for its `invlpg` and DDR-1039 for
  its column-zero guard.
- **Nothing about real hardware.** Every measurement here is QEMU 8.2.2 TCG.
  Whether a physical CPU's SMEP behaves identically is untested and untestable in
  this project.

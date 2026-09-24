# DDR-1140 — CPU exposure report, and why KPTI/retpoline/RSB are recommended for deferral

**Status:** design, committed before code (§NON-NEGOTIABLE 5).
**Operator instruction:** PR #17 comment 5822830053, item 5 (OWNER-verified):
*"KPTI, retpoline, RSB refill (item 70): add them … treat this with the same
rigor as OPEN-2 itself … If at any point the risk of reopening OPEN-2-class
instability outweighs shipping this in v1, say so explicitly and recommend
deferring."*

This DDR does two things:
1. It **builds** item 70's (a) half: a boot-log line that states whether the CPU
   running the kernel is exposed to Meltdown and MDS.
2. It **recommends deferring** KPTI, retpoline and RSB refill past v1. The
   evidence is below. The operator's instruction asked for this outcome to be
   stated rather than pushed through, if it is where the evidence points.

## 1. The exposure report (built)

### 1.1 What it prints

```
[cpu] exposure: vendor=<intel|amd|other> archcap=<0|1> meltdown=<yes|no|unknown> mds=<yes|no|unknown> kpti=0
```

| Condition | meltdown | mds |
|---|---|---|
| `AuthenticAMD` / `HygonGenuine` | `no` | `no` |
| `GenuineIntel`, `IA32_ARCH_CAPABILITIES` (MSR 0x10A) present, `RDCL_NO` (bit 0) set | `no` | from `MDS_NO` (bit 5) |
| `GenuineIntel`, MSR present, `RDCL_NO` clear | `yes` | from `MDS_NO` |
| `GenuineIntel`, MSR absent (CPUID.7.0:EDX bit 29 clear) | `yes` | `yes` |
| any other vendor | `unknown` | `unknown` |

The third and fourth rows follow the rule Linux uses. A CPU that does not
enumerate `ARCH_CAPABILITIES` predates the fixes, so it is treated as exposed.

`kpti=0` is printed as a **literal**, and that is deliberate. The line exists to
make the absence visible in every boot log, next to the answer to whether that
absence matters on this CPU.

### 1.2 Safety

- `rdmsr 0x10A` is issued **only** when CPUID.7.0:EDX bit 29 says the MSR
  exists. A blind `rdmsr` on a CPU without it raises `#GP`. This is the same
  discipline `cpu_mitigations_init()` already applies to `IA32_SPEC_CTRL`.
- The function reads only. It writes no MSR.
- It runs once on the BSP, inside `cpu_mitigations_init()`, which already runs
  before interrupts are enabled.

### 1.3 The gate, and why its obvious form is vacuous

*"Assert the `[cpu] exposure:` line prints"* passes on any implementation,
including one that prints constants. On the CI's default CPU model the honest
answer is also the flattering one, because QEMU's `qemu64` reports an AMD
vendor string. So a gate on the default model alone cannot tell a correct
classifier from one hard-wired to `meltdown=no`.

`smoke-cpuexposure` therefore boots **three CPU models** and requires a
different, exact line from each:

| Arm | `-cpu` | Required |
|---|---|---|
| A | default | `meltdown=no mds=no` |
| B | `qemu64,vendor=GenuineIntel` | `vendor=intel archcap=0 meltdown=yes mds=yes` |
| C | `qemu64,vendor=GenuineIntel,+arch-capabilities,+rdctl-no,+mds-no` | `vendor=intel archcap=1 meltdown=no mds=no` |

- Arm B catches a classifier that ignores the vendor.
- Arm C catches one that never reads the MSR: such a kernel prints `yes` there.
- Arms B and C differ **only** in the MSR, so C is the arm that proves the read.

**Mutants, each on a different arm:**
- **M1:** always print `meltdown=no`. Fails B.
- **M2:** skip the MSR read and treat every Intel CPU as exposed. Fails C.

### 1.4 Not claimed

- **The line reports exposure. It mitigates nothing.**
- Under TCG it describes the emulated CPU, not the host.

## 2. KPTI, retpoline, RSB refill: recommended for deferral past v1

### 2.1 What KPTI means in this kernel, measured

Every user address space today shares the higher-half kernel mapping, in PML4
slot 511 (DDR-1040 §2). KPTI means giving each process a second PML4 that maps
only an entry trampoline, and switching CR3 on every entry and every exit.

Three facts in this tree make that more than an entry-path edit:

1. **No IST stacks.** Every IDT vector is installed with `ist = 0`
   (`idt.c:69`). NMI, `#MC`, `#DF` and `#DB` therefore run on whatever stack
   is current. Under KPTI, an NMI taken in ring 3 before the trampoline has
   switched CR3 would land on `TSS.RSP0`, which is a heap-allocated kernel
   stack that is **not mapped** in the user CR3. The CPU would take a `#PF`
   while delivering the NMI, then a `#DF`, then a triple fault. KPTI therefore
   first needs per-CPU entry stacks, mapped in both CR3s, with IST assigned for
   NMI, `#MC` and `#DF`. That rewrites how every exception is delivered.
2. **`TSS.RSP0` is per-thread and heap-backed** (`tss_set_rsp0`,
   `sched.c:1236` and `:1628`). Under KPTI it must point at a per-CPU
   trampoline stack. That changes the stack every ring-3 → ring-0 transition
   lands on, and so the `context_switch` / `finish_task_switch` frame chain
   that DDR-1115 through DDR-1139 spent 25 DDRs pinning down.
3. **No PCID.** DDR-1075 §3.1(a) measured none. TCG does not implement it
   either: QEMU warns *"TCG doesn't support requested feature: CPUID.01H:ECX.pcid"*.
   So each of the two CR3 writes per syscall and per interrupt is a **full TLB
   flush**. That changes the timing of every syscall and every interrupt on
   exactly the paths OPEN-2 lived in (the SWAPGS producer, DDR-1010; the timer
   ISR producer, DDR-1006; the double dispatch, DDR-1139).

### 2.2 The verification the operator asked for cannot fully exist here

The operator's bar was: *"named mechanism, mutation-tested both directions,
hunted the same way."* Two of the three are achievable. The third is not.

- **Achievable:** proving the CR3 switch happens. A probe reads CR3 in ring 0
  on entry and compares it with the value in ring 3.
- **Achievable:** the stability hunt. That is about 2,200 boots and two
  multi-hour CI campaigns, the DDR-1139 §5 criterion, rerun on the KPTI kernel.
- **Not achievable:** proving that it **mitigates Meltdown**. TCG does not
  execute speculatively, so a Meltdown proof of concept cannot leak under
  QEMU with or without KPTI. The CI hardware cannot demonstrate the property
  the change exists for.

So the gate could show only that KPTI is *wired*. DDR-1097 called that kind of
evidence *"a proof of WIRING and nothing else"*.

### 2.3 Retpoline and RSB refill

- **Retpoline** (`-mretpoline`) rewrites the code generation of every indirect
  call and jump in the kernel. That includes the scheduler DDR-1139 has just
  stabilised.
- The operator has **already accepted** this exact argument for the stack
  protector and CFI. On 2026-09-24 that class was deferred to its own post-tag
  DDR, and the reason given was *"don't touch scheduler-adjacent codegen right
  after DDR-1139 stabilised it"*. Retpoline is the same class of change, with
  the same blast radius.
- **RSB refill** on context switch is hot-path assembly in `context_switch`. On
  its own it is worth little: its main value is as the companion to retpoline.

### 2.4 Exposure, stated so the trade is visible

- **Meltdown affects Intel CPUs that lack `RDCL_NO`.** In practice that means
  pre-2018 Intel. AMD is not affected.
- After §1, every boot log states whether the machine it is running on is
  exposed. A user on an exposed CPU can see it.
- The release notes will say that v1 has no KPTI and that such CPUs are
  exposed.

### 2.5 Recommendation

- **Defer KPTI, retpoline and RSB refill to one post-tag DDR series.**
- Order that series as follows:
  1. IST stacks for NMI, `#MC` and `#DF`, plus per-CPU entry stacks. Hunt it on
     its own.
  2. KPTI on top of that. Hunt it.
  3. Retpoline together with the RSB refill. Hunt it.
- Each step is judged against DDR-1139 §5's pre-registered criterion: zero
  `[schedcheck]` and zero `[apfreeze]` in 2,200 boots.
- **This is a recommendation back to the operator, not a decision.** The
  instruction was to build these, with an explicit escape if the evidence
  warranted it. §2.1 and §2.2 are that evidence.

## 3. Not claimed

- No mitigation is added by this DDR.
- The exposure line changes no behaviour.
- `GLOBAL_FORBIDDEN` is unchanged. One gate is added, `smoke-cpuexposure`.

# DDR-1177 — IPI-based cross-CPU TLB shootdown (checklist C1.2)

Status: DESIGN ONLY. No kernel code in this record. Nothing below is verified;
every behavioural claim about the fixed kernel is **not verified** until a
`pradyos-ci` run link is attached. Platform: QEMU TCG only; **not verified on
real hardware**.

Checklist: `docs/V1_MASTER_CHECKLIST.md:43-46` — "IPI-based TLB shootdown on
unmap/mprotect"; exit gate "shootdown stress gate (name TBD) on SMP". This DDR
names the gate `smoke-tlbshoot`.

## 1. Problem

x86 TLBs are per-CPU and are not coherent with page-table writes. `invlpg`
invalidates only the executing CPU's TLB.

Stale scenario. CPU A and CPU B both run threads of one address space (AS).
B has read VA `v` and caches `v -> frame F`. A unmaps `v`, runs `invlpg v`
locally, and frees F. B's TLB still holds `v -> F`. B keeps reading (or, for a
writable entry, writing) F after it has been returned to the allocator and
handed to another owner. mprotect is the same defect with a weaker effect: A
downgrades `v` to read-only, B's cached RW entry still accepts writes.

## 2. Current state (measured by reading the tree at branch `ddr1177-tlb-shootdown`)

- **No cross-CPU invalidation exists.** DDR-1075 §3
  (`docs/ddr/DDR-1075-group-g-and-a-proof-standard-the-project-cannot-meet.md:129-161`)
  records that a grep for "shootdown" finds nothing and argues the absence is
  correct today on four facts. Fact (c) is "no two threads share an AS".
- **DDR-1077** (`docs/ddr/DDR-1077-a-tripwire-for-the-premise-that-makes-the-missing-shootdown-safe.md:10-34`)
  pins the set of `->cr3` writers (gate `ci-cr3-writers-check`, `Makefile:4018`)
  so that adding `CLONE_VM` is noticed. It explicitly does not build a shootdown.
  Its cited line numbers are stale; today's writers are `kernel/exec/elf.c:323`,
  `kernel/syscall/sys_exec.c:137`, `kernel/proc/sched.c:1149` (`cr3 = 0`, kernel
  master), `kernel/proc/sched.c:1299` (fork child). No `CLONE_VM` exists (grep
  of `kernel/` for `CLONE_VM` is empty).
- **The stated gap:** `kernel/syscall/sys_mmap.c:27-28` — MAP_SHARED stays
  refused because it needs "a cross-CPU TLB shootdown this kernel does not have".
- **Every local `invlpg` site** (each flushes only the current CPU):
  - `kernel/mm/vmm.c:27-28` helper; `vmm.c:245` (`map_core`, new mapping);
    `vmm.c:306` (`vmm_protect_range`, per page); `vmm.c:325` (`vmm_unmap`).
  - `kernel/mm/vmm_cow.c:27-28` helper; `vmm_cow.c:90` (fork: parent PTE made
    RO+COW, "parent AS is active"); `vmm_cow.c:131` (COW fault: PTE re-pointed
    at the copy or re-granted RW); `vmm_cow.c:173` (stack-grow fault, new page).
- **Unmap/mprotect callers:**
  - `sys_munmap` `kernel/syscall/sys_mmap.c:221-232` -> `unmap_range`
    `sys_mmap.c:93-101`, which calls `vmm_unmap` then **immediately**
    `ptnode_free`s the frame (`sys_mmap.c:97-99`). Under sharing, that free is
    the use-after-free in §1.
  - `sys_mprotect` `sys_mmap.c:251-286` -> `vmm_protect_range` (`vmm.c:270`).
  - Other `vmm_unmap` callers: `kernel/syscall/sys_surface.c:388`, `:448`;
    `kernel/main.c:4422`.
- **Kernel mappings are shared by every AS.** `vmm_new_address_space`
  (`kernel/mm/vmm.c:344-358`) copies all 512 PML4 entries from the kernel master,
  so a kernel-half PTE change is visible to every CPU's walk but not to their
  TLBs. All kernel threads share one AS (`cr3 == 0` = master, `sched.c:1149`,
  switch logic `sched.c:1688-1691`). No global pages and no PCID are used
  (grep for `PTE_G`, `PCID`, `CR4_PGE` in `kernel/` is empty), so every CR3
  write flushes the whole non-global TLB.
- **CR3 per CPU:** there is no per-CPU "current AS" field and **no per-AS active
  CPU mask**. `struct percpu` (`kernel/apic/percpu.h:16-61`) holds `current`
  (tcb) and `cpu_idx`/`apic_id`; the live CR3 is derived from `current->cr3`.
- **SMP enumeration:** APs come from the MADT roster, booted by INIT-SIPI-SIPI
  (`kernel/apic/smp.c:1-35`); `g_online` counts them (`smp.c:39`, `:261`),
  exposed by `smp_online()` (`smp.c:45`). Per-CPU records carry `present`
  (`percpu.h:22`).
- **IPI send:** `lapic_send_ipi(dest, icr_low)` (`kernel/apic/lapic.c:68-75`)
  writes ICR_HI then ICR_LO and spins on delivery-status bit 12. Users today:
  wake/resched IPI on vector 49 (`smp.c:292`, `:303`, `:314`), NMI
  (`lapic.c:87`).
- **IDT allocation:** `idt_init` (`kernel/idt.c:79-83`) installs only vectors
  0..63: 0..31 exceptions, 32..47 legacy IRQs, 48 LAPIC timer
  (`kernel/apic/lapic.h:10`), 49 wake IPI (`lapic.h:32`), 50..63 MSI-X
  (`idt.c:146-147`). 0xFF is the LAPIC spurious vector (`lapic.c:34`). Vectors
  **64..254 are free** and have no stubs (`isr_stub_table`, `idt.c:44`; its
  comment still says "32 stub addresses", a stale comment).
- **User fault outcome:** a ring-3 #PF that is neither COW (`idt.c:825`) nor
  stack growth (`idt.c:836`) is logged and the process is killed
  (`idt.c:862-901`, "killing process"); there is no SIGSEGV delivery path.
- **Gate pattern:** `smoke-sigsem` (`Makefile:4349-4354`) sets
  `QEMU_PROBES=sigsem` and lists one `PRADYOS_SIGSEM_*` line per arm in
  `EXTRA_SENTINEL`; the probe formats each line from measured values
  (`user/sigsemtest.c:196-250`). SMP gates set `QEMU_SMP=4`
  (`Makefile:2761-2765`; runner `tools/qemu_runner/boot_test.sh:212-216`).

## 3. Design

### 3.1 Request structure (one global slot)

```c
struct tlb_shoot {               /* kernel/mm/tlb.c, one static instance */
    uint64_t cr3;                /* AS to flush; 0 = kernel half / all ASes */
    uint64_t va, npages;         /* range; npages == TLB_FULL => full flush */
    uint64_t gen;                /* request generation, release-published   */
    uint64_t target_mask;        /* bit per cpu_idx (<= 64 CPUs)            */
    volatile uint64_t ack_mask;  /* bits set by targets with release        */
};
```
Plus per-CPU `uint64_t shoot_seen_gen` in `struct percpu` (append at the tail;
`percpu.h` offsets 0..120 are read by asm and must not move). One request in
flight at a time, serialised by `g_shoot_lock`. Batching is out of scope
(DDR-1075 row 9.3 stays deferred).

### 3.2 IPI vector

`TLB_SHOOT_VECTOR = 0xF0`. Free per §2 (only 0..63 and 0xFF are used). Chosen
high because the LAPIC prioritises by `vector >> 4`; class 15 sits above the
timer (class 3) and MSI-X, so a shootdown is not held off behind device work.
Requires adding one stub and one `set_gate(0xF0, ...)` in `idt_init`; the stale
"32 stub" comment at `idt.c:44` is corrected in the same change.

### 3.3 Target selection

v1: **all online CPUs except self**, filtered to those whose `current->cr3`
resolves to the request's AS, *plus* every online CPU when `cr3 == 0`
(kernel-half change).
- Alternative: a per-AS `active_mask` set/cleared in the CR3 switch
  (`sched.c:1688-1691`). Cheaper (fewer IPIs), but it adds a second writer of
  shared state on the hottest scheduler path, and a CPU that switched away still
  holds entries for the old AS until its next CR3 write. That is fine here only
  because every CR3 write is a full flush (no PCID); it stops being fine the day
  PCID is enabled.
- Tradeoff stated: "all online" is simplest and obviously complete; the
  `current->cr3` filter costs a racy read, but a CPU that switches *into* the AS
  after the filter executes a CR3 write, which flushes. The only CPUs that can
  hold a stale entry are those currently running the AS, so the filter is sound.
  The per-AS mask is deferred until PCID or a measured IPI cost justifies it.

### 3.4 Protocol and memory ordering

Initiator:
1. Edit PTEs (existing code), then local `invlpg`/CR3 reload.
2. Take `g_shoot_lock`. Fill `cr3, va, npages, target_mask`, `ack_mask = 0`,
   then `__atomic_store_n(&req.gen, g+1, __ATOMIC_RELEASE)`.
3. `mfence` (the ICR write is UC MMIO; the fence orders the PTE and request
   stores before the IPI), then `lapic_send_ipi(apic_id, 0x4000|0xF0)` per target.
4. Spin `while (__atomic_load_n(&req.ack_mask, ACQUIRE) != target_mask)`
   with `pause`, servicing §3.7.
5. Release lock. **Only now** free frames (`ptnode_free`).

Handler (vector 0xF0): acquire-load `gen`; if `!= shoot_seen_gen`, flush
(`invlpg` per page, or CR3 reload); `__atomic_or_fetch(&ack_mask, 1<<self,
RELEASE)`; record `shoot_seen_gen`; `lapic_eoi()`.

### 3.5 Call sites that shoot

| Site | Why |
|---|---|
| `vmm_unmap` callers via a new `vmm_unmap_range_flush` used by `unmap_range` (`sys_mmap.c:93`), which reorders free-after-ack | unmap |
| `vmm_protect_range` (`vmm.c:306`) — one shootdown per call, after the loop | mprotect |
| `sys_surface.c:388`, `:448`, `main.c:4422` | unmap (kernel-half if VA is kernel: target all) |
| `vmm_cow.c:90` (fork RO downgrade) | see §3.8 |
| `vmm.c:245`, `vmm_cow.c:173` | **no shootdown**: not-present -> present needs no flush (no negative caching on x86) |
| `vmm_cow.c:131` | **no shootdown**: RO -> RW upgrade; a stale RO entry just faults again and the handler finds the PTE already fixed |

### 3.6 Range threshold

`npages <= 32` -> per-page `invlpg`; above -> CR3 reload (full flush). 32 is a
placeholder, **not measured**; under TCG only static instruction counts may be
reported (CLAUDE.md non-negotiable 15). Revisit with a real-hardware number.

### 3.7 Deadlock rules

- Two CPUs initiating at once each spin waiting on the other's ack. Rule: the
  initiator spins with **IF=1**, so it services incoming 0xF0 IPIs while waiting.
- Callers that hold a lock with interrupts disabled must not shoot. If such a
  call site exists, its spin loop instead polls `req.gen != shoot_seen_gen` and
  runs the handler body inline (the handler is idempotent by `gen`).
- `g_shoot_lock` is taken with a try-loop that also services the pending request
  inline, so a CPU blocked on the lock cannot starve a concurrent initiator.
- Never shoot from NMI or #MC context.

### 3.8 COW and kernel mappings

- Fork's RO downgrade (`vmm_cow.c:90`) runs on the parent's AS. Without
  `CLONE_VM` only the forking thread runs that AS, so the local `invlpg`
  suffices **today**. Once C1.4 lands, a sibling thread on another CPU could
  keep writing the now-shared frame through a cached RW entry: this site must
  shoot then. v1 adds the call (cheap: empty target set today).
- Kernel-half changes go to all online CPUs (§3.3), because every AS shares the
  kernel PML4 entries (`vmm.c:344-358`).

## 4. Gate design: `smoke-tlbshoot`

**Prerequisite conflict:** a user probe with two threads in one AS needs
`CLONE_VM`, which is C1.4 and itself needs C1.2 (`V1_MASTER_CHECKLIST.md:53-56`).
So v1 of the gate is an in-kernel probe (`QEMU_PROBES=tlbshoot`), whose kernel
threads all share the master AS (`sched.c:1149`) — a real shared-AS setup today.
The user-space arm (reader faults) is specified now and enabled when C1.4 lands
(open question Q1).

Setup: `QEMU_SMP=4`. One writer thread on CPU 0, readers pinned on CPUs 1..3.
Kernel-half test VA `v`, two frames: F_old filled 0xAA, F_new filled 0xBB.
Loop 20000 iterations: writer maps `v -> F_old`, publishes `epoch` (release);
readers spin reading `*v`; writer unmaps `v`, shoots, maps `v -> F_new`,
publishes `epoch+1`; each reader, after acquiring `epoch+1`, reads `*v` once.
Reading 0xAA after `epoch+1` is a stale translation.

| Arm | Exact line | Meaning | Why it FAILS on the unfixed tree |
|---|---|---|---|
| A | `PRADYOS_TLBSHOOT_A stale=0 iters=20000` | no reader saw 0xAA after the new epoch | No IPI: readers keep `v -> F_old` cached, so `stale>0`. **Expected; must be shown red in CI before the fix — not verified that TCG's per-vCPU soft TLB reproduces it.** |
| B | `PRADYOS_TLBSHOOT_B early=0` | initiator never returned with `ack_mask != target_mask` (checked by re-reading per-CPU `shoot_seen_gen` after return) | No shootdown code, no counters: line never printed. |
| C | `PRADYOS_TLBSHOOT_C missed=0 targets=3` | every reader CPU acked every request | Line never printed; and with the fix but a wrong mask, `missed>0`. |
| D | `PRADYOS_TLBSHOOT_D prot_stale=0` | mprotect arm: writer downgrades RW->RO and publishes; readers then probe writability via a fixup-guarded write and must take #PF | No IPI: cached RW entry accepts the write, `prot_stale>0`. |
| E (deferred to C1.4) | `PRADYOS_TLBSHOOT_E faulted=1 stale=0` | user thread on CPU 1 reading a page munmapped by a thread on CPU 0 is killed by #PF, never reads freed data | Not buildable: no `CLONE_VM`. |
| — | `PRADYOS_TLBSHOOT_DONE` | completion | — |

Arm D needs a kernel exception-fixup for the probe's write (none exists today);
if that is too large, D moves to the user-space version with arm E.

Mutants (each must be run in CI and shown red before merge):
- **M1 no IPI** (skip `lapic_send_ipi` in the shooter): A red (`stale>0`), D red.
- **M2 no wait** (drop the ack spin): B red (`early>0`; IPI latency exceeds the
  return path). A may stay green by luck, so B is the arm that catches it.
- **M3 wrong mask** (target only `cpu_idx 1`): C red (`missed>0`, readers on
  CPUs 2,3 never ack). A likely red too.
- **M4 free before ack** (reorder in `unmap_range`): not observable by A..D;
  covered only by review. Stated so it is not mistaken for gated.

## 5. Risks and not covered

- TCG may never exhibit staleness (soft TLB flush semantics differ from
  silicon), making arm A vacuous. The pre-fix red run decides; if A is green on
  the unfixed tree, A is labelled vacuous and B/C/D carry the gate.
- 64-CPU limit of `uint64_t` masks; MADT rosters above 64 are refused.
- IPI latency adds to every munmap/mprotect on SMP; not measured.
- Not covered: PCID, global pages, batching (DDR-1075 9.3), MAP_SHARED
  (`sys_mmap.c:27-28` stays refused until C1.3), lazy-TLB, x2APIC.
- DDR-1077's tripwire stays; this DDR updates its stale line numbers only by
  citation, not by edit.

## 6. Honesty labels

Gated by `smoke-tlbshoot` (arms A-C, see Amendment 2). Emulated under QEMU TCG
only. **Not verified on real hardware.** No claim about OPEN-1/2/12/13 or
U1-U3.

## Amendment 1: deviations found during implementation (PR #58, commit `7e3cbaf`)

The core implementation departs from the text above in the ways below. Where they differ, this amendment wins. The core build passed the existing gates in CI ([39109148478](https://github.com/prady4the4bady/Prady4OS/actions/runs/39109148478)). The shootdown behaviour itself is **not verified**: no gate exercises it yet.

1. **Seqcount on `gen`.** Writers set `gen` odd while they fill the slot and even when done; readers re-check it after reading the fields. Without this, a CPU servicing a request inline (§3.4) could pair an old generation with a new target mask and ack a request it never flushed.
2. **The initiator never acks its own request.** It records the new generation as already seen. Otherwise, if it serviced its own request inline, it would set an ack bit outside the target mask and wait forever.
3. **Interrupts stay off for the whole shootdown.** This replaces the first bullet of §3.7. Every wait loop (the lock try-loop and the ack spin) services pending requests inline, which is the second bullet of §3.7. Spinning with interrupts on risked preemption or migration while the initiator owns the slot, and interleaved non-atomic `ICR_HI`/`ICR_LO` writes from an interrupt on the same CPU.
4. **Page-table nodes are freed after the acks too.** §3.4 only reordered the leaf-frame free, but `vmm_unmap` also frees emptied page-table nodes, which the paging-structure caches can still reference. `vmm_unmap_range_flush` works in two passes with one shootdown between them:
   - pass 1 clears PRESENT, keeps the frame address, and tags the PTE with `PTE_SW_DYING` (bit 11);
   - pass 2, after the acks, frees the leaf frames and the page-table nodes.
5. **Readiness gate.** Only CPUs with `percpu.shoot_ready` set are targeted. The flag is set after `lapic_ap_enable()` on each AP, and on the BSP before APs start. Without it, a CPU that is present but has no IDT or LAPIC yet would never ack.
6. **Fork downgrade.** §3.5 placed the flush per page at `vmm_cow.c:90`. The implementation does one full-flush shootdown of the parent address space after the walk instead.

## Amendment 2: owner decision on the gate (#45 comment 6251086742)

Where this amendment differs from §4 or Amendment 1, it wins.

1. **Gate scope.** The C1.2 exit gate is the in-kernel probe `kernel/mm/tlb_probe.c`, arms A-C, run as kernel code on 4 CPUs (`smoke-tlbshoot`, `QEMU_SMP=4`). `CLONE_VM` is not pulled into C1.2.
   - **A** `stale=0 iters=20000`: no reader on CPUs 1-3 sees the previous frame's byte while the phase it read before and after the load is unchanged.
   - **B** `early=0`: when `tlb_shoot` returns, every CPU in the request's target mask has serviced that generation. Kills **M2** (no ack wait).
   - **C** `missed=0 targets=3`: a kernel-half request (cr3 == 0) targets every ready CPU but the initiator. Kills **M3** (wrong mask).
2. **QEMU TCG.** If the M2 red run shows arm A cannot see a stale entry, arm A is labelled **vacuous under TCG** here and arms B and C carry the gate. M2 and M3 must each be shown red in CI, on their own branch, before #58 merges. Results are recorded below.
3. **Arm D (mprotect) and arm E (user space)** move to C1.4's exit criteria as required items. No fault-recovery path that exists only for a test is added to the kernel.
4. **Limits.** The 64-CPU mask limit stays. The per-address-space CPU mask waits for PCID. Why one global, `current`-filtered mask is safe today: no PCID and no global pages are used (`PTE_G`, `PCID` and `CR4_PGE` are absent from `kernel/`), so every CR3 load flushes the whole non-global TLB. A CPU therefore holds translations only for the address space it is running now. A CPU that switches into the address space after the racy target read does a CR3 load, which is a full flush. The day PCID is enabled, a CPU can keep entries for an address space it has left, and this argument no longer holds.
5. **M4 (frame freed before every CPU has acked)** stays **not gated** in C1.2. A gate that catches it is a required C1.4 exit item.
   - **Owner:** the agent working the checklist.
   - **Approach:** under a gate-only build flag, the PMM fills each freed frame with a poison pattern. A `CLONE_VM` sibling thread on another CPU keeps reading the page while the first thread `munmap`s it. A reader that observes the poison pattern through its old translation is a use-after-free. With the correct order (shoot, wait for acks, then free) it cannot observe it.
   - **Mutant:** M4 moves the free before the shootdown, and the gate must go red on it.
6. **Merge rule.** #57 and #58 merge on one green run on the exact head SHA, with a merge commit and a pinned SHA. #58 merges only after M2 and M3 are red.
7. **Cost.** The probe prints `[tlbshoot] reads=<n> cyc_per_shoot=<n>`: rdtsc cycles per `tlb_shoot` call with 3 targets, with interrupts off (§3.7, option 2). Under TCG this is emulated time, not a hardware cost. The value from the green run is recorded below. The fork full-flush versus per-page cost will be measured in C1.4, where fork of a populated address space is gated. Until then it is **not measured**.
8. **Standing rule.** Gate-scope questions are decided and recorded in a DDR, not asked.

### Gate and mutant runs (CI evidence)

| Head | Branch | Run | A stale | B early | C missed / targets | Result |
|---|---|---|---|---|---|---|
| correct code `912ff68` | `c12-shootdown-core` | [39141624725](https://github.com/prady4the4bady/Prady4OS/actions/runs/39141624725) | 0 | 0 | 0 / 3 | green |
| M2 no ack wait `b12f058` | `mut/ddr1177-m2` | [39141621302](https://github.com/prady4the4bady/Prady4OS/actions/runs/39141621302) | 816 | 14832 | 0 / 3 | **red** (A, B) |
| M3 wrong mask `6a9526c` | `mut/ddr1177-m3` | [39141625093](https://github.com/prady4the4bady/Prady4OS/actions/runs/39141625093) | 2794965 | 0 | 20000 / 2 | **red** (A, C) |

Each mutant run failed only `smoke-tlbshoot` (shard 6). The mutants land on different arms: B catches only M2, C catches only M3.

**Arm A is not vacuous under TCG.** It caught M2 by itself (`stale=816`), so the vacuity label from item 2 does not apply.

**Two probe defects were found and fixed before these runs.** Both were in arm A's own check, not in the shootdown.
- Run [39136618235](https://github.com/prady4the4bady/Prady4OS/actions/runs/39136618235) reported `stale=4871737`. With two frames, the next frame is also the previous one. Between the remap and the phase publish, a correct reader sees the next frame. Fixed in `61fe97d`: three rotating frames.
- Run [39139152476](https://github.com/prady4the4bady/Prady4OS/actions/runs/39139152476) reported `stale=2`. An interrupt between the reader's phase load and its byte load let the writer move two phases, and frame (p+2)%3 holds frame p-1's byte. Fixed in `912ff68`: a read counts only when the phase is the same before and after it.
- In both runs arms B and C were 0 on the correct code.
- The first mutant runs (39136668511, 39136669935, 39139150386, 39139154185) were cancelled, because they carried the broken arm A.

**Cost (item 7).** `cyc_per_shoot` on the correct code is 57411, 59096 and 52114 rdtsc cycles in three runs: one `tlb_shoot` with 3 targets and interrupts off, under QEMU TCG on a 2445 MHz TSC, so about 21-24 µs. This is emulated time, not a hardware cost. It is a ceiling for how long interrupts stay off per shootdown under TCG. Fork full-flush versus per-page: **not measured** (C1.4).

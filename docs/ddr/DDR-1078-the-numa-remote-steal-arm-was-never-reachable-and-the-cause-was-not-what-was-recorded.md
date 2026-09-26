# DDR-1078 — the NUMA remote-steal arm has never once executed, and the cause is NOT what DDR-1073 §2 recorded

**Status:** ACCEPTED
**Date:** 2026-09-06
**Corrects:** DDR-1073 §2, on the mechanism and on one measured fact.
**Closes:** the coverage gap DDR-1073 §2 named and declined to build, having
judged it unbuildable. It is buildable, and the thing that makes it buildable is
the thing DDR-1073 got wrong.

---

## 1. What DDR-1073 §2 claimed

> `numa_node_of_cpu` returns 0 for any APIC id with no SRAT entry, so without
> `QEMU_NUMA=1` every CPU is node 0 … **And the two conditions are never present
> together:** `grep -n QEMU_NUMA Makefile` returns EXACTLY TWO LINES (1216, 1221)
> and **NEITHER SETS `QEMU_SMP`**, so both run single-CPU (`boot_test.sh:215`
> passes `-smp` only when set) and with one CPU there is no stealing at all.

It then declined to build a gate, on the reasoning that a bare `steal remote=N`
count is the DDR-1068 `reaped=` shape — a correct kernel legitimately reports
`remote=0` whenever the local pass keeps succeeding — and that forcing a
non-zero value needs work pinned to one node's CPUs, *"which this scheduler has
no API to express."*

**The conclusion (no coverage) was right. Both halves of the reasoning are
wrong**, and the second is wrong in the expensive direction: it declared the gap
unbuildable, so nobody looked again.

---

## 2. Fact one: those gates are NOT single-CPU

`boot_test.sh:279-286` — the `QEMU_NUMA` block **carries its own `-smp 2`**:

```
NUMAOPT=(-m 512M
         -object memory-backend-ram,id=nram0,size=250M
         -object memory-backend-ram,id=nram1,size=262M
         -numa node,nodeid=0,memdev=nram0
         -numa node,nodeid=1,memdev=nram1
         -smp 2)
```

Both arrays are on the command line (`"${SMPOPT[@]}" "${NUMAOPT[@]}"`, :576-577),
so `QEMU_SMP` was never the binding condition. DDR-1073 read line 215 and
stopped; the answer was seventy lines further down in the same file. Confirmed
live in the capture: `[apic] up id=0 cpus=2`.

## 3. Fact two — the real mechanism, and it is one QEMU clause

Measured, on this host (QEMU 8.2.2), two boots differing **only** in the `-numa`
lines, each waiting for `[sched] steal local= remote=`:

| `-numa` form | measured |
|---|---|
| `-numa node,nodeid=0,memdev=nram0` (shipped) | `[sched] steal local=148 remote=0` |
| `…,cpus=0` / `…,cpus=1` | `[sched] steal local=0 remote=167` |

**With `cpus=` omitted, QEMU 8.2 emits no SRAT Local APIC Affinity entry**, so
`g_topo.cpu_node[]` stays `0xFF` for every CPU and `numa_node_of_cpu` returns its
`0` default (`numa.c:194-198`) — **two CPUs, two memory nodes, and one CPU
node.** `steal_pass(self, 0, same=1)` then matches every victim and the remote
pass is unreachable, exactly as DDR-1073 said, **for a different reason**: not
"one CPU", but "one node, because the firmware never said otherwise".

The kernel is not implicated anywhere. `[numa] nodes=2 ranges=3 rejected=0` is
correct in both boots — the **memory** topology was always parsed; it is the
**CPU** half that had no data. The SRAT type-0 parse (`numa.c:97-113`) is
correct and simply never ran.

## 4. Why this defeats the vacuity trap DDR-1073 named

DDR-1073's objection stands **for a general topology**: `remote=0` is a legal
reading whenever the local pass keeps succeeding, so a bare count discriminates
nothing. **One CPU per node removes the alternative.** With CPU0 on node 0 and
CPU1 on node 1 there is exactly one possible victim and it is always remote, so:

* `local=0` is not merely likely, it is **structurally required** — the same-node
  pass can never find a victim, so any non-zero `local` means the node mapping
  has broken;
* `remote > 0` is the positive half — it says the second pass ran **and
  succeeded**, which is the ordering claim DDR-885 makes and nothing has ever
  checked.

The assertion is two-sided without needing an API to pin work to a node: the
**topology** does the pinning, which is the possibility DDR-1073 did not
consider. It is also the strongest available shape — adding CPUs would put two
on one node and hand `local` a legitimate non-zero value again.

## 5. What ships

1. **`boot_test.sh`'s `NUMAOPT` gains `cpus=0` / `cpus=1`.** One clause per node.
   It is also simply more correct: implicit CPU-to-node assignment is deprecated
   in QEMU, and this stops the guest being told a two-node machine has one node's
   worth of CPUs.
2. **`smoke-numa-steal`** — `QEMU_NUMA=1`, requires
   `[sched] steal local=0 remote=`, forbids `[sched] steal local=0 remote=0`.

**The forbidden sentinel is deliberate here and deliberately NOT on `smoke-numa`.**
`smoke-numa`'s own Makefile comment records that it keeps its rejection check
*inside* the required line because a `FORBIDDEN_SENTINEL` disables the DDR-785
early exit and hands that gate a flake. That reasoning is about a gate whose
sentinels land at line ~75 of the boot. This one's sentinel lands at line ~365,
after `rqstress_proof()`, so there is almost no early exit to lose — and
`remote > 0` cannot be written as a required substring, so the forbidden form is
the only way to assert the positive half. Per DDR-1043 the gate therefore runs
its full window by design, which is stated rather than discovered.

**No kernel change.** `kernel.bin` is untouched: the scheduler, the SRAT parser
and the counters were all correct, and what changes is the machine the gate
boots.

## 6. Mutants — three, landing on three different places, and one of them is a finding

| | mutation | lands on |
|---|---|---|
| **M1** | the **literal pre-DDR-1078 topology** — drop both `cpus=` clauses | the **required** arm: `FAIL — required pattern '[sched] steal local=0 remote=' not found`, the capture reading `local=148 remote=0` |
| **M2** | delete the remote pass in `rq_steal` (kernel `ad6292423eda4b5b`) | **NOT this gate's arms** — the **global** `cross-CPU FAIL` sentinel, `[smp] sched cross-CPU FAIL spawned=6/6` |
| **M3** | keep the remote pass working, **do not increment its counter** (kernel `e5a761f9f4c72521`) | the **forbidden** arm: `FAIL — forbidden pattern '[sched] steal local=0 remote=0' appeared` |

**M2 IS THE FINDING, AND IT IS THE REASON THE FORBIDDEN ARM STAYS.** The
intuitive mutant — remove the second pass — does redden, but through a sentinel
this gate does not own: on a one-CPU-per-node machine the remote pass is the
*only* steal path, so deleting it does not merely zero a counter, it stops
cross-CPU scheduling outright and `GLOBAL_FORBIDDEN` catches it first. So M2
proves the pass is load-bearing and proves **nothing about this gate's own
arms**, which is recorded rather than presented as coverage.

That left the forbidden arm unproven, and **M3 is what proves it** — and M3 is
exactly the hole the arm exists for: a machine that steals remotely and
*reports* zero. Without the forbidden arm the required pattern alone
(`local=0 remote=`) passes on M3, because `remote=0` satisfies it. The counter
is the instrument, and an instrument that has stopped counting is the DDR-1060
shape: it would read as "the remote pass never ran" forever, on a healthy
kernel, and nothing would say otherwise.

**The early-exit cost is accepted knowingly** (DDR-1043: a gate declaring a
`FORBIDDEN_SENTINEL` never early-exits and always runs its full window). It is
small here because the sentinel lands at line ~364 of a ~400-line boot, so there
was little tail to skip — and it buys the one arm the global list cannot supply.

**Revert returns `kernel.bin` to `fd913d083446b0d1` BIT-FOR-BIT**, verified by
rebuild rather than assumed, so neither kernel mutant leaves a residue and the
CLAUDE.md size/headroom pair is untouched.

## 7. Regression measured before the claim

`smoke-numa` and `smoke-numa-alloc` both **rc=0** under the pinned topology —
checked because `cpus=` alters what the guest is told and `smoke-numa-alloc`
asserts exact per-node page counts (`node0=59904 node1=6`). They are unchanged,
which is expected (the clause adds CPU affinity entries and touches no memory
range) but was not assumed.

## 8. Not claimed

* **No defect is fixed and none is named.** DDR-885's steal order was always
  implemented correctly; what was missing is a test that could tell if it stopped
  working, which is what DDR-1073 §2 said in the first place.
* **`remote=167` is not a rate or a benchmark.** It is one boot; the two runs are
  cited only for the qualitative flip (`local`↔`remote`), which is the claim.
* **Nothing is established about hardware.** This measures what QEMU 8.2 puts in
  an SRAT. A real machine's firmware supplies CPU affinity entries, so the
  shipped kernel path is the one that was already correct.
* **The 4-CPU picture is unchanged**: `smoke-rqstress` still runs one node, so
  `local=` remains the arm there. This does not merge the two gates.
* **`QEMU_NUMA=1` with `QEMU_SMP` set is now an error**, not a silent
  mis-topology: `cpus=0`/`cpus=1` covers exactly the two CPUs `NUMAOPT` asks for,
  so a gate setting both would leave CPUs QEMU refuses to start. No gate does,
  and that is recorded rather than guarded — a guard would be a second thing to
  keep in step for a combination nothing uses.
* 177 gates → **178**. `GLOBAL_FORBIDDEN` **76, unchanged**. No open issue moves
  (OPEN-1/2/12/13 untouched).

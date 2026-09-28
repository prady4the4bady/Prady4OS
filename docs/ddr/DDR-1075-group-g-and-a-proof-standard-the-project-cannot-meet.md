# DDR-1075 — Group G: a proof standard the project cannot meet, a row with no subject, and a trip-wire for the pthreads row

**Status:** ACCEPTED (audit; docs-only, no code change)
**Date:** 2026-09-06
**Scope:** `CLAUDE.md` §GROUP G — Phase 9 Assembly Optimization (six rows)
**Method:** every row measured against the Makefile, `tools/ci/gate_shards.txt`
and the sources — never inferred from another DDR (DDR-1007's discipline).

---

## 0. Summary

Fifth backlog table audited after Groups E (DDR-1071), F (DDR-1072) and A+B
(DDR-1073). Group G is shaped differently from all three: its rows are not
stale about what is built — they are stale about **what can be proved**.

| § | finding |
|---|---|
| 1 | The group's stated proof standard is **unobtainable in the only environment available**, and the project already wrote down the honest substitute — in the adjacent group, two DDRs in. |
| 2 | Row 9.5's *mechanism* is SHIPPED (DDR-873) and its *stated claim* is NOT delivered. Corrected, not closed. |
| 3 | Row 9.3 has **NO SUBJECT** — there is no TLB shootdown to batch — and the absence is currently CORRECT, on four facts. **§3.2 is a trip-wire for Group D's pthreads row.** |
| 4 | Row 9.6 names the wrong instrument, and the real asymmetry is larger than the row: `memcpy` is fast and **`memset` is a byte loop**. One genuinely buildable item, not built, trap named. |
| 5 | Rows 9.1 / 9.2 / 9.4: what the tree says, each with the reason an assembly rewrite is the wrong lever. |
| 6 | The header's item count is wrong and no seventh item exists. |

**Not claimed:** no code change; `kernel.bin` untouched; 177 gates unchanged;
no gate re-run; **no profile was run** — every figure below is a static count
read out of the tree, or one DDR-870 already recorded.

---

## 1. The proof standard, and the substitute the project already invented

The Group G preamble reads:

> *"For each item: **profile first** (add timing instrumentation), establish
> baseline, implement, measure improvement. Gate must show **measurable
> speedup in a deterministic test**."*

**The harness it asks to be built already exists**, and it is CI-registered:

* `user/benchtest.c` — "Group 8 items 44/45, DDR-870"
* `smoke-bench` — `Makefile:990`, `QEMU_PROBES=bench`, required sentinel
  `PRADYOS_BENCH_OK`, forbidden `BENCH FAIL`
* `tools/ci/gate_shards.txt` — **shard 8, 90 s, strict tier**

So "profile first" is not unstarted work. What matters more is the constraint
that harness states about itself, in its own header:

> *"Under QEMU TCG the guest RDTSC counts EMULATED time, not host cycles and
> not the cycles a real CPU would spend. A dynamically-translated `swapgs` is
> not one hardware `swapgs`. So these figures are: VALID for regression …
> INVALID as an absolute hardware claim."*

`arch/x86_64/context.asm` repeats it beside its own measurement:

> *"It is NOT a hardware cycle count … valid for spotting a regression and
> worthless as an absolute claim. **Do not quote it as 'cycles'.**"*

**A "measurable speedup" as a hardware claim therefore cannot be produced in
this environment at all.** Two things can:

1. **The static instruction and memory-traffic count** — exact, and
   hardware-true. This is DDR-870's "COST" convention (Group 8 item 44), and
   it is why `context.asm` and `ipc_copy.asm` carry instruction counts rather
   than timings.
2. **A TCG regression figure** — meaningful only against another figure from
   the same harness on the same host.

§NON-NEGOTIABLE 17 ("performance claims need a denominator — total AND
per-event") is satisfied by (1). It is *not* satisfied by a TCG number
presented as a speedup, which is precisely the "inventing precision the
measurement cannot support" that `benchtest.c`'s own header refuses.

**The finding is the ordering.** DDR-870 established the substitute; the
Group G table was written afterwards and asks for the thing the substitute
exists to replace. Any session picking up a Phase 9 row and taking the
preamble at its word will either produce a number it must not quote, or stall
looking for a profiler that would not help. **Every Phase 9 row's acceptance
criterion should read: a static instruction-count reduction, plus `smoke-bench`
not regressing.** That is deliverable; "measurable speedup" is not.

---

## 2. Row 9.5 — IPC fast path: mechanism shipped, claim not delivered

Row: *"IPC fast path — single-copy where possible."*

**Shipped, and wired:** `arch/x86_64/ipc_copy.asm` (DDR-873, "Group 8 item
43") — `void ipc_copy32(void *dst, const void *src)`, 4 instructions
(2× MOVDQU load, 2× MOVDQU store), no branches, no CPUID gate, no AVX state
touched. Called at `kernel/ipc/ipc.c:43` (send) and `:81` (recv), pinned by

```c
_Static_assert(IPC_MSG_WORDS * sizeof(uint64_t) == 32,
               "ipc_copy32 hard-codes 32 bytes; IPC_MSG_WORDS changed");
```

**But "single-copy" is a different claim, and it is not delivered.** Counted
on the ring-3 path, in the tree:

| # | copy | site |
|---|---|---|
| 1 | sender's user buffer → kernel `msg[]` | `copyin`, `sys_aether.c:349` |
| 2 | kernel `msg[]` → `e->msg` | `ipc_copy32`, `ipc.c:43` |
| 3 | `e->msg` → kernel `out[]` | `ipc_copy32`, `ipc.c:81` |
| 4 | kernel `out[]` → receiver's user buffer | `copyout`, `sys_aether.c:373` |

DDR-873 made copies 2 and 3 cheaper. **It removed none of the four.** This is
the DDR-1071 §3 `smoke-horizon` shape: the row is half right, so it is
CORRECTED rather than closed — marking it done would claim an elimination
that did not happen.

**And the blocker is named rather than guessed.** `ipc_send` already holds
`e->waiting_receiver` (`ipc.c:45`), so the rendezvous *hook* for a direct
handoff exists — which is exactly why this looks buildable. It is not: the
receiver's destination is a **user pointer in the RECEIVER's address space**
(`a2` at `sys_aether.c:373`), and the sender is executing on its own CR3. A
direct sender→receiver copy therefore needs either the receiver's address
space active or its buffer mapped into the sender's — the same cross-address-
space problem DDR-1038 named for `SYS_FUTEX`, not an assembly question.

A single-copy *kernel-internal* IPC (both sides in ring 0) is reachable and
would remove copy 2 or 3; it is worth little, because the two remaining
`copyin`/`copyout` dominate and the in-kernel callers are not hot.

---

## 3. Row 9.3 — TLB shootdown batching: there is nothing to batch

Row: *"TLB shootdown batching under SMP."*

```
$ grep -rniE "shootdown" kernel/ --include=*.c --include=*.h
   (no output)
```

**There is no cross-CPU TLB invalidation anywhere in this kernel.** Every
invalidation is local: `vmm.c:245`, `:306`, `:325` and `vmm_cow.c:90`, `:131`,
`:173`, all bare `invlpg`. Batching a mechanism that does not exist is not an
optimisation — the row's precondition is unbuilt, so it is the DDR-1038 shape
(assessed, blocker named) and not a deferred speed-up.

### 3.1 Is the absence a defect? Measured: no, on four facts

Each checked in the tree rather than reasoned from the design:

**(a) No PCID and no global pages.**
`grep -rniE "PCID|PTE_GLOBAL|CR4_PGE|_PAGE_GLOBAL" kernel/` returns nothing.
So every CR3 load is a full TLB flush; nothing survives an address-space
switch.

**(b) CR3 is reloaded whenever the address space differs, including to and
from the kernel master** — `sched.c:1538-1543`:

```c
uint64_t kmaster  = vmm_kernel_cr3();
uint64_t prev_cr3 = prev->cr3 ? prev->cr3 : kmaster;
uint64_t next_cr3 = next->cr3 ? next->cr3 : kmaster;
if (next_cr3 != prev_cr3)
    __asm__ volatile("mov %0, %%cr3" :: "r"(next_cr3) : "memory");
```

A kernel thread's `cr3 == 0` maps to the master, so a CPU never *retains* a
user address space while running something else — the lazy-TLB hazard other
kernels carry does not arise here.

**(c) No two threads share an address space.** The only writers of `->cr3`
are `elf.c:315` (a fresh AS), `sys_exec.c:137` (a fresh AS), `sched.c:1105`
(`0` = master) and `sched.c:1239` (fork's fresh child AS, from
`sys_fork.c:41`). There is no `CLONE_VM` — DDR-1038 established that. So a
user address space is loaded on **at most one CPU at a time**, and the CPU
that unmaps a page is the only one that could have cached it.

**(d) Present-entry changes to the kernel master all precede AP bring-up.**
`vmm_protect_kernel()` is `main.c:3728`; `smp_start_aps()` is `main.c:3824`.
The device MMIO maps that *do* run afterwards (`pcie_init` :3845, `ahci_init`
:3854, `virtio_net_init` :3856, `nvme_init` :3874) are **not-present →
present** transitions, which x86 does not require an invalidation for. And
the kernel heap never changes kernel page tables at all — `kheap.c` calls
`pmm_alloc_page()` and dereferences through the identity map; there is no
`vmm_map` in it.

So: absent, and correctly absent. **No defect is named and none is fixed.**

### 3.2 The trip-wire — this is the part that bears on the backlog

**Fact (c) is exactly what Group D's `pthread` / `clone(CLONE_VM|CLONE_FILES|
CLONE_THREAD)` row would delete.** The moment two threads share one address
space and can run on two CPUs, every `vmm_unmap` (`sys_mmap.c:67`,
`sys_surface.c:388`/`:448`) and every `vmm_protect_range` (DDR-1031's
`SYS_MPROTECT`) leaves a stale, still-writable translation on the other CPU —
a read or write to freed or re-protected memory, silent, timing-dependent,
and on the same SMP paths OPEN-2 lives in.

**Nothing in the tree would notice.** There is no assertion, no counter, no
gate, and no comment at any of those unmap sites saying the invalidation is
single-CPU by assumption. `sys_mmap.c:67` says only *"active AS == cr3 during
a syscall"* — true today, and true only because of (c).

Recorded here so the pthreads row cannot be picked up without it. **A TLB
shootdown is a prerequisite of `CLONE_VM`, not a Phase 9 optimisation** — and
that ordering is the opposite of what the two backlog tables imply, since
Group D's pthreads row lists no dependency at all.

---

## 4. Row 9.6 — the wrong instrument, and a larger asymmetry underneath

Row: *"page-table walker SIMD — SSE2 for bulk zero-page mapping."*

### 4.1 The row conflates two subjects

"Page-table walker" and "bulk zero-page mapping" are different things. The
walker (`table_at` / `map_core`, `vmm.c`) is a four-level **pointer chase** —
dependent loads, one 8-byte entry per level. SIMD has nothing to offer it;
its cost is memory latency, not data width.

### 4.2 The asymmetry the row did not name

```c
/* kernel/string.c:4 */
void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++)
        d[i] = (unsigned char)c;
    return dst;
}

/* kernel/string.c:18 */
void *memcpy(void *dst, const void *src, size_t n) {
    return fast_memcpy(dst, src, n);          /* DDR-871: ERMS / REP MOVSQ */
}
```

**`memcpy` was optimised and `memset` was left at one byte per iteration.**
Where that lands, measured:

| site | size | frequency |
|---|---|---|
| `kheap.c:174` `memset(ptr, POISON_FREE, c->obj_size)` | up to **512 B** (pcb cache) | **every `kfree`** — `KHEAP_DEBUG` is unconditionally `1` (`kheap.c:20`) |
| `kheap.c:326` | 4096 B | every slab growth |
| `vdso_page.c:18`, `metric_page.c:22`, `virtio.c:15-17`, `nvme.c:179` | 4096 B | init only |

DDR-986 already recorded that the `kfree` path pays "a 128-byte `memset` on
EVERY `kfree`, unconditionally under `KHEAP_DEBUG`" — it read that cost as
the *reason a further 8-byte store is noise*, which was right, and nobody
noticed the `memset` itself is a byte loop.

### 4.3 The instrument the row names is the wrong one

SSE2 would work — `ipc_copy.asm` establishes that XMM is safe in ring 0 here,
because FXSAVE covers XMM0-15 on every context-switch path. But it buys
nothing over the mechanism already chosen for the sibling function, and it
drags in a state-safety argument that `rep stosb` does not have:

* **`rep stosb` under ERMS** — general-purpose registers only, so no FPU/XMM
  state question arises at all,
* **and the CPUID probe already exists**: `fast_memcpy_init()` (`main.c:3721`)
  records the ERMS bit today. No new gate, no new probe.

This is the same wrong-instruction shape DDR-873's own header recorded for
MOVDQU-vs-VMOVDQU: *"a case where the queue's suggested instruction is the
wrong one, and the reason is worth recording rather than silently
substituting."* Second instance, same group.

### 4.4 `fast_memset` — buildable, NOT built, trap named

This is the one genuinely buildable Group G item. It is not built here, and
the trap is named so the next attempt does not walk into it:

> **`memset` is called before `fast_memcpy_init()` runs.** Early boot zeroes
> memory long before `main.c:3721`. The dispatch variable's **default must be
> the safe generic path**, exactly as `fast_memcpy`'s is — and a mutant that
> reverses the default **would pass every gate**, because the CI CPU
> advertises ERMS, so the wrong default is never taken there. The mutation
> that proves this arm has to run on a CPU *without* ERMS, i.e. it needs its
> own `-cpu` pin, the way DDR-1040's `smoke-smep` pins `qemu64,+smep`.

Its proof is (per §1) a **static instruction count** plus a correctness
self-test at the boundaries — `n = 0, 1, 7, 8, 4095, 4096` and a non-zero
fill byte, since a `rep stosq` + tail implementation gets exactly those wrong
— **not** a TCG cycle figure. `smoke-bench` is the regression witness, not
the proof.

---

## 5. Rows 9.1, 9.2, 9.4 — what the tree says

**9.1 — hot-path `kputc`.** `console.c:146`. The body is a UART THRE poll,
and the loop deliberately drains RX inline:

> *"DDR-809/DDR-808: kputs/kwrite hold the console lock with interrupts OFF
> for the whole buffer, so IRQ4 cannot run and COM1's 16-byte RX FIFO cannot
> be drained by its handler. A burst of output therefore destroyed concurrent
> console input — measured as one lost command per smoke-shell run."*

The cost is **the device, not the instruction count**: at 115200 baud a byte
is ~87 µs on the wire, against which no instruction-count change is visible.
The only real lever is the 16-byte TX FIFO (write up to 16 bytes per THRE),
which changes timing on the path all 177 gates assert on — and this exact
function already carries a standing refusal: DDR-916's per-character drain
was *"tested TWICE and reverted both times … Do not re-add without new
evidence."* An assembly rewrite is the wrong instrument.

**9.2 — context-switch critical path.** Already annotated by DDR-870:
**17 instructions, 14 stack accesses (112 bytes), no locks, no serialising
instruction**, and the file is 21 non-comment lines. The header states why
the saved set is what it is:

> *"the register set saved here is deliberately only the SysV callee-saved six
> plus RFLAGS — each extra register would add two more dependent memory
> operations to every switch in the system."*

There is no fat to remove. A "cycle count reduction" would have to **drop
saved state**, which is a correctness change, not an optimisation. This row
is effectively already delivered by DDR-870's analysis, which concluded the
path is bounded by store-buffer and L1 latency rather than instruction count.

**9.4 — virtio-blk submission batch path.** Exactly **one** notify site,
`virtio_blk.c:274`, and the submitter **blocks immediately after it**
(`sched_block_timeout`, :288). So there is nothing to batch *within* a
caller; coalescing *across* callers means one submitter declines to ring the
doorbell and relies on another arriving to ring it — a liveness hazard on the
one path that already carries a 500-tick watchdog and DDR-976's dest-CPU
tick instrumentation for exactly this failure shape.

**And the standard mechanism is not assembly at all.** `VIRTIO_RING_F_EVENT_IDX`
lets the *device* say when a kick is owed; this driver does not negotiate it —
`virtio_blk.c:364` asks for `VIRTIO_F_VERSION_1 | VIRTIO_BLK_F_SIZE_MAX |
VIRTIO_BLK_F_SEG_MAX` and nothing else. So 9.4's real form is a feature-
negotiation change in C, recorded and not built.

---

## 6. The header count

> `### GROUP G — Phase 9 Assembly Optimization (6 of 7 items ⬜)`

The table lists **six** rows (9.1–9.6), and `docs/NEXT_TASK_QUEUE.md:381`
says "Phase 9.1–9.6". **No seventh item exists anywhere in the tree.** The
shipped assembly work is not it either: `fast_memcpy.asm`, `ipc_copy.asm` and
the cost annotations label themselves **"Group 8 items 42/43/44"** (DDR-871 /
873 / 870) in their own headers — a different group. Whether the count
silently folds one of those in cannot be determined and is not asserted here.

Same class as the counts DDR-1063, DDR-1071 and DDR-1072 corrected: a number
carried forward rather than measured.

---

## 7. What this changes

Group G reads, after measurement, as:

* **9.1** — wrong instrument; the lever is the TX FIFO and it is refused by
  DDR-916's standing note.
* **9.2** — analysed and at its floor (DDR-870); a reduction would drop state.
* **9.3** — **no subject**; a shootdown does not exist, is currently correctly
  absent, and is a **prerequisite of Group D's pthreads row**, not a Phase 9
  item.
* **9.4** — real, but a C feature-negotiation change (`EVENT_IDX`), not
  assembly; coalescing by hand is a liveness hazard.
* **9.5** — mechanism SHIPPED (DDR-873); the "single-copy" claim is blocked on
  the same cross-address-space problem as `SYS_FUTEX`.
* **9.6** — wrong instrument; the real gap is `memset`, and **`fast_memset` is
  the one buildable item**, with its default-path trap named.

**One buildable row out of six**, and the group's stated acceptance criterion
needs replacing before any of them can be closed honestly.

---

## 8. Not claimed

* No code change. `kernel.bin` untouched. 177 gates unchanged (Markdown only).
* **No gate was re-run**, locally or in CI, for this DDR.
* **No profile was run.** Every figure quoted is a static count read out of the
  tree, or one DDR-870 recorded and labelled as emulated.
* **§4's `fast_memset` is NOT built**, and no decision is taken on whether it
  should be before the ISO.
* **§3 names no defect and fixes none.** The shootdown is absent and, on the
  four facts in §3.1, correctly absent today. §3.2 is a statement about a
  *future* change, not a report of a present bug.
* **§5's 9.4 is not re-assessed as work** — `EVENT_IDX` is recorded, not
  scheduled, and nothing is claimed about its benefit, which would need the
  measurement §1 says this environment cannot produce.
* No open issue moves. OPEN-1 / OPEN-2 / OPEN-12 / OPEN-13 untouched.
* No pre-approved exception is revisited.

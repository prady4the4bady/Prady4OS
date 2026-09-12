# DDR-1108 — `SYS_IO_URING_ENTER` honours a caller-controlled intra-page offset

**Status:** BUILT. The finding is a **reachable ring-3 write past a validated
region**, in shipped code gated at strict tier on every CI suite. Per
§NON-NEGOTIABLE 3 the artefact is produced **before** the fix, and the probe
deliberately uses a **safe in-page offset** rather than the corrupting one.

**Found by measuring a row the backlog audit had already recorded and declined
to schedule.** DDR-1102 §2 closed out the Group D io_uring row with a fifth gap
it named but did not cost: *"'no head/tail wrap' is its own limitation and is
DISTINCT from SQE chaining; recorded, not scheduled."* Reading the source to
cost that sentence found something else, and the sentence understates it twice
over — §5.

---

## §1 — The mechanism, derived from the source and not from the shape

`sys_io_uring_enter` (`kernel/syscall/sys_io_uring.c:118`) takes the ring's user
VA as its first argument and applies exactly two checks to it:

```c
if (!vmm_user_range_ok(t->cr3, va & ~0xFFFull, PAGE_SIZE, 1))
    return -EFAULT;
uint64_t phys = vmm_resolve(t->cr3, va & ~0xFFFull);
if (!phys)
    return -EFAULT;
struct io_ring *r = (struct io_ring *)(uintptr_t)(phys + (va & 0xFFFull));
```

**There is no alignment check anywhere.** Both checks are applied to the
**page-aligned base**; the kernel pointer is then formed at the **caller's
intra-page offset**, and `run_sqe` reads `r->sqes[i]` and the loop writes
`r->cqes[i]` through it.

`phys` is usable as a kernel pointer because stage2 identity-maps the **low
1 GiB** with 2 MiB pages (`boot/stage2/stage2.asm:13`, `:520`, `:535`), and
`PMM_MIN_PHYS` is 16 MiB — so every ring frame is inside that window. Checked,
not assumed; without it the existing gate could not pass at all.

**`sizeof(struct io_ring)` is 416 B, derived rather than taken from the
`_Static_assert`:** header `4 x uint32 + 2 x uint32 + uint32[2]` = 32; `sqes` =
`8 x 32` = 256, at offsets 32…288; `cqes` = `8 x 16` = 128, at 288…416.

So the structure leaves the frame whenever

```
(va & 0xFFF) + 416 > 4096      i.e.      (va & 0xFFF) > 3680
```

and at `(va & 0xFFF) = 0xFF0` **all eight SQEs are read from, and all eight CQEs
are written to, the physically adjacent frame** — which the caller does not own,
which `vmm_user_range_ok` never examined, and which the PMM may have handed to
another process, to the slab allocator, or to a page table.

**The `_Static_assert(sizeof(struct io_ring) <= 4096)` is not the protection and
was never able to be.** It bounds the structure against a page; the quantity
that matters here is `offset + sizeof`, and nothing bounds that.

---

## §2 — Reachability, enumerated

- **No capability gate.** `sys_io_uring_register()` calls `syscall_register` for
  both numbers and `grep` for `cap_ok|cap_authorize|is_agent|is_sovereign` over
  the whole file returns **nothing**. NSI 26 is callable by **any ring-3
  process** — PRISM, any probe, any spawned agent.
- **`SETUP` is not a prerequisite.** `ENTER` never checks that the VA came from
  `SETUP`; it needs only a user-RW page, which every process has.
- **This is the only site of the shape in the tree**, enumerated rather than
  assumed: `grep -rn vmm_resolve kernel/ --include=*.c` outside `vmm.c` returns
  three call sites. `sys_mmap.c:66` resolves a **page-aligned** VA in
  `unmap_range`'s own loop and adds no caller offset; `main.c:3811-3812` is a
  kernel COW self-test comparing two resolutions. Neither forms a kernel pointer
  at a ring-3-controlled displacement. **One site, and it is this one.**

---

## §3 — The strength of the claim, stated at its real size

This is **not** an arbitrary-address write and is not described as one. What a
caller obtains is:

- a write into the frame **physically adjacent** to a frame it owns,
- bounded at **416 bytes** past that frame's end,
- at an offset it chooses within that window (via `va & 0xFFF`),
- of values it substantially controls — `cqes[i].user_data` is copied verbatim
  from `sqes[i].user_data` (8 caller bytes) and `cqes[i].res` is an `int`
  result,
- with *which* frame is adjacent influenced, not chosen, through allocation
  order.

That is a **memory-corruption primitive**, not an arbitrary write, and the
difference is worth keeping: inflating it would be the DDR-1059 shape in
reverse — a claim reading considerably stronger than what was measured.

There is also a **read** side at large offsets (the SQEs themselves come from
the adjacent frame), but it is **not an information leak to ring 3**: the values
land in `cqes`, which at those offsets are also outside the caller's page, so
the caller cannot read them back. Recorded so nobody later reports a leak that
does not exist.

---

## §4 — Why no gate could see it, measured in the probe

The only ring-3 consumer in the tree is `user/systest.asm:597-648`, and it:

1. passes `r14` — the VA `SETUP` returned — which is **always page-aligned**,
   because `t->mmap_next` starts at `VMM_MMAP_BASE = 0x8800000000` (aligned) and
   advances only by whole `PAGE_SIZE` steps (`sched.c:1144`, `sys_exec.c:141`,
   `sys_mmap.c:141`, `sys_io_uring.c:70`); and
2. calls `ENTER` **exactly once**.

So the unaligned path has **never executed**, on any gate, ever. `smoke-sysiouring`
is correct for what it asserts (a batched write-then-read on a pipe in one
`enter`, requiring `IO_URING: batch read OK`) and its arms are live; **the set of
arms simply does not span the argument space** — the DDR-1070 class, not the
dead-arm class.

---

## §5 — THE SECOND FINDING: the header understates its own limitation twice

`sys_io_uring.h` says *"Baseline: no head/tail wrap, no kernel-side polling
thread."* Measured, the index fields are not merely un-wrapped:

- **`sq_head`, `sq_tail` and `cq_head` have ZERO kernel writers and ZERO kernel
  readers.** `SETUP` zeroes the page and writes only `entries`; `ENTER` writes
  only `cq_tail`. Nothing in the kernel ever reads any of the three.
- `ENTER` always runs `sqes[0 .. to_submit)` and always writes `cqes[0 .. done)`
  — **indexed from zero on every call**, not from a head.
- `r->cq_tail = done` is an **assignment, not an accumulation**.

Two consequences follow and neither is a wrap:

1. A caller following the real io_uring protocol — publish an SQE at `sq_tail`,
   bump `sq_tail`, call `enter` — gets the **wrong SQE executed** from its
   second call onward, silently. On the first call index 0 and `sq_tail` happen
   to coincide, which is exactly why the shipped probe works.
2. A second `enter` **overwrites** `cqes[0..done)`, destroying completions the
   caller had not consumed.

**NOT FIXED HERE, and that is DDR-1069's test rather than difficulty:** a real
index discipline is a ring rewrite plus an ABI contract, and **nothing shipping
needs it** — the one consumer issues a single `enter` and never reads an index.
What is fixed is the **wording**, in the same commit, so the next session costing
this row reads what is actually true (the DDR-1084 §1 / DDR-1107 pattern applied
to a source comment). Group D's row is **corrected, not closed**.

---

## §6 — THE VACUITY CHECK, done before the arm was written

*(Eighteenth time caught in design text.)*

- **"assert `enter` still works"** — vacuous; that is the existing arm and it
  passes on the unfixed tree by construction.
- **"assert `enter(unaligned)` returns < 0"** — **not** vacuous (the unfixed
  tree returns `1`, a success) but **weak**: a future change making
  `vmm_user_range_ok` reject the call for an unrelated reason would return
  `-EFAULT` and satisfy it while the alignment guard was absent. The arm
  therefore asserts the **exact** `-EINVAL` (DDR-1044's discipline).
- **"assert the exact `-EINVAL`"** — still **not sufficient on its own**, and
  this is the one worth recording: a kernel that ran the SQEs and *then*
  returned `-EINVAL` would pass it. The refusal has to be shown to be a refusal
  **to act**, not merely a return code.

**How the second half is obtained without adding a fragile assertion:** the
poison SQE writes a 2-byte payload into the same pipe the existing arm uses.
If the refused call had executed, the pipe would hold `"XXURING"` and the
existing arm's 5-byte read would return `"XXURI"`, failing its own byte
comparison — which it already makes. **The existing assertion becomes the
did-not-execute check**, at the cost of one ordering constraint and no new
assertion to keep in step.

---

## §7 — The probe uses a SAFE offset, deliberately

The probe passes `ring_va + 96`, **not** an offset past 3680.

At `K = 96` the structure sits at page offset 96: `sqes[0]` at **+128** and
`cqes[0]` at **+384**, both comfortably inside the page and both disjoint from
the bytes the existing arm uses (`+32…+96` for its two SQEs, `+296` and `+312`
for its two CQE results). Nothing outside the caller's own page is touched.

**What that demonstrates is the mechanism — that `ENTER` honours an arbitrary
caller offset — and the out-of-page case follows from the same arithmetic with
no further mechanism required.** Stated plainly rather than implied: this DDR
does **not** exhibit memory corruption, and does not attempt to, because a probe
that corrupted an unrelated physical frame would make its own gate fail for
reasons nobody could attribute, and could genuinely damage the boot it runs in.

---

## §8 — The fix, and why the stronger guard

```c
if (va & 0xFFFull)          /* DDR-1108 */
    return -EINVAL;
```

placed **before** the range check, after which `(va & 0xFFF)` is provably zero
and the pointer becomes `(struct io_ring *)(uintptr_t)phys` — so the invariant
is visible in the code rather than argued in a comment.

**The weaker bound was considered and refused.** Permitting any offset with
`(va & 0xFFF) + sizeof(struct io_ring) <= PAGE_SIZE` would also be correct, and
it is worse: it leaves `ENTER` reading a ring from wherever inside their page a
caller points, which **nothing needs** — `SETUP` only ever returns page-aligned
VAs — while making the safety of every future field addition depend on a
`sizeof` that changes whenever the structure does. Page alignment is the
narrowest guard that matches what `SETUP` actually produces.

**Safe for every shipping caller, measured not assumed:** the only caller is
`systest.asm`, and it passes `SETUP`'s return value unmodified.

---

## §9 — PROOF: the pre-fix tree is the control, and both sentinels flip together

**The pre-fix tree IS the mutant** (the DDR-1066/1067/1090 form) — no synthetic
defect was written. The probe arm is identical in both rows below; only the
one-line kernel guard differs.

| tree | `kernel.bin` | `unaligned ring VA refused` | `batch read OK` | rc |
|---|---|---|---|---|
| **pre-fix** (probe only, kernel untouched) | `9ef04b09f388ae2d` | **0 — ABSENT** | **0 — ABSENT** | 1 |
| **fixed** | `68e74ff4142f7c71` | **1** | **1** | 0 |

**Both halves of the artefact are in the pre-fix row, and the second is the one
that carries the claim.** The first absence says only that no guard existed. The
second says the refusal was not a refusal at all: the unaligned call **acted**,
writing `"XX"` into the pipe, so the aligned arm's 5-byte read returned `"XXURI"`
and failed its own byte comparison.

**And the probe demonstrably reached that arm rather than jumping out of it** —
checked in the capture, not assumed: `SIGNAL: SIGUSR1 caught` at line 224
precedes the io_uring block and `EXECVE: new image running` at line 318 follows
it, so the probe ran through. That is DDR-1089 §6.1's discipline paying for
itself; the first draft of this arm branched to `.uring_done2` on mismatch,
which would have skipped the aligned arm entirely and made the pre-fix row
**ambiguous** — "the poison executed" and "we never got there" would have been
the same observation.

**The isolation is the fixed row.** Same probe, same poison SQE written at page
offset +128, same pipe. If the poison SQE's mere *presence* had been what
suppressed `batch read OK`, it would still suppress it after the fix. It does
not. Only its *execution* did.

**Revert returns `9ef04b09f388ae2d` BIT-FOR-BIT**, verified by rebuild rather
than assumed.

**`kernel.bin` is 1,315,210 B in BOTH rows — SIZE UNCHANGED**, so the
size/headroom pair and `ci-docstate-check` are unaffected, and **a size
comparison cannot tell the two binaries apart at all**; only the hash
discriminates. DDR-1097's finding arriving again, third time.

**No new gate** (179 unchanged): the arm goes on `smoke-sysiouring`, the
DDR-1039/1070 reasoning. `GLOBAL_FORBIDDEN` is **not** touched (77 unchanged) —
the check is deterministic and its own gate asserts both directions, so it
cannot hide in a green run; DDR-1065's reasoning, as against DDR-981/1049's
intermittents.

---

## §10 — NOT CLAIMED

- **No index discipline is built** (§5) and the io_uring row is **corrected, not
  closed** — `OP_FSYNC`, `OP_OPENAT`, eventfd and SQE chaining remain unbuilt,
  as does the wrap.
- **No arbitrary-write primitive is claimed** (§3) — the write is bounded at
  416 B into a *physically adjacent* frame.
- **No information leak to ring 3 is claimed** (§3).
- **No corruption is exhibited** (§7); the probe stays inside its own page.
- **No capability gate is added.** Whether NSI 25/26 should be gated at all is a
  DDR-842 S4 **policy** question — the DDR-793/982 class this project defers to
  the operator. **Recorded, not taken.**
- **No claim that this was ever exploited**, and no open issue moves — this is
  not an `[apfreeze]`, not OPEN-2, and OPEN-1/2/12/13 are untouched.

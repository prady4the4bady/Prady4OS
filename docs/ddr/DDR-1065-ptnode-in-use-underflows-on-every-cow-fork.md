# DDR-1065 — `ptnode_in_use` UNDERFLOWS ON EVERY COW FORK: the artefact, then the fix

**Status:** ARTEFACT PRODUCED (`smoke-sharedpte`, the DDR-1003 §5.1 gate) + FIXED
(DDR-1003 §5.2's narrow version) + M1 = the pre-fix tree.
**Date:** 2026-09-06
**Closes:** the Group E `smoke-sharedpte` row, unbuilt since DDR-1003.

---

## 1. Why this is being done NOW, when DDR-1003 deliberately did not

DDR-1003 §5 recorded the defect and refused to fix it, for two stated reasons:

> **No gate observes this today, so there is no failing artefact**, and
> §NON-NEGOTIABLE 3 forbids the fix without one.

> Changing a memory-accounting path days before a release … is also the wrong
> trade on its own merits.

**The first reason is addressed by building the gate** — that is the entire point
of the ordering here, and it is why the gate lands in the same change as the fix
rather than after it. **The second reason has expired:** the release is HELD
indefinitely pending OPEN-1/2/12/13 and no promotion is in flight, which is the
same circumstance DDR-1061 used to justify registering a gate that might redden.
"Days before a release" was true when DDR-1003 wrote it and is not true now, and
saying so explicitly is better than quietly acting against a recorded decision.

DDR-1003 also named the cost of leaving it: *"a loaded gun: the next person to add
a leak gate spanning a `fork` will get a wrapped counter and a mystery."*

## 2. The mechanism, VERIFIED AGAINST THE TREE rather than taken from DDR-1003

Every claim below was checked in source for this DDR, because this session has
repeatedly found DDR text that no longer matched the tree. It **holds**:

1. `kheap.c:322` `ptnode_alloc()` — `pmm_alloc_page()` then `ptnode_in_use++`.
   **One frame, one increment.**
2. `vmm_cow.c:101` `pmm_incref(frame)` — *"parent + child now share it"*. This is
   the **only** `pmm_incref` call site in the kernel (measured). Refcount 1 → 2.
   **No second increment**, correctly: no new frame was allocated.
3. `pmm.c:192-198` `pmm_free_pages()` at order 0 with `refcount > 1`:
   **decrements the refcount and RETURNS WITHOUT FREEING.**
4. `kheap.c:333` `ptnode_free()` — calls `pmm_free_page()` and then decrements
   `ptnode_in_use` **unconditionally**, whether or not a frame was released.
5. `vmm.c:371` `free_subtree()` frees leaf user data pages with `ptnode_free`,
   skipping only `PTE_SW_SHARED`. **A COW page carries `PTE_SW_COW` (0x200), not
   `PTE_SW_SHARED` (0x400)** — different bits (`vmm.h:22-23`) — so a COW-shared
   page is `ptnode_free`'d from **both** address spaces.

**Net: one `++` at allocation, TWO `--` at teardown, ONE actual frame release.**
`ptnode_in_use` loses 1 per COW-shared frame, permanently, and
`kheap_outstanding()` sums it, so the whole counter drifts down and eventually
wraps.

## 3. The gate — and why the OBVIOUS shape would have tested nothing

DDR-1003 §5.1 warns that the ordinary leak shape (fork, **child writes**, both
exit) is balanced and would **pass**. That is not a footnote; it is the reason
this gate has to be built to a specific design rather than the natural one.
A gate built the obvious way is the dead-arm class again.

`smoke-sharedpte` therefore forks and lets the child **exit WITHOUT writing**:

```
before = kheap_outstanding()
parent = vmm_new_address_space()
pf     = ptnode_alloc()                       /* ptnode_in_use++  -- ONE */
vmm_map_in(parent, va, pf, USER|RW|NX)
child  = vmm_fork_address_space_cow(parent)   /* pmm_incref -> refcount 2 */
                                              /* NO vmm_cow_fault: the point */
vmm_destroy_address_space(child)              /* -- , refcount 2->1, NO release */
vmm_destroy_address_space(parent)             /* -- , releases */
after  = kheap_outstanding()
```

**Correct kernel: `after == before`. Defective kernel: `after == before - 1`.**

**It uses the REAL fork path**, `vmm_fork_address_space_cow()`, not a
reconstruction of it. That matters because of DDR-1014's rule — a proof that
paraphrases the kernel is testing the paraphrase — and it is why this arm is
modelled directly on `cow_selftest` (`main.c:3596`), which already exercises the
same path and differs only in that it *does* fault the child in. The one-line
difference between the two probes is exactly the difference between the balanced
case and the defective one, which is the clearest possible statement of §5.1.

Kernel-side and deterministic: no ring-3 process, no reap polling, no timing, so
this is not an SMP/intermittent gate and §NON-NEGOTIABLE 2's 20x rule does not
apply. The stated N is **1** — the arithmetic is exact.

## 4. The fix — DDR-1003 §5.2's narrow version, and what it refuses

`pmm_free_pages()` returns `void` today, so `ptnode_free` **cannot know** whether
a frame was released. It now returns `int` (1 = released, 0 = reference dropped),
`pmm_free_page()` forwards it, and `ptnode_free` decrements **only on a real
release**.

**Blast radius is zero for existing callers:** 132 call sites ignore the return
value, and `void`→`int` is source-compatible in C with no `warn_unused_result`
attribute in play, so nothing else changes and nothing else needs to.

**REFUSED, and DDR-1003 §5.2 named it in advance:** do *not* fix this by having
`vmm_cow_fault` call `ptnode_free` instead of `pmm_free_page` — that decrements
on a path where no frame was released and merely moves the imbalance.

## 5. NOT CLAIMED

- **No leak is fixed, and none existed.** Frames were always released correctly;
  the *counter* was wrong, not the allocator. This is an accounting fix.
- **The two existing `kheap_outstanding()` readers were never wrong** (`vmm_test`,
  `kheap_stress`): both difference across a window in which nothing forks, so a
  constant offset cancels — DDR-1003 §5's own point, re-verified here.
- **No open issue moves.** OPEN-1, OPEN-2, OPEN-12 and OPEN-13 are untouched.
- ~~The wrap itself is not demonstrated~~ — **THIS DDR'S OWN DRAFT WAS WRONG AND
  THE MEASUREMENT CORRECTED IT.** §5 originally read *"the wrap itself is not
  demonstrated, only the per-fork decrement; driving the counter to actual
  wraparound needs ~2^64 forks."* **It needs one fork.** The pre-fix capture reads
  `before=0 after=18446744073709551615` — `0xFFFFFFFFFFFFFFFF`, i.e. **−1**,
  because `kheap_outstanding()` is legitimately **0** at that point in boot, so a
  single unmatched decrement wraps it immediately. The wrap is *demonstrated*, not
  merely predicted, and the defect is correspondingly worse than the draft said.
  Recorded as a correction rather than silently rewritten, because the direction
  matters: I under-stated it, and reading the actual output is what caught that.

## 6. Two-sided proof, on recorded hashes

| kernel | reading |
|---|---|
| `e256aa4802882aa6` — **M1, the pre-fix tree** (gate present, fix absent) | `[vmm] SHAREDPTE before=0 after=18446744073709551615  SHAREDPTE FAIL` |
| `a9d8bc933595ec0d` — fixed | `[vmm] SHAREDPTE before=0 after=0  PRADYOS_SHAREDPTE_OK` |

**M1 is not synthetic** — it is literally the tree with the gate and without the
fix, the DDR-1046 construction. And the two pre-existing readers are unaffected in
the same capture, which is DDR-1003 §5's claim re-verified rather than assumed:
`kheap stress — base=0x0 after=0x0 (no leak)` and
`vmm unmap reclaim — 0x0 -> 0x0 (clean)`.

**GLOBAL_FORBIDDEN is deliberately NOT touched.** `SHAREDPTE FAIL` is asserted by
`smoke-sharedpte`'s own `FORBIDDEN_SENTINEL` and the OK marker by its
`EXTRA_SENTINEL`, so both directions are covered on its own shard. The precedents
for a global entry (DDR-981's `[apfreeze]`, DDR-1049's `panic_stage=`) are both
**intermittent** defects that could otherwise hide in a green run; this one is
deterministic and cannot. §NON-NEGOTIABLE 6 documents at length how fragile that
list's terminator is, so it earns an entry only when a dedicated gate is
insufficient — and here it is sufficient.

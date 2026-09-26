# DDR-1073 — Groups A and B: a subsystem built at three layers and marked
# nowhere, two more name-matching gates that cover a different claim, a remedy
# prescribed for a closed issue — and one real coverage gap

**Date:** 2026-09-06
**Status:** measured. **Docs only — no code change, `kernel.bin` untouched.**
**Classes:** DDR-1063 §7b (a live-state row that stopped tracking the tree);
DDR-1072 §2 (a gate whose NAME matches a row and whose CLAIM does not), third
and fourth instances; DDR-1068 §2 (a recorded refusal listed as remaining work);
and **one new shape** (§5) plus **one genuine coverage gap** (§2), which is the
finding worth carrying.

---

## 1. B#10 "NUMA affinity" is built at three layers, gated twice at strict tier, and marked nowhere

Measured against the Makefile, `tools/ci/gate_shards.txt` and the source:

| layer | where | gate | shard | tier |
|---|---|---|---|---|
| SRAT topology parsing | `kernel/mm/numa.c` | `smoke-numa` (Makefile:1220) | 2 | **strict** |
| per-node PMM free lists + node-targeted alloc (DDR-882 17b) | `pmm.c:152/172/233/350` | `smoke-numa-alloc` (:1215) | 5 | **strict** |
| **NUMA-affine steal order (DDR-885)** | `sched.c:776-808` | see §2 | — | — |

Neither gate is in `shard_check.sh:50`'s exclude set, so both run on every suite.
`pmm_alloc` prefers the caller's node (`pmm.c:172`) and falls back across nodes
(`:177`); `rq_steal` tries same-node victims first and only then anywhere. The
row carried `— | smoke-numa`: no detail, no marker, and no mention of the second
gate or of the scheduler layer at all.

---

## 2. THE FINDING: no gate exercises the remote-steal pass, so DDR-885's ORDERING claim is untested

`rq_steal` (`sched.c:801`) makes two passes:

```c
struct tcb *t = steal_pass(self, node, 1);   /* same node first */
if (!t)
    t = steal_pass(self, node, 0);           /* then anywhere */
```

`steal_pass` filters with `if (same != (cnode == want_node)) continue;`.

**Measured, not assumed:** `numa_node_of_cpu` (`numa.c:194-198`) returns **0**
for any APIC id with no SRAT entry. So on a boot without `QEMU_NUMA=1`, every
CPU is node 0, pass 1 matches **every** victim, and **pass 2 is unreachable** —
`g_steal_remote` is structurally zero.

Now the gates, and this is the whole of it:

- **`grep -n QEMU_NUMA Makefile` returns exactly two lines**, 1216 and 1221 —
  `smoke-numa-alloc` and `smoke-numa`. **Neither sets `QEMU_SMP`**, and
  `boot_test.sh:215` only passes `-smp` when that variable is set, so both run
  **single-CPU**. With one CPU there is no stealing at all.
- The only gate that asserts the steal counters is **`smoke-rqstress`**
  (`EXTRA_SENTINEL` includes `[sched] steal local=`), and it runs
  `QEMU_SMP=4` with **no NUMA**, i.e. one node.

**So no gate in this tree runs SMP and NUMA together, and nothing anywhere
exercises the second pass.** `[sched] steal local=` is a live arm — it fails if
the counter stops printing — but it cannot distinguish *"NUMA-affine steal
order"* from *"steal from whoever has work"*, because on its own machine those
are the same program.

This is the **DDR-1070 class, not the dead-arm class**: the arm can fail, and
the question is whether the SET of arms spans the claim. It does not. The claim
DDR-885 makes is about **ordering under more than one node**, and the two
conditions that produce it are never present together.

**NOT FIXED, and the fix is not free.** The gate would be `QEMU_NUMA=1
QEMU_SMP=4` plus an assertion that `remote` is reached only after `local` is
exhausted — and a bare `steal remote=N` count would be the wrong arm, because a
correct kernel legitimately reports `remote=0` whenever the local pass always
succeeds (the DDR-1068 `reaped=` shape exactly). Making `remote` non-zero
on demand needs work pinned onto one node's CPUs, which this scheduler has no
API to express. **Recorded as a measured gap with its trap named**, not built:
§NON-NEGOTIABLE 3 governs fixes, and this is a coverage question with no failing
artefact behind it.

---

## 3. `smoke-sfs-persist` — the DDR-1072 §2 trap, third instance, and this one would claim a DEFERRED feature

Group B's row is **"SFS on-disk free-tree persistence"**, gate cell
`smoke-sfs-persist`. That gate exists (Makefile:2164) and is registered
(**shard 6, strict**). It is **DDR-768/769's cross-reboot persistence proof**:
`mkfs.sfs` authors a host image carrying `/PERSIST.TXT` and `/etc/test/config`
plus six padding files (so the tree is genuinely multi-leaf, DDR-773), the
kernel mounts it via `QEMU_SFS2` **without reformatting**, reads both back, and
asserts `PRADYOS_SFS_PERSIST_OK` / `PRADYOS_SFS_NESTED_OK`.

**It asserts nothing whatever about the free tree.** And two other places in
`CLAUDE.md` already say what the row's own subject is:

- §COMPLETED LAYERS: *"Cross-reboot SFS persistence (DDR-768/769) — COMPLETE"*,
  i.e. what the gate actually proves is already marked done elsewhere.
- §PRE-APPROVED EXCEPTIONS: *"SFS block reclamation on-disk — in-memory reclaim
  shipped (DDR-762-v2); **on-disk free-tree deferred post-1.0**"* — the row's
  actual subject is a **logged deferral**.

So closing this row by pointing at its named gate would claim an on-disk free
tree that does not exist, and contradict this file's own exceptions table. Same
trap as `smoke-sendipc` and `smoke-runexp`, with a worse consequence: those two
would over-claim unbuilt work, this one would over-claim **deliberately deferred**
work.

---

## 4. `smoke-sfs-gc` — fourth instance, and half of it is a refusal recorded in §INV.20

Group B's row is **"SFS B+tree CoW GC"**, gate cell `smoke-sfs-gc`
(Makefile:3283, **shard 5, strict**). The gate is **DDR-762-v2's free-space
GC**: 300× create + write(64 KiB) + unlink, whose ~4,800 data blocks exceed the
~4,096-block volume unless each unlink's 16-block run is reclaimed and reused by
exact fit. That is **data-block extent reclamation**, and it is real coverage.

**It is not B+tree node collection**, which is what the row names. And that half
is a recorded decision, not a gap — §INV.20, corroborated in the source rather
than taken from the invariant: `sfs.c:614` (`inode_num == 0` = tombstone = not
found), `:633` (a tombstone slot is reusable), `:788` (`dir_walk` skips them),
`:1151-1156` (unlink overwrites the entry with a tombstone). §INV.20 states the
decision in as many words: *"There is NO B+tree structural delete. Do not
implement one."* Tombstones are **recycled by create**, so there is no garbage
for a collector to collect.

The row is therefore **corrected, not closed**: the free-space half is shipped
and gated at strict tier; the B+tree half is a refusal, and listing it as
remaining work invites the next session to build the thing §INV.20 forbids —
the DDR-1068 §2 failure mode, now seen three times (PRISM `bg`,
`SYS_NET_REVOKE`, the `CAP_*` rows) plus this.

---

## 5. A NEW SHAPE: a remedy prescribed, at line numbers, for an issue closed elsewhere in the same file

Group A's last row reads, in full:

> **smoke-smpuser fix** | Measure `g_ticks` at `main.c:1134` and `main.c:1311`.
> Branch (B) = large gap → scheduler starvation fix. | `smoke-smpuser`

Three things are wrong with it, and they compound:

1. **The issue is closed.** §OPEN ISSUES carries *"smoke-smpuser B#3 — NOT
   REPRODUCED 2026-08-22 … CLOSED as not-reproduced"*, and records that
   `[smp] user on AP OK` is present in every captured boot.
2. **The prescribed action was never runnable**, which that same closed row
   already notes: it says to insert `kprintf(...)`, and **`kprintf` does not
   exist in this kernel** (§INV.9 — the console API is `kputs`/`kputdec`).
3. **The line numbers have drifted, and this is new.** `main.c:1134` is now
   inside `aether_spawn_agent_hook` — an `elf_load` of the agent template — and
   `main.c:1311` is inside the RTC wall-clock print. Neither is a scheduler
   boundary; neither has anything to do with `g_ticks`.

Carry this separately from §1 and §3. §1 understates progress; §3 risks
over-claiming. **This one instructs a future session to instrument two arbitrary
places and read the difference as a diagnosis** — an answer with a number behind
it and nothing under it, which is the DDR-1042 failure mode arriving by
instruction rather than by accident. A row citing line numbers is a row with an
expiry date, and nothing in the tree can check one.

The gate itself is healthy and is **registered, shard 5, strict**, asserting
three sentinels (`[smp] user on AP OK`, `HELLO FROM RING-3`, `PRADYOS_MUSL_OK`)
with `user on AP FAIL` forbidden — i.e. it requires the user programs to run
**correctly** on an AP, not merely to run.

---

## 6. What this changes, and what it does not

**Changes.** Group A's remaining work is the two genuinely open rows (per-CPU
`sched_exit`/zombie reap under full SMP, and `smoke-rqstress` 20× determinism);
the `smoke-smpuser` row is retired to a closed-issue note. Group B reads as:
NUMA affinity **shipped and gated at three layers** with one ordering claim
uncovered (§2); cross-reboot persistence shipped and gated with the on-disk
free tree **deferred, not open**; free-space GC shipped and gated with B+tree
structural delete **refused, not open**. The genuinely unbuilt Group B rows are
the SFS boot root, large-file extents, deep slots, quotas, ext4 write, the NAS
scheduler, the PMM policy, and NVMe IRQ — every one of which has **no gate in
the Makefile at all**, which is the DDR-1063 §7c shape doing its job (a planning
table naming a gate for work not yet done) and is **not** counted here.

**The gate coverage did not change. Only the record of it did — except §2, which
records coverage the project does not have and believed it did.**

**NOT CLAIMED.**

- **No code change.** Markdown only; `kernel.bin` untouched, 177 gates unchanged.
- **No gate was re-run in this session.** What was measured is existence,
  registration, non-exclusion, the recipes' actual assertions (read in full for
  `smoke-numa`, `smoke-numa-alloc`, `smoke-rqstress`, `smoke-smpuser`,
  `smoke-sfs-persist`, `smoke-sfs-gc`), and the source behind them.
- **§2 is not fixed and no gate is built for it.** The trap that makes the
  obvious arm vacuous is named so the next attempt does not walk into it.
- **DDR-885 is not accused of being wrong.** The steal order is implemented and
  reads correctly; what is missing is a gate that could tell if it stopped
  working.
- **No open issue moves.** OPEN-1, OPEN-2, OPEN-12 and OPEN-13 are untouched.
- **No pre-approved exception is revisited** — the on-disk free tree and the
  B+tree delete refusal both stand exactly as recorded.

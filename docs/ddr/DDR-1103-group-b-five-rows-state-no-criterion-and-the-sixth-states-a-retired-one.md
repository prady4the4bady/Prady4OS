# DDR-1103 — Group B: five rows state no criterion, and the sixth states a retired one

**Status:** assessment, Markdown-only. No code change, no gate, no defect fixed.
**Rows:** the six Group B rows never assessed — `smoke-sfs-deepslot`,
`smoke-sfs-quota`, `smoke-ext4-write`, `smoke-nas`, `smoke-pmmpolicy`,
`smoke-nvmeirq`. **None of the six named gates exists.**

---

## §0 — The shape of the table, before any individual row

| # | row | detail column | verdict |
|---|---|---|---|
| 1 | `mkfs.sfs` >512 slots | **—** | genuinely unbuilt (§INV.22 caps it) — §7c correct |
| 2 | SFS free-space quotas | **—** | genuinely unbuilt; a **different** thing from ADR-032 |
| 3 | B#6 ext4 write | **—** | genuinely unbuilt; **ext4 READ is shipped + gated** |
| 4 | B#14 NAS 3-lane *storage* scheduler | **—** | **CATEGORY ERROR — NAS is the process scheduler** |
| 5 | B#15 PMM policy | **—** | **unfalsifiable**; NUMA-affine PMM policy is shipped + gated |
| 6 | B#1 NVMe IRQ | *"On hold until B#3 SMP is fully stable"* | **blocker RETIRED**; half built; logged deferral ×2 |

**Five of six state no acceptance criterion at all, and the sixth states one that
is no longer true.** A row consisting of a title plus a gate name that does not
resolve **cannot be closed or confirmed open by any measurement** — it is not
merely stale, it is unfalsifiable. That is a distinct shape from the ones already
recorded (§7b shipped-but-unmarked, §7c gate-for-unbuilt-work, `smoke-horizon`
half-done, DDR-1084 §1 stale blocker): those are all *wrong*, and can therefore be
*corrected*. These five cannot be wrong, because they assert nothing.

---

## §1 — B#1 NVMe IRQ: the same item in this file three times, with a retired blocker, half built

**Three defects compounding.**

**(a) It is a logged deferral appearing as work — twice over.** §PRE-APPROVED
EXCEPTIONS carries *"NVMe completion IRQ | poll-mode sufficient for ISO;
DDR-774a/b/c deferred until B#3 SMP stable"* and §DEFERRED carries *"NVMe
completion IRQ (DDR-774a/b/c — after B#3 SMP stable)"*. So the **same item appears
in this file three times**, and the work copy does not say so. DDR-1072 §3 found
this shape twice; this is the first at **three**.

**(b) Its stated blocker is retired.** *"On hold until B#3 SMP is fully stable"* —
**B#3 is CLOSED.** §OPEN ISSUES: *"~~B#3 / DDR-806~~ … **ROOT-CAUSED AND FIXED —
DDR-981**"* (`yield()` spun with `RFLAGS.IF` clear; 20/20 at `-smp 4`). The
DDR-1084 §1 pattern once more, and again in the expensive direction: a stale
blocker suppresses work that is already unblocked.

**(c) It is half built, and the source contradicts itself.** `nvme.c:9` says
*"Completion is polled via the CQ phase bit (**no NVMe IRQ**)"* — while
`nvme.c:150` is **`msix_register(NVME_MSIX_VEC, nvme_msix_isr);`**. DDR-774b's
MSI-X plumbing is **present and registered** (vector 50, table entry 0); what is
absent is the completion path *using* it. The `smoke-horizon` shape.

**The verdict is unchanged and the reason is not.** It stays deferred — but on the
**exception's** ground (*"poll-mode sufficient for ISO"*), which is still true,
**not** on the blocker's, which is gone. A session that checked only the blocker
would conclude it is now free to build; a session that checked only the exception
would conclude it is forbidden. Both halves have to be stated together.

The driver itself is gated: `smoke-nvme` (`gate_shards.txt:136`, **shard 5, 60 s,
strict**).

---

## §2 — B#14 "NAS 3-lane storage scheduler" is a category error

`grep -rniE 'three-lane|3-lane|lane'` over `kernel/` returns, apart from Keccak's
FIPS 202 lanes (unrelated), exactly one relevant line — **`kernel/proc/sched.h:5`**:

> *"The 3-lane adaptive scheduler (**NAS**) from the Layer-2 board layers on
> later."*

**NAS is this project's own name for the process scheduler**, from the Layer-2
board. The Group B row files it under **storage**. There is no lane concept
anywhere in `kernel/drivers/blk`, `ahci` or `nvme`.

So a session picking this row up would search the block layer and find nothing —
and the row gives no criterion to tell them they are in the wrong subsystem. Worth
separating from the other five: those assert nothing, this one asserts something
**false about which subsystem the work is in**.

---

## §3 — B#15 PMM policy: unfalsifiable, and the obvious reading is already shipped

The row is `B#15 PMM policy | — | smoke-pmmpolicy`. With an empty detail column
there is no way to say whether it is done.

What exists, measured in `kernel/mm/pmm.c`: a **NUMA-affine allocation policy** —
`numa_node_of_cpu(lapic_id())` selects the local node (`:172`), a fallback scans
every node when the local one is empty (`:152`, `:177`), and frees return to the
**owning** node's list (`:233`, `:344`), with the header noting *"a buddy owned by
a different node is simply not on … numa_node_of(addr)'s list"*. That is DDR-882
17b, and it is **gated** by `smoke-numa-alloc` (`gate_shards.txt:134`, **shard 5,
90 s, strict**), which DDR-1073 §1 established asserts `[numa] alloc node1 ->
node1 OK`.

So **if** the row means NUMA allocation policy it is shipped and gated; if it means
something else — watermarks, reclaim, zone policy, per-agent caps — it is unbuilt.
**The row cannot distinguish these, and neither can any measurement.** Recorded as
unfalsifiable rather than guessed either way.

---

## §4 — The three that are simply unbuilt, and one trap

**`mkfs.sfs` >512 slots** — genuinely unbuilt. §INV.22 records `MKFS_MAX_SLOTS =
512` and DDR-773's bulk load (≤14 slots per leaf, multi-leaf beyond). Unlike
§INV.20's B+tree structural delete, **this is not a recorded refusal** — nothing
forbids raising it. §7c doing its job.

**SFS free-space quotas / per-mount limits** — genuinely unbuilt, **and there is a
trap worth naming before someone closes the row by pointing at it**: ADR-032's
token bucket (`FS_WRITE_BURST_MAX` 1 MiB, `FS_WRITE_REFILL_PER_TICK` 256 KiB/tick,
`sched.h:22-23`) is a **per-thread write RATE limit**, charged in `vfs_write`
against `current_thread->fs_write_budget`. A quota is a **per-mount SPACE** bound.
Same word, different quantity, different enforcement point.

**B#6 ext4 write** — genuinely unbuilt, and the source says so outright:
`ext4_ops` (`ext4.c:298`) ends `/* create/write/unlink/txn = NULL: read-only */`.
The **read** path is shipped and gated — `smoke-fs-ext4` (`gate_shards.txt:103`,
**shard 3, 29 s, strict**). Worth stating because the row's bare title reads as
"ext4 is absent", which is wrong. **And DDR-1080 already improved the failure
mode**: `vfs_unlink`/`vfs_rename` return `-ENOSYS` for a NULL backend op rather
than a bare `-1`, so an ext4 write attempt now names its own reason.

---

## §5 — NOT CLAIMED

- **No code change.** `kernel.bin` not rebuilt, so the size/headroom pair and
  `ci-docstate-check` are unaffected. GLOBAL_FORBIDDEN 76; **179 gates unchanged**;
  no new gate. **None of the six named gates should be built**: four name work that
  does not exist (§7c), one names a subsystem the work is not in (§2), and one is
  unfalsifiable (§3).
- **No defect is found in any code and none is alleged.** `pmm.c`, `nvme.c`,
  `ext4.c` and `mkfs.sfs` are correct for what they were built to do; `nvme.c:9`'s
  comment is **accurate about the completion path** and merely reads as a claim
  about the whole driver. What is corrected is the **rows'**.
- **B#1 stays DEFERRED**, on the exception's ground and not the blocker's; **no
  NVMe completion IRQ is built**, and DDR-774a/b/c are not revisited.
- **Nothing is closed.** Five rows are re-labelled *unfalsifiable*, one has its
  subsystem corrected, and none is marked done — including B#15, where the shipped
  NUMA policy is named as **a** reading of the row, not **the** reading.
- **No gate was run for this DDR.** What was measured: the existence check for all
  six names plus the three near-misses; `gate_shards.txt` rows for `smoke-nvme`,
  `smoke-numa-alloc` and `smoke-fs-ext4`; `pmm.c`'s node selection and free paths;
  `nvme.c`'s header and `msix_register` call; `ext4_ops` read in full; `sched.h:5`;
  `sched.h:22-23`; §INV.20/§INV.22; and both deferral entries. Those gates' green
  status comes from CI having run them.
- **No open issue moves** (OPEN-1/2/12/13 untouched). Not an apfreeze, not OPEN-2.

# DDR-1094 — SFS IS ALREADY THE DEFAULT BOOT ROOT ON THE ARTEFACT THAT SHIPS, AND THE ROW'S APPROACH COULD NOT HAVE DELIVERED IT

Status: ASSESSMENT — docs only, no code change, no gate, no defect found
Date: 2026-09-07
Scope: Group B row 1, and the Group F row whose stated blocker it is.

---

## 1. THE ROW, VERBATIM

> **Provisioned SFS as default boot root** | Gate `sfs_format` at `main.c:1128`
> behind `probe_enabled()`. Update the 12 gates asserting on `[sfs]` self-test
> sentinels. | `smoke-sfs-boot-root`

Four claims. **One is correct, one has drifted, one is unbuildable as stated,
and the deliverable is already shipped and CI-gated.** Taken in that order,
because the last is the one that changes what anyone should do next.

---

## 2. THE COUNT IS CORRECT — STATED FIRST, BECAUSE AN AUDIT THAT ONLY REPORTS ERRORS IS NOT AN AUDIT

`grep -c '\[sfs\]' Makefile` returns **14**, and my first reading of that was
the wrong one. Attributing each hit to its enclosing `smoke-*` target and
classifying it: **two are comments** (`smoke-shell`'s note about a measured
splice artefact, `smoke-rqstress-liveness`'s note about what its absence would
mean) and **twelve are `EXTRA_SENTINEL` — required patterns**:

```
smoke-fs, smoke-fs-liveness, smoke-fs-sfs-rw, smoke-sfs-dirs, smoke-sfs-unlink,
smoke-sfs-btree, smoke-sfs-btree-smp4, smoke-sfs-gc, smoke-user,
smoke-blkmq, smoke-blkmq-trace, smoke-msixap
```

**The row's "12 gates" is exactly right.** Recorded because the correction I was
about to make would have been the error.

Note the shape of that list: four of the twelve (`smoke-blkmq`,
`smoke-blkmq-trace`, `smoke-msixap`, `smoke-user`) are **not SFS gates at all** —
they assert an `[sfs]` line incidentally, because the SFS self-tests happen to
run on every boot. That matters to §4.

---

## 3. THE LINE NUMBER HAS DRIFTED, AND INTO MY OWN COMMENT

`main.c:1128` today is inside `smpresched_proof()` — specifically, inside the
truth-table comment **DDR-1092 wrote one commit ago**. It has nothing to do with
`sfs_format`, whose call sites are `main.c:1548`, `:3013` and `:4191`.

The DDR-1073 §5 shape exactly, and its lesson restated because this is the second
occurrence: **a row citing a line number has an expiry date and nothing in the
tree can check one.** The cost here is only a wasted lookup; DDR-1073's instance
was worse (it instructed a future session to instrument two arbitrary places and
read the difference as a diagnosis).

---

## 4. THE STATED APPROACH CANNOT DELIVER WHAT THE ROW ASKS, AND THE REASON IS STRUCTURAL

"Gate `sfs_format` behind `probe_enabled()`" means: stop destroying the SFS
volume during boot, so a process can durably root at it.

**But the SFS self-tests are destructive BY DESIGN, and each one's design is
recorded.** `smoke-sfs-gc` (DDR-762-v2) only proves anything *because* 300
create+write(64 KiB)+unlink cycles put ~4,800 data blocks through a ~4,096-block
volume — exhaustion is the test. `smoke-sfs-btree` (DDR-763) drives 40 cycles on
one path past the first leaf split. `smoke-sfs-unlink`, the journal abort/commit
arm, and the DDR-967 umount race all mutate or tear down that volume.
**You cannot both destroy a volume and durably root at it** — that is not a
difficulty, it is the same disk.

So the consequence of the gating is arithmetic, not speculative: **twelve gates
require those sentinels**, so all twelve would have to pass the probe key, so the
destructive tests would still run on essentially every gate — and the "default
SFS root" would exist only on the gates that opt *out* of testing SFS. That is
not "SFS as default boot root"; it is "an SFS root on the gates that don't touch
SFS." And four of the twelve are gates about block I/O and MSI-X, which would now
carry an SFS probe key for a reason no reader of those recipes could infer.

**The source already states the same conclusion from the other direction.**
`main.c:1418`, on the line that chooses the default mount:

> 5b: this stable FAT32 mount is the process root for the syscall layer
> **(the SFS mount is later reformatted by the destructive self-tests).**

That is a recorded design decision, not an oversight.

---

## 5. THE DELIVERABLE IS SHIPPED, ON THE ARTEFACT THAT ACTUALLY SHIPS, AND GATED AT STRICT TIER

**On the ISO, SFS *is* the default boot root.** Measured end to end:

1. The ISO presents **no virtio disks**, so `blk_count() == 0` (`main.c:4166`).
2. DDR-972 creates three ramdisks mirroring the topology the boot path expects —
   `blk0` blank (boot-disk stand-in), **`blk1` formatted SFS (the root)**, `blk2`
   blank (the scratch volume the destructive block formats itself).
3. `fs_test_thread`'s existing first-mountable loop skips the blank `blk0`, finds
   `blk1`, and calls `vfs_set_default_mnt(mnt)` on it — **so the default process
   root is SFS**, and `elf.c:329` hands it to every ELF loaded thereafter.
4. **No edit was needed and DDR-972 says so in its own comment:** *"the existing
   bring-up runs unchanged — `fs_test_thread`'s `vfs_mount` loop finds it with no
   edit at all."*

And it is **asserted**, not merely present. `smoke-iso-userspace`
(`gate_shards.txt:44`, **shard 0, 300 s, strict**) requires
`[ramdisk] formatted SFS`, requires `[fs] mounted`, and then drives PRISM
through `echo … > /ISOTEST.TXT`, `ls /`, `cat /ISOTEST.TXT`, `rm /ISOTEST.TXT` —
**a write, a directory listing, a read-back and a delete on the SFS root, from
ring 3, on the release ISO, on every CI suite.** DDR-971 is why that gate exists:
it measured an ISO that passed `NEXUS KERNEL OK` and then idled forever with no
root at all.

**This is the DDR-1071 §7b class at its sharpest yet** — five instances in one
table there, but none of them was on the release artefact. Here the row says
"unbuilt", names an approach that cannot work, and points at a gate name that
does not exist, while the thing it asks for is green at strict tier on shard 0.

---

## 6. WHAT IS GENUINELY OPEN IS NARROWER, AND DIFFERENT IN KIND

On the **development / gate configuration** (virtio disks present), the default
root is FAT32, deliberately, for §4's reason. Moving it is **not** a
`probe_enabled()` change and **not** small:

`grep -oE '::/[A-Za-z0-9._]+' Makefile` — the FAT volume carries `/PRISM.ELF`,
`/TERM.ELF`, `/CMUSL.ELF`, `/ARGTEST.ELF`, `/SLOWTEST.ELF`, `/HELLO.TXT`,
`/BIG8K.TXT`, `/BIGPAT.BIN`, `/LongFileName.txt`, `/DOCS/NOTE.TXT`. **Every one
is reached by path from ring 3**, and `smoke-shell` alone drives `/PRISM.ELF`,
`/EXECTEST.ELF`, `/ARGTEST.ELF` and `/SLOWTEST.ELF` by name. So the real
prerequisite is that **the SFS root carry the userspace image** — a build-system
change to how the volume is provisioned (`mkfs.sfs` already provisions files, and
`smoke-mkfs-sfs-multileaf` gates 20 of them), not a kernel flag.

Recorded as the shape of the work if it is ever wanted. **Not built**, and the
reason is DDR-1069's test rather than difficulty: **the configuration that ships
already roots at SFS**, so moving the gate configuration's root buys no
capability — it would only make the gates resemble the ISO, at the cost of
re-pathing every probe in the tree.

---

## 7. THE OBVIOUS GATE ARM IS VACUOUS — MEASURED BEFORE WRITING (twelfth time)

`smoke-sfs-boot-root` asserting *"a process reads a file from an SFS root"*
**passes on today's tree with no change at all**: DDR-760 already loads
`user/sfsroottest.c` with `sp->root_mnt = root_smnt` and prints
`[sfs] persistent root provisioned; SFS-rooted probe spawned`, DDR-764 roots
`bigwritetest` there, and DDR-761/770 root the **AETHER daemon** there
(`[sfs] AETHER daemon rooted at …`, either the kernel-provisioned volume or a
host `mkfs.sfs` image).

What only a *default*-root change can produce is a process that was **never given
an explicit `root_mnt`** reading from SFS — and that arm already exists, on
`smoke-iso-userspace`, where PRISM is exactly such a process. **So there is
nothing left to gate, `smoke-sfs-boot-root` should not be built, and 179 gates
are unchanged.**

---

## 8. AND IT RETIRES A BLOCKER ON A DIFFERENT ROW — THE DDR-1084 §1 PATTERN, THIRD INSTANCE

Group F: *"AETHER audit ring → SFS persistence | `/etc/aether/audit.log` —
**needs SFS boot root first (Group B item 1)**"*.

**That blocker is not true.** The AETHER daemon is rooted at an SFS mount on
every gate boot (`main.c:3063-3069`, printing
`[sfs] AETHER daemon rooted at provisioned mkfs image` or
`… at SFS /etc/aether/config`), and it *already writes* to that root — DDR-761's
`/etc/aether/config` is read from there and DDR-1085's `task` wire is gated
through it. So the daemon has had a writable SFS root for a long time.

What is actually missing is a **flusher**: `kernel/aether/aether_audit.c` is an
append-only **circular in-memory** log, and nothing serialises it to a file. That
is a different, smaller and entirely unblocked piece of work.

DDR-1084 §1 named this pattern from two instances and declined to build a checker
because the signal is semantic; this is the third, and it is the expensive
direction again — **a stale completion marker understates progress, a stale
blocker suppresses work that is already unblocked.** DDR-1084's cheap substitute
applies here: this DDR names the row the blocker was holding.

---

## 9. NOT CLAIMED

* **NO code change, NO gate, NO defect found and none alleged.** DDR-760/770/972
  are all correct and untouched; `kernel.bin` is not rebuilt, so the size/headroom
  pair and `ci-docstate-check` are unaffected, `GLOBAL_FORBIDDEN` stays 76, and
  179 gates are unchanged.
* **NO gate was run for this DDR.** What was measured is target existence, shard
  registration, sentinel classification (the twelve, individually), the recipes
  of `smoke-iso-userspace` and the SFS gates read in full, and the `main.c` blocks
  at `:1406-1420`, `:1483-1560`, `:3005-3070` and `:4160-4196`. The ISO gate's
  green status comes from CI having run it, not from a run here.
* **The Group B row is CORRECTED, not closed** — SFS-as-default-root on the ISO
  is shipped and gated; on the gate configuration it is a deliberate FAT32 choice
  with a recorded reason, and §6 states what moving it would actually cost.
* **NO decision is taken on §6**, and no `mkfs.sfs` provisioning change is made.
* **The Group F audit row is UNBLOCKED, not built** — §8 names what is missing
  and does not design it.
* **OPEN-1/2/12/13 untouched**, no open issue moves, and no release action is
  taken or proposed; `v1.0.0` stays untagged with no promotion in flight.

# DDR-1112 — File-backed `MAP_PRIVATE` mmap: the last unblocked substantial row, and it is ~25 lines

**Status:** DESIGN (§NON-NEGOTIABLE 5 — written and committed BEFORE the code)
**Date:** 2026-09-13
**Row:** Group D, *"`mmap` file-backed mappings — page-fault handler, dirty
tracking, `msync`"*, gate column `smoke-mmap-file` (does not exist, which is
DDR-1063 §7c doing its job and is not a defect).

---

## §1 — Why this row and not another

Every backlog table has now been audited (Group E DDR-1071, F DDR-1072, A/B
DDR-1073, G DDR-1075, H DDR-1081, D DDR-1102, B DDR-1103, C DDR-1104). **What
remains is overwhelmingly refused, blocked, or unneeded**, and each for a
*measured* reason:

| row | why not built |
|---|---|
| `smoke-rqstress` 20× | **costed and refused** — the identical 20× is on record as passing *through a live defect* (DDR-1082) |
| `bg`, B+tree structural delete, `SYS_NET_REVOKE`, the four `CAP_*` rows | **recorded refusals**, not unbuilt work |
| TLS | blocked on a **trust anchor**, an operator decision (DDR-1104 / DDR-1059) |
| IPv6, `ls -R`, `ps` fd column | need an **ABI or multi-backend VFS widening**, one with a measured overflow hazard (DDR-1101) |
| io_uring index discipline | a **ring rewrite plus an ABI contract**, and nothing shipping needs it (DDR-1108) |
| `pthread` / `CLONE_VM` | needs a **cross-CPU TLB shootdown** that does not exist (DDR-1075 §3 / DDR-1077) |
| `smoke-nas`, B#15, the five `--` rows | **unfalsifiable or a category error** (DDR-1103) |

**File-backed mmap is the one row where none of those objections applies**, and
DDR-1102 §3 already removed the blocker it *looked* to have: it is **not** waiting
on the six-argument widening, because that shipped in DDR-877 — `sys_mmap`
already **reads** `fd` and `offset` and returns `-ENOSYS`, which is *the
implementation* saying "not yet", **not the ABI saying "cannot express it."**
So it is blocked **only on itself**, and it is the **nearer** of DDR-1038's two
unblockers for `SYS_FUTEX` (the other, pthreads, is blocked on the shootdown).

---

## §2 — Four measurements that make it small, each read in the tree

**(a) `vfs_read` IS ALREADY `pread`-STYLE, AND THIS IS THE ONE THAT MATTERS.**

```c
int vfs_read(cap_t cap, const struct vfs_file *f, uint64_t off, void *buf, uint32_t len);   /* vfs.h:89 */
```

It takes an **explicit offset**, and `struct vfs_file` (`vfs.h:22`) carries
`size`, `cookie`, `dirent_clus`, `dirent_off`, `mnt` — **and no cursor**. The
seek position lives in `struct fd_entry.off` (`fd.h:35`), one layer up.

**The plausible blocker was that reading at an offset would move the caller's
file position — a side effect POSIX `mmap` must not have. It does not arise.**
The mapping never touches `e->off`.

**(b) The frames are directly kernel-writable, so no user-pointer question
arises at all.** The loop already calls `ptnode_alloc()`, which returns a
kernel-usable pointer into the identity-mapped low 1 GiB (`stage2.asm:13/:520/
:535`, with `PMM_MIN_PHYS` at 16 MiB — established by measurement in DDR-1108
§2, not assumed here). The kernel fills the frame **before** mapping it, so
`copyout`, `vmm_user_range_ok` and SMAP are not involved on this path.

**(c) The capability is already on the fd.** `struct fd_entry` carries `cap`, and
`sys_io.c:324` and `sys_io_uring.c:106` both call `vfs_read(e->cap, e->file,
…)`. Using the same field means **the mapping inherits exactly the read
capability the `open` established** — a caller that cannot read the file cannot
map it — rather than minting anything new.

**(d) NO `struct vm_area` CHANGE IS NEEDED, and that is worth stating because it
is the part a reader expects to be hard.** Under `MAP_PRIVATE` the pages are
`ptnode_alloc`'d exactly as anonymous pages are and are freed identically at
`munmap` and at address-space teardown; there is no write-back, so unmap needs to
know nothing about where the bytes came from. **Nothing about the deferred
machinery — dirty tracking, `msync`, page-fault-driven demand paging — is
required for the private case.** Those are `MAP_SHARED`'s problem, and
`MAP_SHARED` stays refused (§4).

---

## §3 — The design

In `sys_mmap`, replace the blanket `if (a_fd != -1) return -ENOSYS;`:

1. `a_fd == -1` → **the existing anonymous path, verbatim.** (The DDR-1032 shape:
   the old input takes the old code unchanged, so all existing callers and the
   green `smoke-sysmmap` arms are untouched by construction.)
2. `a_fd != -1` **and** `MAP_ANONYMOUS` → `-EINVAL`. An fd *and* "anonymous" is a
   contradiction, and absorbing it silently is the DDR-877 defect.
3. `MAP_SHARED` → `-EINVAL`, unchanged. §4.
4. `PROT_EXEC` → `-EINVAL`, unchanged (W^X; executable code arrives via the ELF
   loader). **Deliberately not relaxed for the file-backed case**, even though
   "map an executable" is the classic use — it needs its own decision and its
   own arm, and the conservative answer is the one already in the file.
5. `fd_get(t, a_fd)` → `-EBADF` if absent; `e->kind != FD_VFS` → **`-ENODEV`**,
   because a pipe or the console has no byte at an offset. Distinct codes, per
   DDR-1080's rule that a return value should discriminate its failure family.
6. `a_off & (PAGE_SIZE - 1)` → `-EINVAL` (POSIX requires a page-aligned offset;
   and after the check the arithmetic below is provably aligned, the DDR-1108
   pattern of making the invariant visible in the code rather than argued in a
   comment).
7. In the existing page loop, after `ptnode_alloc()` (which returns a **zeroed**
   frame), read the file into it:
   `vfs_read(e->cap, e->file, (uint64_t)a_off + i * PAGE_SIZE, frame, PAGE_SIZE)`.

**THE TAIL PAST EOF IS FREE AND IS EXACTLY POSIX.** `ptnode_alloc` zeroes; a short
`vfs_read` at or past EOF leaves the remainder zero. POSIX says the bytes of the
last partial page beyond the file's end read as zero. **So the correct behaviour
falls out of doing nothing**, and a negative `vfs_read` return is the only case
needing a branch (unwind, `-EIO`).

Every other line of the function — arena bounds, overlap, `vma_free_slot`, the
AETHER memory charge, `pflags`, the unwind on failure, `mmap_next` — is unchanged.

---

## §4 — What is deliberately NOT built

- **`MAP_SHARED`** — needs write-back, a shared page cache and cross-process
  coherence. On this kernel it would also need the **cross-CPU TLB shootdown that
  does not exist** (DDR-1075 §3), because two processes sharing a mapped frame is
  the same premise `CLONE_VM` deletes. **Refused, and `ci-cr3-writers-check`
  (DDR-1077) is not weakened by anything here** — no address space is shared.
- **Demand paging / a page-fault handler for file pages.** The row names it; this
  maps **eagerly**. Eager is correct-but-slower and needs no fault path; demand
  paging is a latency optimisation whose cost belongs to whoever needs it.
  `ADR-038`'s stack path is demand-paged and is untouched.
- **`msync`** — has no subject under `MAP_PRIVATE`: there is nothing to sync.
  (The Group G §9.3 shape, named rather than left implicit.)
- **`MAP_FIXED` replace semantics, partial `munmap`, `mremap`** — unchanged.
- **`PROT_EXEC`** — §3.4.

**So the row is CORRECTED AND PARTLY CLOSED, not closed.** Marking it done would
over-claim three named pieces.

---

## §5 — The vacuity check, done BEFORE the arms were written (nineteenth time)

**(a) "mmap a file and assert it succeeds" is WEAK, and weak in the exact way
DDR-877 named.** It does distinguish fixed from unfixed (the unfixed tree returns
`-ENOSYS`), but **a kernel that accepts the fd and hands back anonymous zero
pages would pass it** — which is verbatim the defect DDR-877 called *"worse than
incomplete … 'map this file' succeeded and returned something else entirely."*
Shipping that arm alone would re-create the bug the four-argument form had.

**(b) The arm must assert the FILE'S OWN BYTES**, which is the value a kernel
cannot produce without reading the file.

**THREE ARMS, each catching something the others do not:**

| arm | fixture | asserts | what only a real implementation produces |
|---|---|---|---|
| **A — content + EOF tail** | `/HELLO.TXT`, **25 B**, `PRADYOS filesystem works!` | bytes 0..24 equal the file **and byte 4095 == 0** | zero pages fail the first half; a build that fills the whole page from a repeating read fails the second |
| **B — non-zero offset, multi-page** | `/BIGPAT.BIN`, **65,536 B = 16 pages** | map at **offset 4096**, assert the byte at mapped `+0` equals the pattern at **n = 4096**, not n = 0 | a kernel that ignores `a_off` and reads from 0 fails; **the fixture's non-vacuity is already established** — DDR-973 chose `(7n + 3 + 31*(n>>8)) & 0xFF` precisely because plain `7n+3` has period 256, which made every cluster identical and let a chain-repeat mutant pass |
| **C — the cursor is untouched** | either | `lseek(fd, 0, SEEK_CUR)` **before and after** the mmap returns the same value | this is the §2(a) property. **It is the one that would break silently** if a later change "simplified" the read to use `e->off`, and nothing else in the tree would notice |

**A fourth arm is recorded and NOT built**: *"write to the mapping, then read the
file through a fresh fd and assert the ORIGINAL bytes"* — the property that
distinguishes `MAP_PRIVATE` from `MAP_SHARED`. It is **genuinely discriminating**
and it is **unreachable today**, because `MAP_SHARED` returns `-EINVAL`, so there
is no build in which it could fail. Adding it would be the dead-arm class.
**It becomes owed the moment `MAP_SHARED` is ever built**, and that is why it is
written down here rather than omitted.

**No new gate** — the arms go on **`smoke-sysmmap`** (shard 5, strict), which
already owns this syscall's claims; the DDR-1039/1070 reasoning. **`smoke-mmap-file`
should not be built.** Gate count stays **179**.

---

## §6 — Honest accounting against DDR-1069's test

**DDR-1069's test is "does anything shipping need it?", and the honest answer is
NO.** No ring-3 program in the tree maps a file; `SYS_FUTEX` would need it, and
nothing needs `SYS_FUTEX`. By that test alone this would not be built, and that
is exactly why the test is stated here rather than skipped.

**What makes this one different, and the difference is cost, not appetite:**

- it needs **no new subsystem, no new NSI, no ABI change** (DDR-877 already
  widened the ABI — the row's remaining work is *implementation behind an
  interface that already expresses it*);
- it needs **no new probe ELF**, so `ci-probe-rodata-check`'s count and the
  page-aligned 8,192 B per embedded probe are unchanged;
- **no data structure changes** (§2d);
- the discriminating arm is **cheap and obvious** (the file's own bytes), unlike
  the rows refused for having no non-vacuous arm at any affordable N;
- and the **anonymous path is untouched by construction**, so the blast radius on
  179 green gates is the `a_fd != -1` branch that today returns `-ENOSYS`
  unconditionally — a branch **no gate and no shipping program currently takes.**

That last point is the strongest safety argument available and it is a
measurement, not a hope: **the code being added is only reachable through an
input that is refused today.**

---

## §7 — Proof plan (to be executed, not claimed)

1. `make image` warning-clean at `-Werror`; record the new `kernel.bin` hash and
   size, and **recompute headroom from the size in the same edit**.
2. The three arms on `smoke-sysmmap`, read back **from the capture** rather than
   inferred from `rc=0` (DDR-1041).
3. **M1** — accept the fd but skip the `vfs_read` (i.e. hand back zero pages, the
   DDR-877 defect): must fail **arm A** and **arm B**, and must **pass** every
   pre-existing `smoke-sysmmap` arm, which is the vacuity claim *measured* rather
   than asserted.
4. **M2** — read from offset `0` instead of `a_off`: must fail **arm B alone**,
   with arm A still green. Different arm from M1, so neither carries the other
   (the DDR-1044 check).
5. **M3** — use `e->off` as the read offset instead of an explicit one: must fail
   **arm C**, and is the mutant that justifies arm C existing.
6. Regression: `smoke-sysmmap`, `smoke-fs`, `smoke-shell`, `smoke-mprotect`,
   `smoke-execve-argv`, `smoke-blk-integrity`, plus hygiene **all eight**.
7. Revert must return the current binary **bit-for-bit, verified by rebuild, not
   assumed.**

**NOT to be done in a CI-wait window:** this is a kernel change, and a red on a
tree stacked under a running suite cannot be attributed (the DDR-1107 rule this
design was written under).

---

## §8 — NOT CLAIMED

- **NOTHING IS BUILT YET.** This is a design committed before the code, and the
  proof in §7 is a plan, not a result. No hash, no gate result and no mutant
  outcome is claimed.
- **`MAP_SHARED`, demand paging, `msync`, `MAP_FIXED`, partial `munmap`,
  `mremap` and `PROT_EXEC` are NOT built** and the row is **corrected and partly
  closed, not closed**.
- **`SYS_FUTEX` IS NOT UNBLOCKED BY THIS DOCUMENT** — it is unblocked by the code
  landing, and even then DDR-1038's assessment stands on its own terms; nothing
  here decides to build it.
- **NO defect is found in any code and none is alleged.** `sys_mmap`'s `-ENOSYS`
  is **correct** for an unimplemented feature and is the honest answer DDR-877
  deliberately chose over silently returning zero pages; `vfs_read`, `fd_get` and
  the fd capability model are all correct for what they were built to do.
- **NO new gate** (179 unchanged), **no new probe ELF**, **no new NSI**, **no
  `GLOBAL_FORBIDDEN` change** (77) — the check is deterministic and its own gate
  asserts both directions, so it cannot hide in a green run (the DDR-1065
  reasoning, as against DDR-981/1049's intermittents).
- **No `struct vm_area`, `struct vfs_file` or `struct fd_entry` change.**
- **No open issue moves** (OPEN-1/2/12/13 untouched). Not an apfreeze, not OPEN-2.

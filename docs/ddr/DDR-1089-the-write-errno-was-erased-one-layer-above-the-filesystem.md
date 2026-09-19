# DDR-1089 — The write errno was erased one layer above the filesystem

**Status:** implemented
**Date:** 2026-09-07
**Branch:** `dev/phase1-seyp3n`
**Class:** diagnosability. **No defect in the write path is fixed and none is
alleged** — SFS refuses correctly and loudly; what it cannot do is say *why*.

---

## 1. The standing artefact

`docs/PRE_LAUNCH_CHECKLIST.md` §4.9 has read **"Unexplained, unfixed"** since
DDR-1020, roughly sixty-eight DDRs:

> DDR-1020 M4: a plain rewrite of an existing SFS file returns short for both a
> longer *and* an equal-length payload, while `unlink` + recreate succeeds. The
> ADR-032 write budget is **excluded** as the cause, because the `unlink`+create
> succeeded at the same point.

DDR-1080 split `vfs_unlink`/`vfs_rename`'s fused `-1` for exactly this class of
reason, and its own **RECORDED AND NOT ACTED ON** says the same fusion "runs
through most of the VFS entry layer". This is that residual, taken one layer
further down, in the filesystem the ISO boots from.

---

## 2. The source explains §4.9, and it is a scope limit rather than a defect

`sfs_write` (`kernel/fs/sfs/sfs.c:989`) refuses in one line:

```c
if (off != in->size || in->extent_count >= 4) { pmm_free_page(ip); return -1; }
```

and the function's own header states the first half as deliberate: *"`off` must
equal the current file size (**mid-file overwrite is a later slice**)"*.

A plain rewrite opens an existing file and writes at `off = 0` while `in->size`
is non-zero, so **`off != in->size` fires before any write is attempted**. Both
of §4.9's puzzling observations follow directly:

* **length-independent** — "both a longer *and* an equal-length payload" — because
  the refusal never looks at `len`;
* **`unlink` + recreate succeeds** — because that resets `in->size` to 0, so
  `off == in->size` and the append path runs. The ADR-032 budget was correctly
  excluded, and can now be excluded *positively* rather than by elimination.

**So §4.9 is not a defect.** It is `sfs_write`'s documented append-only scope,
reported through a number that cannot say so.

**What is NOT established, stated rather than smoothed over:** §4.9 says
*"returns short"*, and a first-chunk refusal returns a negative value, not a
short count — so the exact observation depends on the fd offset at that moment,
and **it has not been reproduced here**. §4.9 is therefore **narrowed and made
answerable, not closed**: the mechanism is now readable, and §4 is what lets the
next occurrence state which condition fired instead of being reasoned about.

---

## 3. THE FINDING — the errno is erased one layer above SFS

Splitting `sfs_write`'s `-1` would have been **invisible to every ring-3 caller**,
and that was measured before the arm was written (ninth time this class has been
caught in design text — DDR-1039 §3.1, 1058 §2, 1067 §2, 1070 §4, 1083 §4,
1084 §4, 1085 §2.1, 1088 §4).

`fd_write_user`'s FD_VFS branch (`kernel/syscall/sys_io.c`):

```c
int w = vfs_write(e->cap, e->file, e->off, kbuf, chunk);
if (w < 0) { pmm_free_page(kp); return total > 0 ? total : -EIO; }
```

**Every negative return from `vfs_write` becomes `-EIO`** when nothing has been
written. So all seven `sfs_write` conditions, plus `vfs_write`'s own bare `-1`
for ADR-032 budget exhaustion, arrive in ring 3 as one number — and it is a
number that **actively misleads**: "mid-file overwrite is not implemented"
reported as an *I/O error* sends a future debugger to the block layer.

That is the DDR-1046/1060/1074/1080 shape one layer up: a channel that cannot
carry the distinction the layer below is being asked to make. It is also why
this DDR changes two files rather than one — **splitting SFS alone would have
been unobservable, and a gate arm written against it would have been vacuous.**

---

## 4. The change

**`kernel/fs/sfs/sfs.c` — `sfs_write`'s seven conditions get distinct codes:**

| condition | was | now |
|---|---|---|
| versioned handle (`f->dirent_clus != 0`) | `-1` | `-EPERM` (**= -1**, keeps its value) |
| `pmm_alloc_page()` failed | `-1` | `-ENOMEM` |
| `inode_block_of()` failed | `-1` | `-EIO` |
| `off != in->size` (append-only scope) | `-1` | `-ENOSYS` |
| `extent_count >= 4` (file full) | `-1` | `-EFBIG` |
| `write_extent()` failed | `-1` | `-ENOSPC` |
| `bt_insert()` failed | `-1` | `-EIO` |

`-EPERM` is 1, so **the permission-shaped case keeps `-1` and the other six move
away from it** — DDR-1080's move exactly, and what makes `-1` discriminating from
here on rather than ambiguous. `-ENOSYS` for the append-only scope follows
DDR-956's precedent in `vfs_rename`. `EFBIG` (27, the Linux value) is added to
`errno.h`; it did not exist.

**`kernel/fs/vfs/vfs.c` — the ADR-032 budget stops being a bare `-1`:**
`vfs_write`'s token-bucket refusal (§INV.19) returns **`-EAGAIN`**. It is a rate
limit that refills from elapsed ticks, so "try again" is what it means — and it
is exactly the condition §4.9 had to **exclude by argument** ("the budget is
excluded, because the `unlink`+create succeeded at the same point") because the
number could not say. Included because the artefact names it, not to widen scope.

**`kernel/syscall/sys_io.c` — `fd_write_user` propagates instead of flattening:**
`return total > 0 ? total : w;`. **The partial-write path is untouched** — a
write that made progress still returns its short count, which is the POSIX
contract and what every existing caller reads.

---

## 5. Safe, measured rather than assumed

* **No caller compares a write result against the literal `-1`** — `grep` over
  `kernel/` and `user/` for `== -1` / `!= -1` on `vfs_write`/`SYS_WRITE` returns
  nothing.
* **No gate asserts a specific errno out of a write.**
* The only ring-3 mention of `-EIO` is `user/ftrunctest.c:125`, and it is about
  **`SYS_FTRUNCATE`**, a different syscall, untouched here. That comment is also
  the precedent this DDR's arms follow: *"Asserting only 'returns negative' is not
  enough … Pinning the exact errno is what makes this case verify the guard."*
* The user-visible error changes from `-EIO`, which was wrong for six of the
  seven conditions, to the real one.

---

## 6. The arms, and why the obvious one is vacuous

"Rewrite an existing SFS file and assert it fails" **passes today** — it already
returns `-EIO`. What only the split plus the propagation can produce is **two
conditions that were the same number before and are different numbers now**, both
reachable deterministically from ring 3:

* **Arm A** — reopen `/BIG.TXT` (8192 B, written by this probe's existing body)
  and write at offset 0 → **exactly `-ENOSYS`**, the append-only refusal.
* **Arm B** — fill a fresh file to four extents with four separate writes, then a
  fifth → **exactly `-EFBIG`**, the file-full refusal.

Both are pinned to the exact value, never `< 0`. The arms go on
**`smoke-vfs-bigwrite`** via `user/bigwritetest.c`, which already runs on the
persistent SFS root and whose own header already documents the 5th-extent
rejection — **no new gate and no new probe ELF** (the DDR-1039/1070 reasoning),
so `ci-probe-rodata-check`'s ELF count and the page-aligned 8,192 B per embedded
probe are both unchanged. (An earlier draft of this section named
`smoke-fs-sfs-rw`, which is a different gate; corrected against the Makefile.)

### 6.1 The arms REPORT and the gate JUDGES — and that was a correction, not a plan

Both arms were first written the way every other assertion in this probe is
written: `fail()` on a wrong value. **M1 then died at arm A and arm B was never
reached** — so the mutant that is supposed to demonstrate *both* values collapsing
to `-EIO` could only ever show one of them, and a reader would have had to take
the second on trust.

That is DDR-1020's rule arriving from the other direction: *"a probe should
REPORT and let the gate JUDGE; every `fail()` before the print silently removes
an arm."* It was written there about arms that pass for the wrong reason; here it
is about an arm that **cannot be observed at all** once an earlier one fails.
Restructured: a `dec()` signed-decimal writer, no `fail()` in either new arm, and
**both values on ONE line through one `write(2)`** — DDR-1056's uline rule, so a
concurrent printer cannot splice the sentence the gate greps for:

```
PRADYOS_BIGWRITE_ERRNO rw=-38 ext5=-27
```

The gate pins that **exact pair**. Neither arm can mask the other, and a wrong
value is visible in the capture rather than hidden behind an early exit.

---

## 7. Two mutants, landing on different arms, neither carrying the other

The DDR-1044 M2/M3 check.

* **M1 — the propagation removed** (`fd_write_user` back to `-EIO`), i.e. the
  pre-fix fd layer with the SFS split still present; kernel `a15b93772f50e7f5`.
  Capture: **`PRADYOS_BIGWRITE_ERRNO rw=-5 ext5=-5`** — **both arms fail, both
  reading `-EIO`**. That is §3 reproduced rather than argued: **the split alone is
  invisible from ring 3.** And the same capture carries `PRADYOS_BIGWRITE_OK`, so
  every pre-existing arm of this gate passes on the unfixed tree — §4's vacuity
  claim measured rather than asserted.
* **M2 — the two SFS conditions left fused** (`extent_count >= 4` still returning
  `-ENOSYS`), propagation present; kernel `efe16c1c2420de77`. Capture:
  **`rw=-38 ext5=-38`** — **arm A passes, arm B fails alone.** M2 is the
  load-bearing one for the split: it returns a *plausible* errno for the wrong
  reason, and a one-arm gate would have shipped it.

The two land on different arm sets and **neither carries the other** (M1 fails
both, M2 fails only B), which is the DDR-1044 check. Reverting either returns
`kernel.bin` to **`6db97e890b1bc367`, 1,307,018 B, bit-for-bit** — verified by
rebuild, not assumed. The size is **unchanged**, so §CURRENT BUILD STATE's
size/headroom pair is untouched and `ci-docstate-check` is unaffected.

---

## 8. NOT CLAIMED

* **No defect in the write path is fixed and none is alleged.** SFS refuses
  correctly; mid-file overwrite and the 4-extent ceiling are recorded scope
  limits (`sfs.c:977`, and `bigwritetest.c`'s own header), not bugs. What changes
  is that a refusal now names itself.
* **§4.9 is NARROWED, NOT CLOSED** — §2 states exactly what is and is not
  established, and the *"returns short"* observation is not reproduced here.
* **Mid-file overwrite is NOT implemented** by this change, and the 4-extent
  ceiling is NOT raised. Both remain the later slices their own comments say.
* **The rest of the VFS entry layer is deliberately untouched** — DDR-1080
  measured eighteen `return -1;` sites in `vfs.c` and changed only the two its
  artefact reached; this changes only the two layers §4.9's artefact reached, for
  the same reason. Widening it is an ABI-visible sweep deserving its own decision.
* No open issue moves (OPEN-1/2/12/13 untouched); no new gate; `GLOBAL_FORBIDDEN`
  76 unchanged.

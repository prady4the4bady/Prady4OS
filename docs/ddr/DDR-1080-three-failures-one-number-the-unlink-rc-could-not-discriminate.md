# DDR-1080 — three unrelated failures behind one number: the `rc=` the unlink probe reports could not discriminate

**Status:** ACCEPTED
**Date:** 2026-09-06
**Artefact:** CI 34053412311, shard 4, `smoke-smplock`, tip `5093ca9`.
**This is an INSTRUMENT change. No defect is fixed and no cause is named.**

---

## 1. The artefact

`5093ca9` is DDR-1078 — **no kernel source change** — and the shard's own
post-gate assertion printed `kernel.bin: OK`, so the binary is the same one that
had failed differently one run earlier (DDR-1079). The boot got through SFS
mount, `create/lookup OK`, `64K write/read byte-exact OK`, `hier dirs OK`, then:

```
[sfs] unlink/rmdir detail step=9 rc=18446744073709551615
[sfs] unlink/rmdir FAIL
```

`18446744073709551615` is `0xFFFF…FF` = **-1**. Step 9 is
`vfs_unlink(cap, smnt, "/D/E")` — removing a directory whose only file step 8
had just removed **successfully, with the same `cap` and the same `mnt`**.

**Two different failures on the same gate, on the same binary, one run apart**
(DDR-1079 was the panic-walker #GP). That pattern is worth recording on its own.

---

## 2. The finding: `-1` cannot say which of three things happened

```c
int vfs_unlink(cap_t cap, int mnt, const char *path) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m || !m->fs->unlink || !cap_ok(cap, CAP_FS_WRITE))
        return -1;
```

**Three unrelated defect families collapse into one number:**

| condition | family |
|---|---|
| `!m` — the mount was gone | umount-under-a-live-caller (DDR-967 / FSRM, DDR-954) |
| `!m->fs->unlink` | a filesystem wired without the op |
| `!cap_ok(…)` | the capability race (DDR-964 / OPEN-10, where `rc=-1` **is** `-EPERM`) |

And the field that reports it exists **precisely to say what the failing step
returned**: DDR-984 added `step=` and `rc=` because the probe *"used to collapse
twelve assertions into one `ok` flag … so a failure said only THAT it broke,
never WHICH step or with what rc."* It fixed the `step=` half. **The `rc=` half
was still ambiguous, and this capture is the first time that cost anything.**

The DDR-1046 / DDR-1060 / DDR-1074 shape again: an instrument that cannot
discriminate the case it exists for.

### 2.1 What the artefact does narrow, stated as a narrowing and not a verdict

Step 8 succeeded and step 9 failed with the **same `cap` and the same `mnt`**,
one call apart. Against the three candidates: `!m->fs->unlink` is constant and
would have failed at step 2; the capability worked one line earlier. `!m` — the
mount disappearing between two adjacent calls — is the only one of the three
**consistent with step 8 passing**.

**That is a narrowing, not a mechanism.** §NON-NEGOTIABLE 3 forbids a fix on it,
and none is attempted. It is written down so the next capture is read against it
rather than from scratch.

---

## 3. The change, and why it discriminates

Split the three, following the file's **own** direction — `vfs_create` already
takes distinct precondition codes (`errno.h` is included at `vfs.c:6` with the
comment *"DDR-888: distinct precondition codes from vfs_create"*), and
`vfs_rename` already split `-ENOSYS` out (DDR-956). Only the mount-gone and
capability cases were still fused.

```
!m                 -> -ENODEV   (mount gone before we looked)
!m->fs->unlink     -> -ENOSYS
!cap_ok(…)         -> -EPERM    (== -1, UNCHANGED)
```

**`-EPERM` is `-1`** (`errno.h:9`), so the capability case **keeps its value and
the other two move away from it.** That is what makes `-1` discriminating from
here on rather than ambiguous — a future `rc=-1` now means the capability case
specifically, and `rc=-19` means the mount was gone.

`-ENODEV` and `-EIO` are deliberately different: `-ENODEV` is *gone before we
looked*, `-EIO` is `mnt_lock_live`'s *died while we waited* (DDR-954). Same
condition at two instants, and which one it is matters to exactly the
umount-race family §2.1 narrows to.

`vfs_rename` gets the same split, per its own *"modelled exactly on
vfs_unlink"*.

**Safe, measured not assumed:** every caller of `vfs_unlink`/`vfs_rename` in the
tree tests `== 0` or `!= 0` and **not one compares against the literal `-1`**;
the ring-3 consumers (`fsrmtest.c`, `actiondeltest.c`) test `!= 0` / `>= 0`; and
**no gate asserts a specific errno out of `SYS_UNLINK`**. The user-visible error
for a refused unlink changes from `-1`, which was never a valid errno, to a real
one.

---

## 4. Proof, and its limit stated plainly

`smoke-fs`, `smoke-fs-sfs-rw`, `smoke-smplock`, `smoke-shell` and
`smoke-rename-sfs` all **rc=0**; hygiene **ALL EIGHT**. `kernel.bin`
**1,290,634 B — size unchanged**, so the CLAUDE.md size/headroom pair is
untouched.

**There is no mutant here and that is a deliberate limitation, not an
oversight.** The three arms are *preconditions that a healthy boot never takes* —
forcing one means constructing a kernel with no mount, or no `unlink` op, or a
stripped capability, each of which fails long before step 9 and would prove that
the arm prints a number rather than that the number **discriminates**. What this
change is worth is decided by the **next real occurrence**, and that is the
honest statement of its status.

---

## 5. Recorded and NOT acted on

`grep -n "return -1;" kernel/fs/vfs/vfs.c` returns **18 sites**. The same fusion
runs through most of the VFS entry layer — `vfs_readdir` collapses `!m`,
`!m->fs->readdir` and `!cap_ok(CAP_FS_READ)` into `-1` in one line, as do the
others. **Only the two functions the artefact touched are changed here.**
Widening it is a bigger, ABI-visible sweep that deserves its own decision, and
days from a held release it is not one to take in passing.

---

## 6. Not claimed

* **No defect is fixed and no cause is named.** The SFS failure is **not
  explained**; §2.1 narrows the candidates and says so.
* **This is not attributed to DDR-1078**, whose tip it appeared on: that commit
  changed no kernel source, and its `boot_test.sh` edit lives inside
  `if [ -n "$QEMU_NUMA" ]`, which `smoke-smplock` does not set.
* **Nor is it exonerated.** *"The diff is elsewhere"* is not an argument
  (DDR-1042).
* **One occurrence. No rate**, and no campaign is run to manufacture one.
* **It is not established whether this failure was primary or downstream.**
  That run predates DDR-1079's scan fix, so the capture was scanned only until
  the first matching pattern — if a panic or freeze pattern was also present,
  nothing looked. **A tip at or after `595cd3e` will say.**
* `GLOBAL_FORBIDDEN` **76 unchanged**; 178 gates unchanged; no new gate; no open
  issue moves (OPEN-1/2/12/13 untouched).

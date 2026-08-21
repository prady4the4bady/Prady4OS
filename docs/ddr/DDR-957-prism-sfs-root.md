# DDR-957 — root PRISM at the SFS mount (FEAT-E)

Status: ACCEPTED. Written before the code it governs (R16).

## 1. Problem

`user_boot_from_sfs(cap, smnt, "PRISM.ELF", …)` (`main.c:1934`) loads the PRISM
ELF **from** the SFS volume but leaves the new thread's `root_mnt` at the FAT
default. Every VFS call PRISM subsequently makes therefore resolves against
FAT32 — including `mv`, whose backend `fat32_ops` has no `.rename`
(`fat32.c:635`), so `vfs_rename` returns `-ENOSYS` for every shell-reachable
path (DDR-956 §7).

## 2. Why the obvious fix is wrong

Setting `pr->root_mnt = smnt` **after** the call is a race.
`user_boot_from_sfs` calls `sched_unblock(ut)` internally (`main.c:503`) and then
returns, so the thread is already runnable when the caller receives the pointer —
PRISM may have resolved paths against FAT before the assignment lands.

This is exactly what the tree already documents at `main.c:1831`:

> "Spawned from its embedded bytes (not via SFS — root_mnt is set BEFORE
> unblock, which user_boot_from_sfs doesn't allow)"

which is why the ext4 root-mount probe hand-rolls `elf_load` → set `root_mnt` →
`sched_unblock` instead of using this helper.

## 3. Decision

Give the helper the ability the comment says it lacks, in the same place it
already applies `is_sovereign` ("authority BEFORE the first run",
`main.c:501-502`):

- The existing body becomes `user_boot_from_sfs_rooted(…, int sovereign,
  int root_mnt)`, which applies `root_mnt` when `>= 0` immediately before
  `sched_unblock`.
- `user_boot_from_sfs(…)` becomes a thin wrapper passing `root_mnt = -1`
  ("leave the default").
- Only PRISM's call site uses the `_rooted` form, with `smnt`.

25 existing call sites are untouched, and the compiler enforces the arity — no
site can silently miss the new argument.

## 4. Constraints honoured

- No other thread's `root_mnt` changes; the wrapper's `-1` is a no-op.
- The FAT mount's lifetime is unchanged — it stays mounted and every other
  process keeps resolving against it.
- No new syscall, no capability change, no struct growth (`root_mnt` already
  exists on `struct tcb`).

## 5. Risk — PRISM builtins now read SFS, not FAT

Every path-taking builtin changes which volume it sees: `ls`, `cat`, `touch`,
`rm`, `mv`, `run`, and the redirection targets (`>`, `>>`, `<`, `2>`, `2>>`).

Concretely, `smoke-shell` is the gate most exposed: it writes and reads
`/PRISMNEW.TXT`, `/REDIR.TXT`, `/APP.TXT`, `/IN.TXT`, `/TR.TXT`, `/OUT9k2.TXT`,
`/ERR9k2.TXT`, `/EAP55a.TXT`, `/BOTH66c.TXT` and reads `/BIG8K.TXT`. The first
group is created by the shell itself and will simply be created on SFS instead —
no behavioural difference. **`/BIG8K.TXT` is the exception: it is authored by the
host into the FAT image (`fat-image` target), so it does not exist on SFS.** The
`cat /BIG8K.TXT | cat` step and any assertion on its contents will change
meaning.

That makes `smoke-shell` a required regression check for this change, not an
optional one. If it fails on `/BIG8K.TXT`, the fix is to provision that file onto
the SFS volume too — not to revert the rooting.

## 6. Verification

`make image` clean, `kernel.bin` still ≤ 1,048,576 B, shard-check 145/6/6,
then `smoke-shell` and `smoke-rename` both run and read.

---

## 7. STATUS: implemented, builds clean, but REGRESSES smoke-shell — NOT pushed

`user_boot_from_sfs_rooted` + the PRISM call site are in and build clean
(`626f9652`, then `09aed0a6` with the EXECTEST follow-on; rc=0, 0 warnings,
`kernel.bin` 1,044,862 B, shard-check 145/6/6).

**But `smoke-shell` fails**, exactly in the risk area §5 named — though on a
different file than predicted:

```
[shell] FAIL: background job never reaped (DDR-881)
[1]+ Done(127)   /EXECTEST.ELF
[2]+ Done(127)   /EXECTEST.ELF
```

The gate asserts `Done(0)` (Makefile:1132). Exit 127 means `run /EXECTEST.ELF`
could not launch the image.

### What has been ruled out
`/EXECTEST.ELF` being absent from SFS. It was placed there by reusing the
mount-generic `fat_place_exec_image(cap, smnt)` at the PRISM launch site, and
the serial log now shows **three** `[exec] placed /EXECTEST.ELF` lines. The file
is on the volume and `run` still returns 127, so absence is not the cause.

(Note for whoever continues: the first attempt to check this grepped the make
output rather than `build/shell_serial.log`, which smoke-shell writes serial to,
and returned a misleading zero. Grep the serial log.)

### Still unknown
Why `run` fails with the file present. Next probes, in order:
1. What `do_run_bg` / `do_run` in `user/prism.c` actually call — path resolution
   vs. an execve that may still target the default mount.
2. Whether the SFS copy is written *before* PRISM is unblocked. The placement
   call sits at the PRISM launch site; if PRISM starts first, the file may not
   yet exist when the shell runs. Ordering here is the same class of bug as the
   `root_mnt`-before-unblock issue this DDR exists to fix.
3. Whether exit 127 originates in the loader or in PRISM's own error path.

### Decision
FEAT-E is **not** shipped and **not** pushed. It trades a working `smoke-shell`
for an unproven `mv`, which is a net regression: `smoke-shell` is one of the 145
green gates and job control is a shipped feature. Reverting the rooting is NOT
the answer either — DDR-956 §7 shows `mv` is unusable without it. The correct
close is to finish diagnosing item 2 above.

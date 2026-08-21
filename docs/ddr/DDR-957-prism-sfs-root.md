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

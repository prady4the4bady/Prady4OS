# DDR-1101 — `ls -R` / `ps` full: three asks, three different kinds of thing

**Status:** assessment, docs-only. No code change, no gate, no defect fixed.
**Row:** Group D — *"PRISM `ls -R` / `ps` full — open-fd listing, recursive ls,
signal-mask display"*, gate `smoke-prism-ls`.

---

## §0 — Verdict up front

The row reads as one piece of shell polish. Measured, its three asks are three
different kinds of thing and **none of them is a shell change**:

| ask | status | what it actually needs |
|---|---|---|
| signal-mask display | **NO SUBJECT** | there is no signal mask in this kernel |
| recursive `ls -R` | **not buildable from ring 3** | a type the kernel has and discards at **two** syscall boundaries |
| open-fd listing | buildable, with a **measured overflow hazard** and a safe lever | an ABI widening of `struct procinfo` |

`smoke-prism-ls` does not exist, and **that is DDR-1063 §7c doing its job** — a
planning table naming a gate for work not yet done. It is not counted as a defect.

---

## §1 — "signal-mask display" has no subject

`kernel/proc/signal.h` read in full: **four** signals (`SIGKILL` 9, `SIGUSR1` 10,
`SIGPIPE` 13, `SIGTERM` 15), a per-TCB **pending bitmap** and a handler table.
`grep` for `sigprocmask|sig_mask|sigmask|blocked` over `signal.h` and `signal.c`
returns **nothing**. DDR-1090 reached the same conclusion from the other side
("there is no mask; `sig_active` is a one-bit in-a-handler flag that is a de-facto
ALL-BLOCKED"); this re-measures it rather than inheriting it.

So a `ps` that printed a "signal mask" column would be printing a field the kernel
does not have — the **DDR-1059 shape**, a display reading considerably stronger
than it is. This is the **Group G §9.3 pattern** (a row whose named subject does
not exist) appearing in Group D.

**There is a real adjacent field and it is not the same one:** `sig_pending` is
genuine per-TCB state. A "pending signals" column would be truthful. It is **not**
what the row asks for, and it is **not built** — `struct procinfo` does not carry
it either, so it lands in §3's ABI question.

---

## §2 — `ls -R` is not buildable from ring 3, and the reason is not the recursion

Recursion needs one thing: **telling a directory from a file.** Ring 3 cannot,
by any route.

**`SYS_GETDENTS` gives names only — and discards a value it already has.**
`sys_getdents` (`sys_file.c:141`) calls

```c
vfs_readdir(t->fs_cap, t->root_mnt, path, (int)index, name, &sz);
```

`sz` is filled by the backend and **then never read**; the syscall returns the
**name length** and copies out only the name. `a4` is `(void)`-cast.

**`SYS_FSTAT` cannot answer either — it reports a constant.** `sys_fstat`
(`sys_file.c:87`) sets `st.st_mode = S_IFREG | 0644` on the `FD_VFS` branch **and**
on the else branch. Every fd is a regular file, unconditionally.

**`S_IFDIR` is defined and has zero writers in the syscall layer.**
`kernel/include/stat.h:12` defines it; `grep -rn S_IFDIR kernel/ user/` returns that
definition and **only ext4's own internal constant** — and `ext4.c:147` *computes*
`is_dir` and keeps it inside the driver.

**So the distinction exists in the filesystem layer and is destroyed twice at the
syscall boundary.** PRISM's own `ls` already documents the consequence in its error
string — *"ls: %s: empty or not a directory"* — which is exactly the conflation
that blocks `-R`: probing a child with `getdents` cannot separate an **empty
directory** from a **file**.

**What lifting it costs, measured, and it is not a shell change:** the `readdir`
op's signature is `(ctx, path, index, name, size)`, so a type out-param changes
**every backend's** op; and `struct vfs_file` (`vfs.h:22`) carries `size`, `cookie`,
`dirent_clus`, `dirent_off`, `mnt` — **no type** — so the `fstat` route needs a new
field set by every backend's `open` as well. A multi-backend VFS change, for a
shell flag.

---

## §3 — Open-fd listing: a real hazard, and a safe lever that exists here and not next door

`struct procinfo` (`sched.h:218`) carries `pid`, `ppid`, `state`, `flags`,
`name[16]`, `run_ticks`, `dispatches`. **No fd information and no signal state.**

**The naive widening is the DDR-842 hazard, measured:** `sys_getprocs` does
`copyout(uout, &pi, sizeof pi)` with the **kernel's** `sizeof`, and **three**
userspace files declare their own copy of the layout and read it back across
**four** call sites — `user/prism.c:786`, `user/ckpttest.c:93` and `:122`,
`user/setnametest.c:58`. Widening the struct makes the kernel **write past buffers
sized for the old shape** — an overflow, not a parse error. (`user/compositor.c`
and `user/agentmetricstest.c` carry the same layout for `SYS_AGENT_METRICS`, a
different syscall, so they are additional layout carriers but not in this copyout
path. An earlier draft of this DDR said "five callers"; the measured figure is
three files, four call sites.)

**The safe lever is `a3`, and it exists for `getprocs` precisely because of what
DDR-1098 §2 measured.** All four call sites use the three-argument `nsi` stub and
pass a **literal 0** in `a3` — and `a3` is **RDX**, which a three-argument stub
*does* bind. So `a3 == 0` can keep the original path verbatim while
`a3 == caller's sizeof(struct procinfo)` lets the kernel copy out
`min(kernel_size, caller_size)` and widen safely: the DDR-1032/DDR-1098 shape.

**And the same lever does NOT exist one syscall across, which is the reusable
part.** `sys_getdents`' free argument is `a4`, and `a4` is **R10**, which
three-argument stubs do **not** bind — so a kernel treating it as an out-pointer
would write to whatever the compiler last left in R10. That is DDR-1098 §2's
finding applied a second time, and it is why §2's fix has to go in the return value
or the buffer rather than in a spare argument. **Check which register a "spare"
argument is before designing around it.**

---

## §4 — NOT BUILT, and the obvious gate arm is vacuous

Nothing here is built, on DDR-1069's test: the row is **shell ergonomics**, and the
two buildable thirds cost a multi-backend VFS change (§2) and a syscall ABI
widening with a measured overflow hazard (§3). Neither is proportionate days from a
held release, and neither is needed by anything shipping.

**The obvious arm for the `ps` half is vacuous and is recorded before it is
written (seventeenth time caught in design text):** *"assert `ps` prints an fd
column"* passes on a kernel that prints a **constant** — which is precisely how
`fstat` reports `S_IFREG` today. What only a real implementation can produce is a
count that **changes with the process**: a probe holding a known number of open fds
reporting that number while a second process reports a different one, so neither
value can be a literal.

---

## §5 — NOT CLAIMED

- **No code change.** `kernel.bin` not rebuilt, so the size/headroom pair and
  `ci-docstate-check` are unaffected. GLOBAL_FORBIDDEN 76; **179 gates unchanged**;
  no new gate, and `smoke-prism-ls` **should not be built** for work that does not
  exist yet.
- **No defect is found and none is alleged.** `sys_getdents`, `sys_fstat` and
  `sys_getprocs` are all correct for what they were built to do; `S_IFREG` on every
  fd is accurate today **because nothing can open a directory to contradict it**,
  and that is a scope limit, not a bug. What is corrected is the **row's** account
  of what remains.
- **The row is CORRECTED, not closed.** `ls -R` stays open with a named blocker;
  open-fd listing stays open with a named hazard and a named safe lever;
  signal-mask display is **retired as having no subject**, and `sig_pending` is
  named as the truthful adjacent field rather than silently substituted for it.
- **No `readdir` op signature is changed**, no field is added to `struct vfs_file`
  or `struct procinfo`, and no NSI is reserved.
- **No gate was run for this DDR.** What was measured: `signal.h` read in full;
  `sys_getdents`, `sys_fstat` and `sys_open` read in full; `vfs_readdir`'s signature
  and body; `struct vfs_file` and `struct procinfo`; a tree-wide `grep` for
  `S_IFDIR`; PRISM's `ls` and `ps` builtins; and every `SYS_GETPROCS` call site with
  the stub each uses.
- **No open issue moves** (OPEN-1/2/12/13 untouched). Not an apfreeze, not OPEN-2.

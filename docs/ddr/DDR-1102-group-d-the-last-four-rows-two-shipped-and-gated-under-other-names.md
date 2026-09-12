# DDR-1102 — Group D's last four rows: two shipped and gated under other names

**Status:** assessment, Markdown-only. No code change, no gate, no defect fixed.
**Rows:** Group D — 6-arg `sys_mmap` ABI widening; `mmap` file-backed; dynamic
linking; `io_uring` completions. These were the four never assessed.

---

## §0 — Verdict

| row | named gate | measured |
|---|---|---|
| 6-arg `sys_mmap` ABI widening | `smoke-mmap6` — **does not exist** | **SHIPPED (DDR-877) + GATED, strict, every suite** as **`smoke-sysmmap`**. §7b **+** wrong name. |
| `io_uring` completions | `smoke-iouring` — **does not exist** | **core SHIPPED + GATED, strict, every suite** as **`smoke-sysiouring`**; the four named extensions genuinely unbuilt. `smoke-horizon` shape **+** wrong name. |
| `mmap` file-backed mappings | `smoke-mmap-file` — does not exist | genuinely unbuilt — **§7c doing its job, not a defect** |
| dynamic linking | `smoke-dynlink` — does not exist | genuinely unbuilt — **§7c doing its job, not a defect** |

Two defects, two correct behaviours, and **the mechanical signal is identical in
all four** — "the named gate does not exist". That is DDR-1081 §3's case for
refusing a checker, now with a fourth and fifth instance, and this table is only
separable by reading what the work actually is.

---

## §1 — The 6-arg mmap ABI is shipped, and the row carries no marker

`kernel/syscall/sys_mmap.c`'s own header says it outright:

> "**DDR-877 (item 19): this is now the real POSIX six-argument mmap.** The 4-arg
> form was worse than incomplete — a caller passing fd and offset had them
> silently discarded and got anonymous zero pages back, i.e. *"map this file"*
> succeeded and returned something else entirely. **fd and offset are now read and
> REJECTED** when they ask for something this implementation does not do."

The gate is **`smoke-sysmmap`** (`Makefile:2135`), `gate_shards.txt:140` → **shard
5, 23 s, strict**, and **not** in `shard_check.sh`'s exclude set, so it runs on
**every CI suite**. Its required set is
`SYSMMAP OK` / `SYSMMAP WX REJECTED` / `SYSMMAP FD REJECTED` /
`SYSMMAP OFF REJECTED` / `SYSMUNMAP OK`.

**And it is non-vacuous by construction — DDR-877 anticipated the dead-arm class
in the probe's own comment**, which is worth quoting because it is the discipline
this project spent fifteen later DDRs re-deriving:

> "The accept arm above already passes r8=-1 and r9=0, so a broken marshal turns
> a5 into garbage and mmap fails. **That proves the registers arrive, but not that
> the kernel READS them** — a kernel still discarding fd and offset passes it
> unchanged. These two arms only pass if a5 and a6 are read and acted on, and they
> are distinguishable from each other because the two errors differ (-ENOSYS vs
> -EINVAL): **swapping r8 and r9 in the marshal would fail both**."

**Verified in the probe rather than taken from the comment** (`user/systest.asm`
:276-318): the FD arm sets `r10=0x22, r8=3, r9=0` and requires exactly `-ENOSYS`;
the OFF arm sets `r10=0x22, r8=-1, r9=4096` and requires exactly `-EINVAL`. Exact
values, two of them, differing — the DDR-1044 standard, reached in DDR-877.

So this row is the **DDR-1071 §7b class** (shipped, CI-registered, strict-tier work
with no completion marker) **and** the **DDR-1040 `smoke-wx` shape** (a gate name
that does not resolve) in one row.

---

## §2 — io_uring: the core is shipped and gated; the row's four asks are not

`smoke-sysiouring` (`Makefile:4450`), `gate_shards.txt:139` → **shard 5, 24 s,
strict**, not excluded. It asserts a **batched WRITE then READ on a pipe in one
`io_uring_enter`**, requiring `IO_URING: batch read OK` — i.e. both completions
*and* the data, not merely that the ring mapped.

`kernel/syscall/sys_io_uring.h` states the scope precisely, and it is exactly what
the row asks beyond: **`OP_READ` / `OP_WRITE` only**, on `FD_PIPE` / `FD_VFS` /
console, *"Baseline: **no head/tail wrap**, no kernel-side polling thread."*

So the row's four named asks — `OP_FSYNC`, `OP_OPENAT`, eventfd, SQE chaining —
**are genuinely unbuilt**, and §COMPLETED LAYERS' *"PROC-A..E (pipes/epoll/signals/
io_uring/musl) ✅ COMPLETE"* is about the **baseline**, not about them. This is the
**`smoke-horizon` shape**: corrected, **not closed**. Marking it done would
over-claim four unbuilt opcodes; leaving it unmarked hides a strict-tier gate that
has been running on every suite.

**A fifth gap the row does not name:** `no head/tail wrap` is its own limitation
and is distinct from "SQE chaining". Recorded, not scheduled.

---

## §3 — A dependency correction, and it shortens a chain rather than lengthening one

DDR-1038 blocked `SYS_FUTEX` on *"either file-backed mmap or pthreads"*. DDR-1075
§3.2 / DDR-1077 then established that **pthreads is itself blocked on a cross-CPU
TLB shootdown** that does not exist.

It would be natural to assume file-backed mmap is in turn blocked on the 6-arg ABI
widening — the row sits directly above it and lists the widening as outstanding.
**It is not: the widening is done.** `sys_mmap` already reads `fd` and `offset` and
returns `-ENOSYS` for a file-backed request, which is the *implementation* saying
"not yet", not the *ABI* saying "cannot express it".

So **file-backed mmap is blocked only on itself**, and it is the **nearer** of
DDR-1038's two unblockers for `SYS_FUTEX`. Group D's rows list no dependencies at
all, which is the same defect DDR-1075 §3.2 recorded for pthread/shootdown.

---

## §4 — NOT CLAIMED

- **No code change.** `kernel.bin` not rebuilt, so the size/headroom pair and
  `ci-docstate-check` are unaffected. GLOBAL_FORBIDDEN 76; **179 gates unchanged**;
  no new gate. `smoke-mmap6`, `smoke-iouring`, `smoke-mmap-file` and
  `smoke-dynlink` **should not be built** — two are duplicates of existing
  strict-tier gates under their real names, two name work that does not exist.
- **No defect is found in any code and none is alleged.** DDR-877's mmap, the
  io_uring baseline and both gates are correct; what is corrected is the **rows'**
  account of what remains.
- **The io_uring row is CORRECTED, not closed** (four opcodes plus the ring-wrap
  limitation remain), and the mmap-widening row is the only one of the four that
  closes.
- **file-backed mmap and dynamic linking are NOT unblocked** — §3 removes a
  blocker that was never real, which is not the same as building either.
- **No gate was run for this DDR.** What was measured: `sys_mmap.c`'s header;
  `smoke-sysmmap` and `smoke-sysiouring` recipes read in full; their shard rows and
  absence from the exclude set; `user/systest.asm:276-318` read verbatim to confirm
  the register bindings rather than trust the comment; `sys_io_uring.h`'s scope
  statement; and the existence check for all four named gates. Both gates' green
  status comes from CI having run them, not from a run here.
- **No open issue moves** (OPEN-1/2/12/13 untouched). Not an apfreeze, not OPEN-2.

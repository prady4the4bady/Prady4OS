# DDR-1107 — TWO CHECKLIST ROWS FALSIFIED BY A DDR THAT EDITED THE FILE

**Status:** assessment + correction. **Docs-only: no code change, no gate, no
defect found and none alleged.** `kernel.bin` is not rebuilt, so the size /
headroom pair and `ci-docstate-check` are unaffected. `GLOBAL_FORBIDDEN` 77.
179 gates unchanged. No open issue moves (OPEN-1/2/12/13 untouched).

**Written in a CI-wait window.** DDR-1106's build is deliberately HELD until CI
rules on DDR-1105 (`c204ed0`), and that hold applies to *any* kernel change, not
only to a second hottest-path one: a red on a stacked tree cannot be attributed.
This work touches no build input.

---

## 1. What was swept, and what it found

`docs/PRE_LAUNCH_CHECKLIST.md` §SECTION 4 ("MEASURED, RECORDED, NOT FIXED") is
eighteen rows, and §5.3 ("Unbuilt backlog by group") was swept with it. Each was read against the current tree. **Sixteen are accurate**
— including four that were corrected by the DDR that retired their blocker, in
the same commit as the work:

| row | retired by | corrected |
|---|---|---|
| §4.1 `SYS_IPC_SEND` unwired | DDR-1084 | yes (DDR-1086 §1) |
| §4.9 SFS in-place rewrite unexplained | DDR-1089 | yes, in DDR-1089 |
| §4.10 `resched FAIL` reading | DDR-1092 | yes, in DDR-1092 |
| §4.11 `lock_stat` blind to `mnt_lock` | DDR-1060 | yes, in DDR-1060 |

**Two are false, and both were falsified by ONE DDR: DDR-1098 (`3a1ff98`).**

### 1.1 — §4.17b says the cursor is "Not built". It is built.

The row reads, of the resumable cursor it had just designed:

> A cursor fits in the **ignored `a3`** … **Not built** — an ABI extension plus
> a daemon change plus a gate is its own decision.

Measured in the tree, not inferred from the DDR:

* `struct aether_audit_cursor { uint64_t from, first; }` — `aether.h:292`.
* `aether_audit_read_since(out, max, cur)` — `aether.h:297`,
  `aether_audit.c:172`.
* `static uint64_t g_written;` — `aether_audit.c:65`, commented
  *"total appends ever; **NEVER saturates**"*, which is precisely the monotonic
  counter the row said it *"additionally needs"*, `g_count` being documented as
  saturating.
* `sys_read_audit` reads `a3` as an optional in/out cursor, `copyin`s it,
  `copyout`s it **even when `n == 0`**, and `a3 == 0` still takes the original
  path verbatim — the DDR-1032 shape the row itself proposed.
* A ring-3 consumer exists: PRISM's `audit` builtin (`user/prism.c:841`).

So the row's own remedy shipped, and the row still calls it unbuilt.

### 1.2 — §4.18's "none does" clause is false, and PRISM is the counter-example

The row records the silent 64-clamp and then says:

> The return value *is* the count, so a caller could notice; **none does, and
> none has reason to.**

`user/prism.c`'s drain loop does both halves:

```c
long ask = (want > 64) ? 64 : want;      /* pre-clamped: never asks for more */
long n = nsi(SYS_READ_AUDIT, (long)buf, ask, (long)&cur);
...
total += n;  want -= n;
cur.from = cur.first + (unsigned long long)n;
if (n < ask) break;                      /* caught up with the newest */
```

It uses `n` to advance the cursor **and** to decide it has caught up. It is also
the first caller that cannot be silently narrowed at all, because it never asks
for more than the kernel will give.

**The two probes the row names are unchanged and the row is right about them** —
`egressaudittest.c:81` still asks 128 and `privacynettest.c:240` still asks 256,
neither notices, and both still pass for the reason the row gives. So the row is
**corrected, not closed**: the clamp is still silent, and "no caller notices" is
no longer true.

### 1.3 — §5.3's "Remaining:" list carries the same class, twice

Found while writing §1.2, in the **same file, a different section**. §5.3
("Unbuilt backlog by group") re-measured on 2026-09-05 **by grepping the Makefile
for each named target**, and its Group D block ends with a bare list:

> Remaining: `smoke-readline`, `smoke-futex`, `smoke-pthreads`, `smoke-mmap6`
> (6-arg `mmap` ABI), `smoke-mmap-file`, `smoke-dynlink`, `smoke-iouring`,
> `smoke-sigaction`, `smoke-prism-ls`.

The **grep was right** — `smoke-mmap6` and `smoke-iouring` are absent from the
Makefile — and the **heading is wrong**, because "Remaining" asserts the *work*
remains. Measured here directly rather than inherited from DDR-1102:

| name | Makefile | real target | shard | tier |
|---|---|---|---|---|
| `smoke-mmap6` | **absent** | **`smoke-sysmmap`** | 5 (23 s) | strict |
| `smoke-iouring` | **absent** | **`smoke-sysiouring`** | 5 (24 s) | strict |

Neither real target is in `shard_check.sh`'s `EXCLUDE`
(`smoke-aarch64 smoke-riscv64 smoke-agent-live smoke-selftest smoke-fs-liveness
smoke-fast`), so **both run on every suite at strict tier**.

* **6-arg `mmap` CLOSES.** DDR-877 shipped the real POSIX six-argument form — its
  header calls the 4-arg version *"worse than incomplete"* because `fd` and
  `offset` were *"silently discarded"* — and `smoke-sysmmap` requires
  `FD REJECTED` and `OFF REJECTED` with **exact, differing** errnos, so
  *"swapping `r8` and `r9` in the marshal would fail both"*. Non-vacuous by
  construction, and anticipated in the probe's own comment in 2026.
* **`io_uring` is CORRECTED, NOT CLOSED.** `smoke-sysiouring` asserts a batched
  write-then-read on a pipe in **one** `io_uring_enter` — both completions *and*
  the data. But `sys_io_uring.h` states the scope: `OP_READ`/`OP_WRITE` only,
  *"no head/tail wrap, no kernel-side polling thread"*. The row's four asks
  (`OP_FSYNC`, `OP_OPENAT`, eventfd, SQE chaining) are genuinely unbuilt — **and
  a fifth the row never names is the missing ring wrap.**

**That is the eighth and ninth instance of a class §5.3 itself counts** — it
already labels `smoke-maximize` *"the sixth"* and `smoke-jobctl` *"the seventh"*.
The other seven names in the list stay, three of them with qualifications
recorded elsewhere and not repeated here (`smoke-pthreads` is blocked on a
cross-CPU TLB shootdown that does not exist, DDR-1075 §3.2 / DDR-1077;
`smoke-mmap-file` is blocked **only on itself** now that the widening is done,
DDR-1102 §3; `smoke-sigaction` and `smoke-prism-ls` each name things with **no
subject** in this kernel, DDR-1090 §7 / DDR-1101 §1).

**Why this is recorded under §1 and not given its own DDR number:** it is the
same sweep, in the same file, in the same session, and the measurement it rests
on is DDR-1102's, already on record. What is new is only *that a second section
carried the falsified claim* — which is §2's finding a second time, not a second
finding.

---

## 2. The finding, and it is not the one §4.1 predicted

§4.1's own correction (DDR-1086 §1) sharpened DDR-1084 §1's rule to:

> the rule must read *names the rows in EVERY document that records it* — here
> `CLAUDE.md` **and** this file.

That rule diagnoses the failure as **forgetting the document**. It does not fit
here. `git show 3a1ff98 --stat` lists `docs/PRE_LAUNCH_CHECKLIST.md` at
**1 insertion, 1 deletion** — the file was open in that commit — and the diff is
the **§6 DDR-free-range carrier cell**, `DDR-1098+` → `DDR-1099+`. DDR-1098 also
updated `CLAUDE.md`'s Group F row correctly, saying in as many words
*"THE CURSOR IS BUILT — DDR-1098, and the row is RE-BLOCKED ON THE WRITE PATH,
not closed."*

**So the document was not forgotten and the claim was not misunderstood. The
narrower diagnosis:**

> The carrier that gets bumped on every DDR is **mechanical** and lives in §6.
> The rows a DDR falsifies are **semantic** and live in §4, ~1,400 lines above.
> Nothing connects the two, and a session editing a free-range cell has no
> reason to scroll up.

That is a **third** shape for this family, distinct from the two on record:
DDR-1084 §1 (the unblocking DDR never opens the row's file), DDR-1086 §1 (it
opens one of the files that record the claim and not the others), and this —
**it opens the right file, edits it, and touches only the mechanical cell.**

**Two instances, which is not a rate**, and no rate is claimed.

### 2.1 — No checker is built, and the reason is measured rather than asserted

The mechanical signal available here is *"a DDR's commit touched
`PRE_LAUNCH_CHECKLIST.md` only in §6"*, which is trivially true of most DDRs and
correct for nearly all of them — a check reddening on every routine carrier bump
to catch this one. That is the shape DDR-1086 §4 already costed and refused for
the free-range ranges themselves (**nine of ten** stated `DDR-N+` ranges naming
an occupied number are *correct* historical records), and the shape DDR-1081 §3
sharpened with a case where the identical mechanical signal was a defect on one
row and correct on three others. Nothing in the tree can read "does this row's
claim still hold".

**The cheap substitute, and it is narrower than §4.1's:** a DDR that ships the
remedy a checklist row says is *"not built"* has, by construction, falsified that
row — and it knows which row, because the row is what it was answering. The
obligation attaches to **shipping a named remedy**, not to editing a file.

---

## 3. A coupling recorded, not acted on

PRISM's pre-clamp hand-copies the kernel's `64`. It is correct today and the
duplication is one-directional: a PRISM clamp **≤** the kernel's is always safe.
If the kernel's clamp were ever *lowered*, PRISM would ask for more than it gets
and `if (n < ask) break;` would stop the drain early — an **under-report**, not
an overflow and not a hang. DDR-1095 §5 already recorded why the clamp should not
be changed (returning `-E2BIG` would redden three green gates for no defect), so
nothing is owed here. Worth knowing before anyone touches that constant.

---

## 4. NOT CLAIMED

* **NO code change, NO gate, NO defect found and none alleged.** `sys_read_audit`,
  `aether_audit_read_since`, the 64 clamp and PRISM's builtin are all correct for
  what they were built to do. What is corrected is the **rows**.
* **§4.17b is CORRECTED, NOT CLOSED.** The read-side blocker is retired; the
  flusher is **re-blocked on the write path** (DDR-1098 §4, split three ways by
  DDR-1100 §2, of which only `inline_extents[4]` is the on-disk format). F#76's
  half-claim finding (tamper-**evidence** shipped, **durability** did not) stands
  untouched.
* **§4.18 is CORRECTED, NOT CLOSED.** The clamp is still silent and the two
  probes still ask for more than they receive.
* **DDR-1098 is not criticised.** Its work is real, two-sided on recorded hashes,
  and it updated `CLAUDE.md`'s row correctly. The defect is in the **record**,
  which is the DDR-1084 §1 failure again.
* **NO checker is built** (§2.1), and DDR-1086 §4's refusal stands unchanged.
* **NO gate was run for this DDR.** What was measured: §SECTION 4 read row by
  row; `aether.h:285-300`, `aether_audit.c:57-200` and `sys_read_audit` read in
  full; `user/prism.c`'s drain loop read in full; `git show 3a1ff98 --stat` and
  its checklist diff; the two probe call sites' requested counts.
* **The free-range carriers are advanced to `DDR-1108+` at all four sites**
  (§INV.4, §CURRENT BUILD STATE, §ORIENTATION, and this file's §6) — past this
  DDR, per DDR-1086 §3's caught-before-commit note.

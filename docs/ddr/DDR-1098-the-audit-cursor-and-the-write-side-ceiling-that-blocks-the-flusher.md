# DDR-1098 — THE AUDIT CURSOR, AND THE WRITE-SIDE CEILING THAT BLOCKS THE FLUSHER

Status: IMPLEMENTED (cursor) + MEASURED BLOCKER RECORDED (flusher).
Date: 2026-09-12.
Branch: `dev/phase1-seyp3n`.

## §0 — WHAT THIS IS AND IS NOT

**IS:** a resumable cursor on `SYS_READ_AUDIT` (37), so ring 3 can read an audit
record that is not one of the newest 64; and a PRISM `audit` builtin that uses
it — the operator-facing consumer that makes `SYS_VERIFY_AUDIT`'s answer
actionable.

**IS NOT:** the flusher. **IS NOT** F#76's durable ledger. No file is written.
DDR-1095 §2 named the read API as the blocker on that row; §4 below measures a
**second, larger and independent blocker on the WRITE side**, so the row moves
from "blocked on the read API" to "blocked on the SFS write path", which is a
different and much bigger piece of work. Recording that is half the value here:
without it, the next session builds the cursor's consumer and discovers the
ceiling afterwards.

No defect is fixed and none is alleged. `aether_audit`, the DDR-842 chain,
`aether_audit_verify` and the tamper injector are untouched.

## §1 — THE GAP, AND IT IS DDR-842'S OWN VALUE PROPOSITION

`aether_audit_verify` returns *the index of the first mismatch*, and DDR-842
states in `aether.h` exactly why a boolean would not do:

> A boolean would be useless to an operator: "tampered at entry 1204" locates the
> event being hidden; "tampered" does not.

**Ring 3 cannot read entry 1204.** Measured: `aether_audit_read`
(`aether_audit.c:130`) computes `start = (g_head + LEN - n) % LEN` and returns
**the most recent `n` entries and nothing else**; `sys_read_audit`
(`sys_aether.c:290`) clamps `max` to **64** against `AETHER_AUDIT_LEN = 4096`;
there is no cursor, no start index and no sequence number in the public record.
So the verifier hands the operator an index into a window the operator has no
way to address — 64 of 4096, always the newest, **1.5% of the log**.

That is the DDR-1046 class, in the form where the control works and its *output
cannot be acted on*. It is not a criticism of DDR-842, whose verifier is correct
and gated two-sidedly; the reading half was simply never built.

## §2 — THE DESIGN, AND WHY IT IS NOT DDR-1095 §2.1's

DDR-1095 §2.1 proposed **a cursor in `a3`**, having measured that all four
callers pass `a3` explicitly as `0` and that each freestanding stub binds
`"d"(a3)`, so RDX is written rather than left to chance. That half stands and is
used here.

**Its other half does not survive measurement.** Putting the returned sequence in
`a4` would read a register no existing caller binds: `a4` is **R10**
(`arch/x86_64/syscall_entry.asm:105`, `mov r8, r10 ; C arg5 (a4)`), and the
three shipped probes plus `pradyos.h` all use a **three-argument** stub whose
clobber/input list stops at `"d"(a3)`. R10 therefore holds whatever the compiler
last left in it. A kernel that treated it as a user pointer would `copyout` to a
garbage address on every existing call — `-EFAULT` on three green gates, for a
value they never asked for. The probes that *do* pass four arguments
(`actionexptest.c`, `actionipctest.c`, `agent_base.c`, …) each declare their own
`nsi4` with `register long r10 __asm__("r10")`, which is precisely the evidence
that a three-argument stub does not.

**`a3` therefore carries BOTH directions, as a pointer to two words:**

```c
struct aether_audit_cursor { uint64_t from; uint64_t first; };
```

* `a3 == 0` (NULL) → **verbatim today's behaviour**: the newest `n`, nothing
  written back. This is the DDR-1032 shape — `args == NULL` takes the original
  path unchanged — and it is established by measurement, not hope: every one of
  the four call sites passes a literal `0` into RDX.
* `a3 != 0` → `from` is the sequence to resume at (`0` = the oldest retained),
  and the kernel writes `first` = the sequence of the first entry it returned.

**Why an out-word at all, rather than letting the caller assume it got what it
asked for.** Two reasons, and the second is the load-bearing one:

1. `first - from` **is the wrap loss** — exactly the quantity
   `AETHER_AUDIT_WRAP` announces to serial and which ring 3 has never been able
   to see. A caller that assumed `first == from` would silently under-report a
   log that had wrapped underneath it.
2. **It is the only value in this design that the caller cannot manufacture.** A
   cursor passed in and echoed back proves nothing; a kernel that ignores `a3`
   leaves the caller's own request sitting in its own memory and every check
   against it passes. §5 is built on this.

## §3 — THE SEQUENCE NUMBER COSTS ZERO BYTES OF RING, AND THAT MATTERS HERE

The obvious implementation adds `uint64_t seq` to `struct aether_audit_entry`.
**Do not.** That entry is 64 bytes and the ring is `AETHER_AUDIT_LEN` of them
under a `_Static_assert`ed allocation order; 72 bytes × 4096 = 294,912 B would
push `AUDIT_RING_ORDER` from 6 to 7, i.e. **256 KiB → 512 KiB of PMM**. DDR-842's
own header records what a size mistake on this exact structure cost last time: a
hardcoded `pmm_alloc_pages(5)` left the ring writing **128 KiB past its
allocation**, surfacing as verification reporting tampering at index 1810.

**The sequence is derivable and is derived.** One monotonic `uint64_t g_written`
in BSS, incremented inside the append under the lock that is already held:

* newest retained seq = `g_written`
* oldest retained seq = `g_written - g_count + 1`
* the entry with seq `s` sits at ring index
  `(g_head + LEN - (g_written - s) - 1) % LEN`

Checked at both ends: `s = g_written` gives `g_head - 1` (correct, `g_head` is the
*next* write index), and `s = g_written - g_count + 1` gives
`(g_head + LEN - g_count) % LEN`, which is the existing `start` expression with
`n = g_count`. Nothing on the ring changes; `kernel.bin` grows by no entry bytes.

`g_written` is **not** `g_count`. `g_count`'s own declaration says it *"caps at
AETHER_AUDIT_LEN live"* — it saturates, which is exactly why DDR-1095 found it
unusable as a resume point. `g_written` never saturates: at this kernel's append
rate a `uint64_t` does not wrap in any reachable uptime.

## §4 — THE FLUSHER IS BLOCKED ON THE WRITE PATH, AND THE CEILING IS FOUR

DDR-1094 §8 retired this row's first stated blocker ("needs SFS boot root
first"). DDR-1095 §2 retired the second ("a flusher was never buildable from
ring 3 — there is no cursor"). **Here is the third, measured rather than
designed around:**

* `sfs_write` (`sfs.c:1007`) refuses the fifth extent outright —
  `if (in->extent_count >= 4) return -EFBIG;` — and `struct sfs_inode` carries
  `struct sfs_extent_ref inline_extents[4]`. **Each write call produces exactly
  one extent** (`sfs.h:119`: *"Each write produces one extent"*).
* `fd_write_user`'s `FD_VFS` branch (`sys_io.c:116`) chunks at
  `const uint32_t CHUNK = 4096` and calls `vfs_write` once per chunk.

**Therefore a file created from ring 3 accepts at most four 4096-byte extents —
16,384 bytes, for the life of the file — and there is no fifth append, ever.**
DDR-1089's arm B already pins this from the other direction: four separate
4096-byte writes to `/EXT5.TXT` then a fifth, asserting **exactly `-EFBIG`**.

A flushed audit log is ~60 bytes a record over a 4096-record ring — about
240 KiB, **fifteen times the ceiling**. So a periodic flusher is not "a small
userspace change nobody got round to" for a second, independent reason: it gets
four appends in total.

**And the version that fits is the trap DDR-1095 §2 already named.** A file at
`/etc/aether/audit.log` holding whatever fits in 16 KiB, written once, would read
as a durable audit trail and be a truncated sample — the DDR-1059 shape, a
control that reads considerably stronger than it is. **Not built, deliberately.**

Lifting the ceiling is an **on-disk format change** (`inline_extents[4]` is in
the inode block, and `mkfs.sfs` authors host images that `smoke-sfs-persist`
mounts without reformatting), which is its own decision and not one to take days
from a held release. It is the Group B "SFS extent overflow / large files" row
(`smoke-sfs-largefile`), and this is the first time that row has a number
attached to it: **4 extents, 16,384 bytes, from ring 3.**

## §5 — THE OBVIOUS GATE ARM IS VACUOUS, MEASURED BEFORE IT WAS WRITTEN

Fifteenth time this class has been caught in design text. Three candidate arms
were walked and two were discarded:

**(a) "`audit` prints records."** Passes on a kernel with no cursor at all — it
prints the newest 64. Vacuous.

**(b) "`audit 1 4` reports `first=1`."** *Also vacuous, and this is the subtle
one.* On the unfixed tree `a3` is `(void)`-cast, so nothing is written back and
the caller reads **its own request** out of its own memory — `first == 1` because
PRISM put it there. A cursor echoed back proves nothing. This is why §2's out-word
is a **separate word from the in-word**, and why PRISM **pre-poisons** it: `first`
is set to `0xA0D17C0` before every call, and a kernel that ignores `a3` leaves the
poison in place for the gate to see.

**(c) "two successive drains return different records."** Fails for the opposite
reason: the boot appends audit records continuously, so on the unfixed tree the
"newest 64" window *moves on its own*, and two calls legitimately differ with no
cursor anywhere. Churn defeats it.

**THE SHIPPED ARM** is a drain of more records than one call can return.
`SYS_READ_AUDIT` clamps at 64, so a request for 130 is **three** syscalls, each
resuming where the last ended. On a kernel with a cursor that yields 130 distinct
records with strictly increasing sequences; on a kernel without one, every call
returns the same newest window, so the second window's first record **is
byte-identical to the first window's first record** — and that is what `dup=`
reports. The count alone is not the discriminator (PRISM would count 130 either
way); `dup=0` and the un-poisoned `first=` are.

## §6 — WHAT IS DELIBERATELY NOT CHANGED

* **The 64-entry clamp stays.** DDR-1095 recorded it as silent narrowing and
  recorded why not to change it: `-E2BIG` would redden three green gates for no
  defect, and 64 × 64 B is a correct bound on a kernel stack staging buffer. With
  a cursor the clamp stops being a ceiling and becomes a **page size** — which is
  the point.
* **The record layout is untouched.** DDR-842 refused to widen
  `struct aether_audit_entry_pub` for a measured reason (three probes carry their
  own copy of the layout, so extra bytes would be *"an overflow, not a parse
  error"*). The sequence travels in the cursor word, beside the records, never
  inside them.
* **`SYS_VERIFY_AUDIT` is untouched.** It returns a verdict and an index; this
  makes the index reachable and changes nothing about the verdict.
* **No new NSI.** 37 gains a meaning for an argument it was already given and
  already ignored, in the one way that is provably safe at every existing call
  site.

## §7 — A SIDE EFFECT, STATED RATHER THAN CLAIMED AS A FIX

`aether_audit_read` did **not** take `g_audit_lock`; the new common path does,
because `g_head`, `g_count` and `g_written` must be read as one consistent
triple or the derived index is wrong. The shipped read path therefore gains the
lock. That is strictly more correct — an unlocked reader could previously copy an
entry an appender was mid-way through writing — but **no artefact of that has
ever been captured, no defect is alleged, and it is not claimed as a fix**
(§NON-NEGOTIABLE 3). It is recorded because the change is real and a reader of
the diff will see it. The cost is bounded: a ≤64-entry copy under a lock that
`aether_audit_verify` already holds for a 4096-iteration SHA-256 walk.

## §8 — NOT CLAIMED

* **NO flusher, NO file, NO durable ledger.** F#76's durability half is *not*
  delivered and its row is **re-blocked, not closed** — §4 moves it from the read
  API to the SFS write path, with a number.
* **NO defect fixed and none alleged.** §7's lock is a side effect, not a repair.
* **NO record-layout change**, no new NSI, no change to the 64 clamp, no change
  to `SYS_VERIFY_AUDIT`, no change to `aether_audit`, the DDR-842 chain, the
  verifier or the tamper injector.
* **NO capability change.** `sys_read_audit` had no capability gate before this
  and has none after; whether it should is a DDR-842 S4 policy question for the
  operator, recorded and not taken.
* **NO new gate** — the arms go on `smoke-shell` (the DDR-1039/1070 reasoning),
  so the gate count is unchanged.
* **NO open issue moves.** OPEN-1/2/12/13 untouched; this is not an `apfreeze`.

## §9 — PROOF

**Two-sided, on recorded hashes.**

| build | `kernel.bin` | `audit 1 130` | `audit` |
|---|---|---|---|
| fixed | `8283919d806459eb` | `first=274 n=130 calls=3 dup=0 pois=0 lost=273` | `first=4354 n=16 calls=1 dup=0 pois=0 lost=0` |
| **M1** (`if (0)` on the `a3` branch — the pre-DDR-1098 behaviour verbatim) | `3c2e6d36681d24af` | `first=168630208 n=130 calls=3 dup=1 pois=1 lost=168630207` | `first=168630208 n=16 calls=1 dup=0 pois=1 lost=0` |

`168630208` is `0xA0D17C0`: **the poison survived**, which is what `pois=1` reports.
And **`n=130 calls=3` is byte-identical in both rows** — §5's claim measured
rather than argued. A gate asserting the count would have shipped M1.

Reverting M1 returns `kernel.bin` to `8283919d806459eb` **bit-for-bit**, verified
by rebuild rather than assumed.

**What the numbers say about the log itself, and it is worse than DDR-1095
estimated.** On an ordinary `smoke-shell` boot the drain reports `lost=273` and
the newest window starts at sequence 4354, so `g_written ≈ 4369` and `g_count`
is pinned at `AETHER_AUDIT_LEN`: **the ring has already wrapped and evicted 273
records before the shell is even usable.** The window ring 3 could previously
address was therefore 64 of 4369 written — **1.5%**, and not even the first 273.
DDR-1095 derived that ratio from the constants; this measures it on a real boot.

**Regression, run rather than reasoned about — the four gates that exercise the
`a3 == 0` path and the verifier are all `rc=0`:** `smoke-egress-audit`,
`smoke-privacy-netfilter`, `smoke-auditchain`, `smoke-auditchain-tamper`. That is
the check that matters for §2's claim, because those three probes are the callers
whose unchanged behaviour is asserted "by construction".

`smoke-shell` PASS. Hygiene **ALL EIGHT**. `GLOBAL_FORBIDDEN` **76**, unchanged —
nothing here is a sentinel. **179 gates unchanged.** `kernel.bin` **1,311,114 B —
SIZE UNCHANGED**, the additions fitting inside existing page padding, so the
CLAUDE.md size/headroom pair and `ci-docstate-check` are unaffected.

## §10 — A DEFECT THIS CHANGE CAUSED, FOUND BY RUNNING IT, AND WORTH CARRYING

The first working version put the 64-record staging buffer **on PRISM's stack**,
as every other builtin does (`dmesg` already carries a `char b[4096]` there).
`smoke-shell` then failed at an **unrelated** arm:

```
[shell] FAIL: agent list did not read the roster (DDR-888)
prism> agent list: rc=-14
```

`-14` is `-EFAULT`, on a builtin this change does not touch, for a 16-byte stack
array. **Mechanism:** the 2 KiB local pushed this function's frame past ADR-038's
eagerly-mapped stack window, and `vmm_user_range_ok` validates a syscall pointer
**without faulting the page in** — which is the measured reason
`USER_STACK_EAGER_PAGES` is 8 rather than 1 (ADR-038's own A/B: 30/30 eager vs
**0/30** at one page). So the kernel refused the pointer instead of the CPU
faulting it in, and the symptom landed on whichever builtin next handed the
kernel a deep stack buffer.

Fixed by making the buffer `static`, and **confirmed by experiment, not by
argument**: with that one change `AGENT ROSTER slots=8 active=0` returns and
`smoke-shell` passes. Carry the general form: **a large local in a ring-3
program's hot frame can appear as `-EFAULT` from an unrelated syscall**, because
the failure surfaces at whoever next crosses the eager-stack boundary rather than
at the function that consumed the budget.

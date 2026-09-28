# DDR-1095 — THE AUDIT-PERSISTENCE BLOCKER IS THE READ API, AND F#76 IS MARKED SHIPPED ON WORK THAT DECLINED THE ATTRIBUTION

Status: ASSESSMENT — docs only, no code change, no gate, no defect fixed
Date: 2026-09-07
Scope: Group F "AETHER audit ring → SFS persistence", and the F#76 row it is the
same claim as.

---

## 1. WHY THIS ROW, AND WHAT DDR-1094 LEFT

DDR-1094 §8 retired this row's *stated* blocker — *"needs SFS boot root first"* —
by measuring that the AETHER daemon has been rooted at a writable SFS mount on
every gate boot since DDR-761/770 (`main.c:3063-3069`). It named what was missing
as "a flusher" and deliberately did not design one.

**Designing it found that the flusher was never buildable from ring 3, and the
real blocker is one function's return contract.**

---

## 2. THE BLOCKER, MEASURED

`aether_audit_read` (`kernel/aether/aether_audit.c`) returns **the most recent
`n` entries and nothing else**:

```c
int n = (int)g_count;
if (n > max) n = max;
uint32_t start = (g_head + AETHER_AUDIT_LEN - (uint32_t)n) % AETHER_AUDIT_LEN;
```

and `sys_read_audit` (`sys_aether.c`) clamps `max` to **64**, against a ring of
`AETHER_AUDIT_LEN = 4096`. There is **no cursor, no start index, and no sequence
number** — `g_count` is documented in its own declaration as *"total appended
(caps at `AETHER_AUDIT_LEN` live)"*, i.e. it saturates, so it cannot be resumed
from either. The public record (`struct aether_audit_entry_pub`) carries
`timestamp / agent_pid / action_type / action_id / result` and no log sequence.

**Consequence: ring 3 cannot drain the log.** Two successive calls return
overlapping windows with no way to tell which entries are new, and at best
**64 of 4096** are ever reachable. A daemon-side flusher is not a small
userspace change that nobody got round to — it could not have been written.

**And the naive version would be the DDR-1059 shape.** A file named
`/etc/aether/audit.log` that silently held only the newest 64 events would read
as a durable audit trail and be a 1.5%% sample — a control that reads
considerably stronger than it is. Naming that trap is most of the value here.

### 2.1 THE FIX IS AVAILABLE WITHOUT TOUCHING THE ABI DDR-842 PROTECTED

`sys_read_audit` ignores `a3` and `a4` (`(void)a3; (void)a4;`), so a **cursor can
go in `a3` with no change to the record layout** — which matters, because DDR-842
refused to widen that struct for a stated and measured reason: three ring-3
probes carry their own copy of the layout, and adding 32 bytes per entry would
have made the kernel *"write past buffers those probes sized for the old shape —
an overflow, not a parse error."*

**And `a3 == 0` can keep meaning "newest `n`" verbatim, safely — measured, not
assumed.** All four callers pass `a3` explicitly as `0`:

| caller | form |
|---|---|
| `user/include/pradyos.h:92` | `pradyos_syscall(SYS_READ_AUDIT, (long)buf, max, 0)` |
| `user/egressaudittest.c:81` | `nsi(SYS_READ_AUDIT, (long)buf, 128, 0)` |
| `user/privacynettest.c:240` | `nsi(SYS_READ_AUDIT, (long)buf, 256, 0)` |
| `user/sovegresstest.c:72` | `nsi(SYS_READ_AUDIT, (long)buf, 64, 0)` |

Each freestanding probe's inline stub binds `"d"(a3)`, so RDX is written, not
left to chance. That is the DDR-1032 shape — *"`args == NULL` takes the original
path verbatim"* — established by measurement rather than by hoping.

**What it still needs is a monotonic counter**, because `g_count` saturates: a
`uint64_t g_written` in BSS, incremented on every append, is the value a cursor
resumes from, and the gap between a caller's cursor and the oldest retained entry
is exactly the wrap-loss `AETHER_AUDIT_WRAP` already announces to serial.

**NOT BUILT HERE.** It is a syscall ABI extension plus a daemon change plus a
gate, and this project's own precedent (DDR-1038, DDR-1050, DDR-1069) is to put
the blocker on record first and let the extension be its own decision. §4 is the
non-vacuous arm it would need, recorded so it is not re-derived.

---

## 3. THE FINDING THAT MATTERS MORE: F#76 IS MARKED SHIPPED ON WORK THAT DECLINED THE ATTRIBUTION

`CLAUDE.md:690`:

> | F#76 tamper-evident ledger | **SHIPPED + GATED ×2** — `smoke-auditchain`
> (shard 0) + `smoke-auditchain-tamper` (shard 4), both strict | ✅ |

Those two gates are **DDR-842's**, and DDR-842's own entry in
`docs/AETHER_MASTER_FEATURES.md:120-128` ends:

> **Honest limit:** the log is circular, so verification covers the retained
> window — a wrap is a real gap in the chain of custody, which is why
> `AETHER_AUDIT_WRAP` is emitted. **The durable ledger that survives wrap is
> F#76 and is not claimed here.**

`aether_audit_verify`'s source comment says the same thing in the same words.
**So DDR-842 explicitly disclaimed F#76, and the tracker marked F#76 ✅ by
pointing at DDR-842's gates.**

Measured, so the disagreement is not a reading: `grep -rn 'audit\.log' kernel/
user/ Makefile` returns **zero writers** — nothing anywhere writes a log file.
What shipped is **tamper-EVIDENCE over a circular in-memory window**; what F#76
names is **DURABILITY**. Both are real; they are not the same claim.

**This is the DDR-1071 §7b class in its mirror form, and the mirror is the worse
direction.** There, five rows presented shipped work as remaining — a false
negative, which costs a re-measurement. Here a row presents *half* of a claim as
complete — a false positive, and a false positive is a row that silently stops
being work. It is also the same shape as `smoke-horizon` (bands shipped, mesh
deferred, row corrected not closed), except that this one carries a **✅**.

**DDR-842 is not criticised anywhere in this.** Its work is real, gated
two-sidedly (arm 2 exists precisely so a `verify()` returning 0 unconditionally
cannot pass), and it found a live 128 KiB-for-256 KiB heap overflow on the way.
It also stated its limit accurately in its own record. **The defect is in the
tracker, and it is exactly the failure DDR-1084 §1 named:** the session that did
the work described the residual correctly in prose and did not revisit the row.

**And the two rows are the same item.** Group F's *"AETHER audit ring → SFS
persistence"* and F#76's durability half are one piece of work, listed twice —
once as open with a wrong blocker (DDR-1094 §8), once as ✅ complete.

---

## 4. THE GATE THIS WOULD NEED, RECORDED BECAUSE THE OBVIOUS ONE IS VACUOUS

Thirteenth time this is caught in design text. *"Assert `/etc/aether/audit.log`
exists and is non-empty"* passes on a daemon that writes any bytes at all, and
*"assert it contains a record"* passes on one that formats a record it invented —
the DDR-1066 M2 lesson, where a probe printing a marker it already held passed a
gate that looked end-to-end.

**What the daemon cannot manufacture is a record about a process that is not
it.** The boot fills the ring with kernel-written entries carrying other
processes' `agent_pid` — `AR_CAP_DENIED` from the deny-arm probes,
`AR_NET_CONNECT` from the egress probes, `AR_AUDIT_READ` from
`SYS_VERIFY_AUDIT`. A flushed file containing an entry whose `agent_pid` is
**not** the daemon's own pid is evidence it read the kernel's ring, and no amount
of formatting can fake it.

A second arm follows free from §2.1: with a cursor, **two flushes must not
duplicate** — the second file must begin after the first ended. Without the
cursor that arm cannot be written at all, which is one more way of saying the
blocker is real.

---

## 5. RECORDED AND NOT ACTED ON — A SILENT CLAMP

Per the operator's standing instruction to surface gaps and not act on them:
`sys_read_audit` clamps `max` to 64 **silently**, and two of its three probe
callers ask for more — `egressaudittest.c` for **128**, `privacynettest.c` for
**256**. Both still pass, because each searches the newest window for a specific
record it has just caused, so the truncation never changes their verdict.

But it is the silent-narrowing class this project keeps finding (DDR-1055/1056's
line splice, DDR-1089's errno erasure one layer up): a caller asks for 256, gets
64, and is told nothing. The return value *is* the count, so a caller could
notice — none does, and none has reason to. **Not changed here:** returning
`-E2BIG` would break three green gates for no defect, and the clamp is a correct
bound on a kernel staging buffer. Worth knowing before anyone reads a probe's
`buf[256]` as evidence that 256 entries were examined.

---

## 6. NOT CLAIMED

* **NO code change, NO gate, NO defect fixed.** `kernel.bin` is not rebuilt, so
  the size/headroom pair and `ci-docstate-check` are unaffected;
  `GLOBAL_FORBIDDEN` stays 76 and 179 gates are unchanged.
* **NO cursor is built and no NSI is reserved.** §2.1 establishes that the
  extension is *safe to make*, which is not the same as deciding to make it.
* **The flusher is NOT built and NOT designed** beyond §4's arm.
* **DDR-842's work is not disputed** — the tamper-evidence is real and gated
  two-sidedly. What is corrected is the **record**: F#76's ✅ covers the evidence
  half and not the durability half, which DDR-842's own text says outright.
* **NO gate was run for this DDR.** What was measured is `aether_audit_read`'s
  body, `sys_read_audit`'s clamp and ignored arguments, `AETHER_AUDIT_LEN`,
  `g_count`'s declared saturation, all four `SYS_READ_AUDIT` call sites and their
  inline stubs, the `smoke-auditchain` recipes, and a tree-wide grep for
  `audit.log` writers.
* **OPEN-1/2/12/13 untouched**, no open issue moves, no release action taken or
  proposed.

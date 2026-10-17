# DDR-1018 — Section 3C `ACTION_QUERY_MEMORY`, and a correction to DDR-1017 §1

**Status:** IMPLEMENTED + gated + M1/M2 mutation-checked on distinct kernel
hashes. **Four of eight.** Corrects DDR-1017 §1, which listed this type as
possibly blocked; it is not. Also removes a duplicated `_Static_assert` that
DDR-1016's commit shipped.

---

## 1. CORRECTION — `QUERY_MEMORY` is not blocked

DDR-1017 §1 found `ACTION_SEND_IPC` unbuildable (no ring-3 IPC surface) and
added: *"`QUERY_MEMORY` needs checking the same way before anyone budgets it as
'one more probe'."* That was the right caution and the wrong guess.

**Checked. It has a full ring-3 executor**, and has had one since DDR-836:

```
#define SYS_MEMORY_WRITE   82  /* (key, val, vallen)     -> 0 | -EPERM|-EINVAL|-ENOSPC */
#define SYS_MEMORY_READ    83  /* (key, out, outlen_ptr) -> 0 | -EPERM|-ENOENT|-EFAULT */
```

Both gated on **CAP_MEMORY** (`sys_agentmem.c:20`, `:48`), both already exercised
by `user/agentmemtest.c`. So an approved `QUERY_MEMORY` can be carried out by an
ordinary agent, and this follows **DDR-1015's** auto-approving shape.

**`SEND_IPC` remains blocked**, and that was re-checked properly rather than
carried forward: an exhaustive grep of every `#define SYS_` in `syscall.h` for
`ipc|chan|msg|endpoint|bcast|send|recv|port` returns only `SYS_SOCK_CONNECT`,
`SYS_SURFACE_SENDKEY`, `SYS_SURFACE_SENDEV` and `SYS_NET_ALLOW` — none an
agent-to-agent channel. `ipc_send`/`ipc_recv` stay kernel-only.

So Section 3C is **4 of 8, 1 blocked, 3 to go** — not the "5 remain, 2 blocked"
DDR-1017 §7 predicted.

## 2. The gate asserts the content, and the seed is the control

```
[actionquery] PRADYOS_ACTIONQUERY_OK id=258 st=2 n=24 first=Q
[actionquery] PASS — proposed, approved, then read the fact back
```

`n=24` is exactly `len("Quorum reached at tick 7")`, the value the probe seeded
with its own authority **before** submitting. That seed doubles as the control:
a read returning those bytes proves the store works in this boot, rather than
silently accepting writes and losing them.

`first=Q` is a cheap cross-check that the bytes came from the seeded key.
**It is NOT independently mutation-checked**, and that is recorded rather than
implied: a mutant that returned exactly 24 bytes from a *different* key would
have to be constructed rather than found. Same treatment DDR-998 gave its M3.

## 3. The dead-arm class, a third time — caught before the mutants ran

The first draft called `fail()` on any non-`APPROVED` verdict and then printed
`st`. So the printed `st` could **only ever be 2**, and the gate's
`test "$st" = "2"` could never fire. That is the third instance in three DDRs:

- DDR-1016 §5 — `st` unreachable, because `aether_poll` frees the slot on a
  terminal verdict so the second poll always returned `-ESRCH`.
- DDR-1017 §4 — `ctrl` a literal `1`, because every control mismatch `fail()`d
  before the line printed.
- here — `st` unreachable, because the probe refused to print a bad verdict.

**A field whose only reachable value is the passing one is decoration, not
measurement.** The verdict is now reported, not asserted, and M2 lands on it.

## 4. Why the poll had to be bounded, and the proof that it mattered

The draft also copied DDR-1015's 20000-iteration poll loop. That loop is safe
**only while the action auto-approves and breaks it on iteration 1** — which is
exactly the condition M2 removes. Under M2 the unbounded loop would have spent
20000 syscalls against `AETHER_RATE_MAX = 60` per 100 ticks and the kernel would
have killed the agent before it printed anything (DDR-1016 §4, measured there as
`AGENT_RATE_LIMITED PID=37`).

So the probe uses DDR-1016's two-polls-around-a-ring-3-spin shape even though the
common path needs only one poll. **M2 confirms it: the probe survived a
permanently-PENDING verdict and reported it.** The bound is not defensive
programming, it is what makes the mutant readable.

## 5. Measured

Baseline kernel **`c928493492bba59e`**, `-Werror` clean, **1,126,794 B** against
the 1,572,864 B gate.

| # | mutation | kernel | result | arm |
|---|---|---|---|---|
| M1 | claim success without reading; `first='Q'` kept deliberately | `85d57430833c879d` | `st=2 n=0 first=Q` → FAIL | `n` only |
| M2 | kernel adds `QUERY_MEMORY` to `aether_action_forces_pending()` | `baccd11d421d0c5c` | `st=1 n=0 first=?` → FAIL | `st` (see below) |

M1 isolates `n` exactly as DDR-1015's M1 did — the sentinel and the first-byte
check both pass on a probe that never read.

**M2 moves `st` AND `n` together, and that is correct, not sloppy.** For an
auto-approving type the two are coupled by design: a verdict that never arrives
means the read must not happen. The force-pending gates (DDR-1016/1017) keep
their arms independent because there the effect is expected *not* to occur under
a passing verdict too. Claiming one-arm isolation here would misdescribe the
design.

Gate suite on the baseline, one hash verified before and after each run:
`smoke-actionquery`, `smoke-agentmem`, `smoke-actionspawn`, `smoke-actiondel`,
`smoke-actionread`, `smoke-shell` (73-pattern scan clean), `smoke-blkmq`,
`smoke-rqstress-liveness`, `smoke-blk-integrity` — all PASS.
`hygiene_check.sh` ALL THREE PASSED (162 gates / 10 shards / 7 excluded;
65 probe ELFs; 47 entry points).

## 6. A duplicate `_Static_assert` shipped in `5d2efd5`

`aether.h` carried the DDR-1016 `DELETE_FILE == 6` pin **twice**, with its
comment, from restoring the header after that DDR's M2 mutation and re-applying
the pin on top. Two identical `_Static_assert`s at file scope are legal C11, so
nothing failed and no gate could see it. Removed here. The lesson is narrow but
real: restoring a file from a scratch copy and then re-applying an edit is not
idempotent, and `git diff` was the check that would have caught it.

## 7. Three remain

`PROPOSE_HYPOTHESIS` (DDR-1015 shape — an SFS write), `REWRITE_AGENT_CODE` and
`EVOLVE_GENOME` (force-pending, DDR-1016/1017 shape). `SEND_IPC` stays blocked on
§1 and is not probe work.

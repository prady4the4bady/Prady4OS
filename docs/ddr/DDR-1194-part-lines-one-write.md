# DDR-1194: `smoke-part` prints each `[part]` line in one write

Status: **implemented** in this PR. Approved by the operator as option B on
#81 (comment 6557323941, item 4), after #81 merged.
Label: **mechanism fixed, not proven by a gate.** A cross-CPU race does not
reproduce on demand, so no mutant can be shown red reliably (§4).
Platform: QEMU on GitHub-hosted runners. Nothing here is verified on real hardware.

## 1. The failing artefact

- **Run:** 41299374169, attempt 1, head `5997083` (#81).
- **Job:** `build-and-boot (shard 2)`, gate `[20/21] make smoke-part`.

```
[smoke] FAIL — required pattern '[part] trace plain=DDDLFSF txn=FJLFSF tail=ok setup=ok' not found.
[part] trace plain=DDDLFSF[user] sys_exit(0) pid=21 writes=0 — thread terminating
```

- The `kernel.bin` at `5997083` is byte-identical to the one at `547e1c5`, which passed on run 41272172932.
- Attempt 2 re-ran only shard 2 on the same SHA, and it passed (job 124835189375).
- So the trace content was right (`plain=DDDLFSF`). The line was split, the same
  class of failure as DDR-1055.

## 2. Mechanism

`kernel/main.c` assembled each `[part]` line from 2 to 10 separate
`kputs` / `kputdec` calls.

- Each call takes `g_console_lock` on its own (`console.h` explains this under DDR-963 §5).
- So any other CPU's printer can land *between* two of the calls.
- In the artefact, a thread exiting on another CPU printed its
  `[user] sys_exit(...)` line (`kernel/syscall/syscall.c:233-241`, one `kline_emit`).
  That line landed right after `plain=DDDLFSF`.
- The gate's `grep -F` for the whole required line then found no match.

`kernel/console.h:17` already sets the rule this code broke: use `kline` "for
any line a gate asserts on". All nine `[part]` lines are in `smoke-part`'s
`EXTRA_SENTINEL`, so all nine are in scope, not only the trace line that
happened to split.

## 3. Change

Each of the nine lines is now built in a block-scoped `kline` and emitted with
one `kline_emit`. That is one `kwrite` under `g_console_lock`, so it is atomic
against every printer, including the ring-3 `write(2)` path.

- `kputs(x)` becomes `kline_s(&k, x)`, and `kputdec(v)` becomes `kline_d(&k, v)`.
  Both print unsigned decimal.
- The bytes printed are unchanged. No sentinel, assertion or bound moves.
- **Length bound.** The longest line is the trace line. It is at most
  2 × 95 B (`struct trace_dev.log[96]`) + 46 B of fixed text = 236 B, under
  `KLINE_MAX` (256). If it ever overflowed, it would print `[kline] TRUNC`,
  which is in `GLOBAL_FORBIDDEN` and fails the gate loudly; the line would not
  be silently shortened.
- **Size.** `kernel.bin` is 1,569,162 B before and after (padding). The sha256
  changes from `52b4efbd0ace` to `c578d1c99add`, measured locally on `4909109`
  plus this change.

## 4. Why there is no mutant

The gate's failure needs another CPU to print inside a window a few
microseconds wide. Two runs on byte-identical kernels split one way and not the
other (§1).

- A mutant that puts back the separate `kputs` calls would pass most runs.
- Showing it red would need an injected concurrent printer, which would test the
  injection, not this fix.
- So this DDR claims only that **the mechanism is removed**: no `[part]` line can
  be split any more, because each is one `kwrite`.
- It does **not** claim a gate proves the fix. A green run of this PR shows only
  that the change did not break `smoke-part`.

## 5. Not in scope

Other gate-asserted lines in the tree may still be built from several `kputs`
calls. This DDR does not audit them. A tree-wide audit against the
`console.h:17` rule is a separate item.

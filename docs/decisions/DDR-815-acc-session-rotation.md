# DDR-815 — ACC session rotation: per-agent channels and an epoch

**Status:** accepted
**Date:** 2026-08-05
**Governs:** `kernel/syscall/sys_acc.c`, `SYS_ACC_ROTATE` (NSI 81)
**Builds on:** DDR-813 (ACC envelope)

## The problem

DDR-813 shipped ACC with **one global channel**:

```c
static uint64_t g_acc_last_seq;   /* replay floor  */
static uint64_t g_acc_seq_out;    /* next nonce seq */
```

Two consequences, both of which matter once more than one agent exists:

1. **Agents share a replay window.** Agent A sealing envelopes advances the same
   counter agent B's envelopes are judged against. One agent's traffic can make
   another's envelopes look like replays, and the audit log cannot attribute a
   rejection to the right channel.
2. **Nothing can ever be revoked.** If an agent's signing key is believed
   compromised, there is no way to say "envelopes from before now are no longer
   acceptable". The replay floor only ever moves forward one envelope at a time.

## Decision

**Per-agent channel table + an epoch, rotated by a new owner-only syscall.**

```c
typedef struct {
    uint64_t owner_pid;   /* 0 == free slot           */
    uint64_t last_seq;    /* replay floor, per channel */
    uint64_t seq_out;     /* next outbound nonce seq   */
    uint64_t epoch;       /* bumped by SYS_ACC_ROTATE  */
} acc_chan_t;
```

- Each agent gets its own `last_seq` / `seq_out`, so one agent's traffic cannot
  affect another's replay verdict.
- `SYS_ACC_ROTATE` bumps `epoch` and **raises the replay floor to `UINT64_MAX`**,
  so envelopes minted before the rotation can never verify again. A re-keyed
  agent gets a fresh channel under its new public key.

  **This is revocation, not a reset — and the difference is the whole feature.**
  My first implementation cleared `last_seq` to 0, which would have made every
  pre-rotation envelope acceptable *again*: the exact replay hole this syscall
  exists to close, introduced by the syscall meant to close it. Caught while
  writing the gate's arm 3, before it ran. The slot is also kept as a tombstone
  rather than freed, because freeing it would let the old key allocate a clean
  slot with `last_seq = 0` and replay everything.

## SYS_ACC_ROTATE is CAP_SOVEREIGN. This is the whole security argument.

Rotation **permanently revokes a channel**. Two reasons it cannot be CAP_AGENT:

1. An agent could revoke *another* agent's channel — a denial of service against
   a control the owner depends on.
2. More fundamentally: the reset-to-zero implementation I first wrote would have
   let any caller re-open the replay window. The safe semantics were not obvious,
   and a mechanism whose correctness depends on getting its own reset semantics
   right should not be reachable by the party it constrains.

The capability that protects a mechanism must not be grantable to the party the
mechanism constrains.

So the split across ACC's three calls is:

| call | capability | why |
|---|---|---|
| `SYS_ACC_SEAL` (77) | CAP_AGENT | any agent may report to the owner |
| `SYS_ACC_OPEN` (78) | CAP_SOVEREIGN | reading is the owner's alone |
| `SYS_ACC_ROTATE` (81) | **CAP_SOVEREIGN** | rotation resets replay state |

This matches AGS's reasoning (DDR-814): the privileged direction is whichever one
an attacker would want.

## Audit

Rotation is recorded with its own code, `AR_ACC_ROTATED`, **appended** to
`enum aether_result` per DDR-832 — never inserted, and the existing
`_Static_assert`s stay untouched. "The owner rotated this channel" is a distinct
operator-facing fact from a seal, an open, or a rejection: it is the event that
explains why previously-valid envelopes stopped verifying, and folding it into
`AR_APPROVE` would make that unexplainable.

## Bounds (S2)

`ACC_MAX_CHANNELS` is fixed at 16. A full table returns `-ENOSPC` rather than
evicting a live channel — silently evicting one would reset some other agent's
replay floor, which is precisely the attack this DDR exists to prevent. A
bounded table that rejects is safe; one that recycles is a replay oracle.

## The gate

`smoke-acc-rotate`, four arms:

1. seal → open round-trip on a channel — `ACC_OK`
2. **replay after open** — `ACC_ERR_REPLAY` (the floor works before rotation)
3. **rotate, then present a pre-rotation envelope** — rejected
   (this is the arm the feature exists for; arms 1-2 pass without any rotation
   implemented at all)
4. **rotate from a non-sovereign caller** — `-EPERM` and an audited denial
   (without this, the capability check is untested and the mechanism is a
   replay tool wearing a safety label)

## The rule this earns

**A control that can be reset needs the reset to be more privileged than the
control.** Anti-replay state is only as strong as the weakest caller who can
clear it; shipping the reset at the same capability as the operation it guards
would have left ACC no better off than before DDR-813.

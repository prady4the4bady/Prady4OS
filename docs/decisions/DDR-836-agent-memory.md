# DDR-836 — agent memory: CAP_MEMORY and NSI 82/83

**Status:** accepted
**Date:** 2026-08-05
**Governs:** `kernel/aether/agentmem.{c,h}`, `kernel/syscall/sys_agentmem.c`
**Section:** E (NSI 82/83), 3B (CAP_MEMORY)

## What this is

A bounded key/value store agents use to persist facts across actions:

```
SYS_MEMORY_WRITE  82   (key, val, vallen)      CAP_MEMORY
SYS_MEMORY_READ   83   (key, out, outlen_ptr)  CAP_MEMORY
```

`CAP_MEMORY` is `1u << 18` — the next free bit after `CAP_AGENT` (`1u << 17`).
It follows the `CAP_NET` precedent exactly: a `CAP_` bit in `cap.h` plus a
`uint32_t is_memory` flag on the TCB, checked directly in the syscall.

## This is a shared blackboard, NOT per-agent isolated storage

Stated plainly because the name invites the opposite assumption: **records are
global to all CAP_MEMORY holders.** Any agent with the capability can read and
overwrite any key. The capability is the whole boundary.

That is a deliberate choice for this slice, not an oversight:

- The obvious alternative — key records by owner pid — is **worse than nothing**,
  because a pid is recycled on exit. A new agent inheriting a dead one's pid
  would silently inherit its memories. That is the same reasoning that made
  DDR-815 key ACC channels by verify key rather than pid, and here there is no
  cryptographic identity available to key on.
- Per-agent isolation needs a durable agent identity, which arrives with the
  agent roster (Section G). Building isolation on pids now would have to be
  unbuilt then, and would give a false sense of separation in the meantime.

So: an agent that should not see another agent's facts must not hold
`CAP_MEMORY`. Recorded here so nobody later reads "agent memory" and assumes a
boundary that is not there.

## Bounds (S2)

`MEM_MAX_RECORDS` 64, `MEM_MAX_KEY` 32, `MEM_MAX_VAL` 256. A write to an existing
key **replaces** it. A full table returns `-ENOSPC` and never evicts: silently
dropping a stored fact would surface later as an agent reasoning from a memory
that quietly vanished.

## Audit

`AR_MEM_WRITE` / `AR_MEM_READ`, **appended** to `enum aether_result` per DDR-832,
existing `_Static_assert`s untouched. Reads are audited as well as writes: for a
shared blackboard, "who read this fact" is as much of an operator question as who
wrote it.

## The gate — `smoke-agentmem`, four arms

1. write → read round-trip, byte-for-byte.
2. read an unknown key → `-ENOENT`, not an empty success.
3. overwrite an existing key → the new value, not the old, and not a duplicate
   record (a store that appends instead of replacing makes "what is X?" depend on
   scan order).
4. **a thread WITHOUT `CAP_MEMORY`** → both calls `-EPERM`, audited. One ELF
   spawned twice at different capability, as with DDR-815/834.

## The rule this earns

**A capability whose scope is "everything the capability can reach" must say so
where the reader will look.** The failure mode of an under-documented boundary is
not a crash — it is someone granting the capability to an agent they would not
have granted it to, because the name implied a narrower reach than the code has.

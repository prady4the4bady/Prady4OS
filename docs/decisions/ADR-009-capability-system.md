# ADR-009: Capability system — kernel-table handles, not MAC tokens

- **Status:** Accepted 2026-06-18 (user-approved)
- **Phase:** 2c

## Context

The blueprint specifies a 128-bit capability token `[64-bit resource id][32-bit
perms][16-bit generation][16-bit MAC truncation]` and calls it "cryptographically
unforgeable." A 16-bit truncated MAC gives only ~1/65536 forgery resistance —
that is not unforgeable in any meaningful sense, and a MAC-checked ambient token
is more complex and more fragile than the proven alternative.

## Decision

Use **opaque, table-indexed capability handles** (as in seL4 CNodes and POSIX
file descriptors), confirmed with the user:

- A handle is a 64-bit opaque value `(generation << 32) | slot_index` into a
  **per-process, kernel-private capability table**. Userspace never holds the
  authority bits — it holds an index that is only meaningful inside the kernel.
- Each slot carries a **generation counter**. A handle is valid only if the slot
  is in use AND its generation matches. **Revocation is O(1)**: bump the slot
  generation; every outstanding handle for it instantly fails validation.
- `cap_restrict` and `cap_delegate` can only ever **subset** rights
  (`old & mask`), never amplify — no confused-deputy / privilege escalation.
- Subsystems gate operations with `cap_validate(table, handle, required_rights)`
  before acting (demonstrated by `demo_file_read`; IPC will use this next).
- The blueprint's 128-bit MAC token is retained only as a possible **external
  wire format** (e.g., for cross-machine or persisted caps) layered on top later;
  the internal representation is the table handle.

Rights bitmap: FILE_R/W, NET, PROCESS_SPAWN, KERNEL_QUERY, DISPLAY,
HARDWARE_READ, IPC_SEND/RECV (extensible). Resource types: NONE/FILE/IPC/DEVICE.

## Consequences

- Simple, fast, and secure by construction; no crypto in the hot path.
- 64 slots per table for now (static); a growable table is a later tweak.
- Generation is 32-bit; wraparound after 2^32 reuses of one slot is a
  theoretical-only concern.
- Verified: 11/11 NCS tests pass (validate, rights enforcement, restrict,
  delegate-no-amplify, O(1) revoke, guarded-op). Each thread/process gets its
  own table (`tcb->caps`). IPC (NIA) will be built on top of these caps.

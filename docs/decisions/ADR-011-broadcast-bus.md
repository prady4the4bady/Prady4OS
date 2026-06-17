# ADR-011: Sovereign Broadcast Bus (kernel pub-sub)

- **Status:** Accepted 2026-06-18
- **Phase:** 2c

## Context

The third NIA primitive (blueprint + Layer-2 board): a kernel-resident
publish/subscribe bus carrying system-wide events (AGENT_PROPOSAL,
RESOURCE_ALERT, APPROVAL_REQUEST, MODE_CHANGE). It is the mechanism the
SOVEREIGN/MANUAL approval system uses without polling: the compositor subscribes
to APPROVAL_REQUEST, aetherd publishes proposals.

## Decision

`kernel/ipc/bcast.{c,h}`:

- A subscriber registers an **interest mask** (bitmask of event types) and owns a
  small per-subscriber event ring; it drains via `bcast_wait` (blocking until an
  event arrives, using the scheduler's block/wakeup).
- `bcast_publish` fans out to every subscriber whose mask matches the event type,
  enqueueing the event and waking a blocked subscriber.
- **Capability-gated and resource-bound:** publish requires `CAP_BROADCAST`,
  subscribe requires `CAP_IPC_RECV`, both bound to the bus resource via
  `cap_authorize`. (A new `CAP_BROADCAST` right was added.) This is how
  MODE_CHANGE etc. will be restricted to privileged publishers.
- Single-CPU `cli`/`sti` critical sections guard the subscriber list and queues
  and close the lost-wakeup race, consistent with the sync endpoint (ADR-010).

## Consequences / deferred

- Per-subscriber queue is fixed (32) and drops on overflow (telemetry-style); a
  subscriber that wants every event must keep up.
- Subscribers register before the publisher publishes (no retroactive delivery).
  Real users (compositor/aetherd) subscribe at startup, so this is fine.
- SMP-safe locking replaces `cli`/`sti` when SMP lands.

## Verification

QEMU: two subscribers with different masks receive only their subscribed event
types (sub-alert → ALERT only; sub-approve → APPROVAL+MODE only), publisher
gated by CAP_BROADCAST. smoke PASS; `-Werror` clean. This completes the NIA IPC
fabric (sync endpoint + async SPSC ring + broadcast bus).

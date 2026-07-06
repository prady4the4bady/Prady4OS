# DDR-721 — double-buffered page flip (virtio-gpu)

> DDR before code. Layer-7 polish: the deferred "double-buffer / page-flip"
> item. Today `SYS_FB_FLUSH` TRANSFERs the guest framebuffer into the ONE host
> resource being scanned out — the host can present a half-transferred frame
> (tearing). Fix it host-side, with ZERO client API change.

## Decisions
- **D1 — two host resources, one guest buffer.** The driver creates a second
  2D resource and ATTACH_BACKINGs it to the SAME guest pages (legal: backing
  is just the transfer source; each resource owns its own host store). The
  client keeps its single mapped buffer and its existing MAP/FLUSH calls.
- **D2 — flip on flush.** `SYS_FB_FLUSH` now: TRANSFER into the resource NOT
  currently scanned out → SET_SCANOUT to it → RESOURCE_FLUSH it. The displayed
  resource is always a fully-transferred frame; the transfer target is always
  off-screen. A frame counter alternates the roles.
- **D3 — proof sentinel.** After the first complete alternation (both
  resources have been scanned out) the driver prints
  `[gpu] page-flip OK` once.

## Gate
`smoke-flip`: GPU boot; asserts `[gpu] page-flip OK` alongside the existing
compositor sentinels (the compositor's steady frame loop guarantees ≥2
flushes). 66 gates.

## Non-goals
Triple buffering; vsync pacing; damage-rect flips (full-frame transfers stand);
per-surface GPU resources.

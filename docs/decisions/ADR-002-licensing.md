# ADR-002: Licensing

- **Status:** Accepted
- **Date:** 2026-06-17
- **Phase:** 0

## Context

The blueprint forbids GPL kernel code: "No GPL licensed kernel code. Not a
single line. Write everything original or use MIT/Apache2/BSD-2 licensed
components." We need a license that is permissive, compatible with the Rust
ecosystem, and imposes no copyleft.

## Decision

License PRADYOS under the **MIT License** (`LICENSE`), copyright
"The PRADYOS Authors".

## Alternatives considered

- **Dual MIT OR Apache-2.0** — the Rust-ecosystem norm; Apache-2.0 adds an
  explicit patent grant. Deferred to keep the repo minimal now; can be added
  later if a patent grant becomes desirable (purely additive).
- **BSD-2-Clause** — equivalent permissiveness; MIT chosen for ubiquity.
- **GPL/LGPL** — forbidden by the blueprint.

## Consequences

- Any third-party component we vendor must be MIT/Apache-2.0/BSD-compatible.
  No GPL/LGPL dependencies in kernel or agent runtime.
- If we later add Apache-2.0, update this ADR and add `LICENSE-APACHE`.

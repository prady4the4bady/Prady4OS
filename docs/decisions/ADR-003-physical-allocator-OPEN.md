# ADR-003: Physical memory allocator design

- **Status:** ACCEPTED 2026-06-17 — buddy allocator (user-approved). The
  Blueprint's "Physical Frame Oracle" is deferred as an optional, measurement-
  gated enhancement; its "without fragmentation" claim is not adopted.
- **Date:** 2026-06-17
- **Phase:** 2b

## Context

The two ground-truth documents specify **different** physical allocators:

- **Blueprint §NMA Tier 1** — a custom *Physical Frame Oracle* (PFO): a
  "bitmap + red-black tree hybrid" with "variable-weight allocation" claiming
  to satisfy any size up to 512 contiguous frames "without fragmentation" via
  "predictive coalescing during idle CPU cycles."
- **Instructions §2b** — a plain **buddy allocator (power-of-2 blocks)**.

These are incompatible designs. This must be resolved before Phase 2b.

## Engineering assessment (blunt)

- "Without fragmentation" is **not achievable in general** — any allocator
  serving arbitrary contiguous sizes can fragment. The claim should be softened
  to "low fragmentation under expected workloads," and proven with a benchmark,
  not asserted.
- A red-black tree of free regions is a real and reasonable design (best-fit /
  size-indexed free lists), but it is more complex and slower per-op than a
  buddy allocator, and harder to make correct under SMP.
- A **buddy allocator** is the pragmatic Phase 2b choice: simple, well
  understood, fast O(log n), easy to test. We can layer region-coalescing or a
  size-indexed tree on top later if measurements justify it.

## Recommendation (to be confirmed with the user at Phase 2b)

Start with a **buddy allocator** (Instructions §2b) for a correct, testable
PMM. Treat the PFO's variable-weight/predictive-coalescing ideas as a possible
Phase-2b+ enhancement, gated on actual fragmentation measurements. This is a
deviation from the Blueprint's stated design and therefore requires explicit
user approval before implementation.

## Consequences

Deferred until Phase 2b. No code depends on this yet.

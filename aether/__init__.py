"""AETHER Agent OS layer — host-side Python 3.13 agent stack.

Runs on top of the OS as a service; it is not code the bare-metal target
executes. Invariants for THIS layer are S1-S14 in aether/kernel/invariants;
they are independent of the C kernel's S1-S8 and must not be merged.
"""

from __future__ import annotations

__all__: list[str] = []

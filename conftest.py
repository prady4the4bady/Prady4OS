"""Pytest bootstrap for the AETHER host-side agent layer.

Puts the repository root on ``sys.path`` so the ``kernel.*`` / ``agents.*``
packages import without an install step. This layer runs on the HOST, beside
the bare-metal PRADYOS kernel — it is tooling, not code the target executes.
"""

from __future__ import annotations

import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

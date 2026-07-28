"""Per-test isolation for the AETHER suite (WSL QUIRK-10).

Two module-level singletons default to paths inside the *source tree*:

    aether/kernel/audit/audit_log.py     _DEFAULT_LOG     -> audit_log.jsonl
    aether/kernel/lockbox/cap_sovereign.py _DEFAULT_LOCKBOX -> tokens.jsonl

Anything constructed without an explicit ``audit=``/``lockbox=`` falls back to
them. That has two consequences, and the second is the dangerous one:

1. the suite writes into the repo as a side effect of running;
2. **state leaks between test modules** — a lockbox grant or audit record from
   one module is visible to the next, so a test can pass because an earlier
   module happened to register something. That is the classic "passes in
   isolation, fails in suite" failure, and it also hides the opposite bug: a
   test that *should* fail on a missing capability silently passing.

The autouse fixture below rebinds both singletons to a fresh per-test temporary
directory, so every test starts from empty state and the repo is never touched.
It is autouse deliberately — an opt-in fixture only isolates the tests that
remember to ask, which is precisely the ones that do not have the bug.
"""

from __future__ import annotations

from pathlib import Path
from typing import Iterator

import pytest

from aether.kernel.audit import audit_log as _audit_mod
from aether.kernel.lockbox import cap_sovereign as _lockbox_mod


@pytest.fixture(autouse=True)
def isolate_default_singletons(tmp_path: Path) -> Iterator[None]:
    """Point the process-default audit log and lockbox at a per-test tmp dir."""
    saved_log = _audit_mod._DEFAULT_LOG
    saved_lockbox = _lockbox_mod._DEFAULT_LOCKBOX
    saved_log_path = _audit_mod.DEFAULT_LOG_PATH
    saved_reg_path = _lockbox_mod.DEFAULT_REGISTRY_PATH

    # The path CONSTANTS matter as much as the singletons. Modules written as
    #     self.audit = audit if audit is not None else AuditLog()
    # construct a FRESH log at DEFAULT_LOG_PATH, so rebinding only the singleton
    # still leaves those writing into the source tree. Both are read as module
    # globals at call time, so rebinding the attribute is enough.
    _audit_mod.DEFAULT_LOG_PATH = tmp_path / "_default_audit_log.jsonl"
    _lockbox_mod.DEFAULT_REGISTRY_PATH = tmp_path / "_default_tokens.jsonl"

    fresh_audit = _audit_mod.AuditLog(_audit_mod.DEFAULT_LOG_PATH)
    _audit_mod._DEFAULT_LOG = fresh_audit
    _lockbox_mod._DEFAULT_LOCKBOX = _lockbox_mod.Lockbox(
        registry_path=_lockbox_mod.DEFAULT_REGISTRY_PATH,
        audit=fresh_audit,
    )
    try:
        yield
    finally:
        _audit_mod._DEFAULT_LOG = saved_log
        _lockbox_mod._DEFAULT_LOCKBOX = saved_lockbox
        _audit_mod.DEFAULT_LOG_PATH = saved_log_path
        _lockbox_mod.DEFAULT_REGISTRY_PATH = saved_reg_path

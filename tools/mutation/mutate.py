#!/usr/bin/env python3
"""Fail-closed mutation harness for the Python host layer (DDR-853).

WHY THIS EXISTS. Mutation testing is the only check that tells you a guard is
actually verified: flip the guard off, and if no test dies, the guard is not
being tested. Every DDR in the 847-852 range leans on a mutation table.

WHY IT IS WRITTEN THIS WAY. The ad-hoc shell version used for those slices had
two defects, both of which made it report success it had not earned:

1. **A mutation whose target string was absent was SKIPPED with a warning**,
   and the run still printed kills. DDR-850 reflowed a call onto three lines,
   so one guards-matrix mutation silently stopped applying — and the kill it
   reported came from the previous mutation's stale bytecode. A harness that
   treats "I could not do the thing" as anything other than failure is the
   project's recurring structural defect, in the tool built to detect it.

2. **`__pycache__` on /mnt/c has coarse mtime granularity**, so a
   mutate/restore cycle inside one second could leave Python importing the
   PREVIOUS module. Kills were then attributed to the wrong mutation.

So this harness: aborts if a target is missing or ambiguous, clears bytecode
before every run, imports with `-B`, and **fails if a mutation kills nothing**.
A mutation that kills nothing is the finding, not a footnote.
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


class MutationError(Exception):
    """The harness could not do what it was asked. Never a warning."""


def _clear_bytecode(root: Path) -> None:
    for cache in root.rglob("__pycache__"):
        shutil.rmtree(cache, ignore_errors=True)


def _apply(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count == 0:
        raise MutationError(
            f"{path}: mutation target not found.\n  {old!r}\n"
            "The source has changed since this mutation was written. Refusing "
            "to continue: a skipped mutation still prints kills from whatever "
            "ran before it, which reads as a guard being verified when it is not."
        )
    if count > 1:
        raise MutationError(
            f"{path}: mutation target appears {count} times — ambiguous. "
            "Refusing to guess which one was meant."
        )
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def _run(runner: Path) -> list[str]:
    _clear_bytecode(REPO / "aether")
    proc = subprocess.run(
        [sys.executable, "-B", str(runner)],
        capture_output=True, text=True, cwd=str(REPO),
        env={"PYTHONDONTWRITEBYTECODE": "1", "PATH": ""},
    )
    return [
        line.split("FAIL", 1)[1].strip()
        for line in proc.stdout.splitlines()
        if line.strip().startswith("FAIL")
    ]


def run_matrix(spec_path: Path) -> int:
    """Run a mutation matrix. Returns a process exit code.

    Spec JSON: {"runners": [...], "mutations": [{"name","file","old","new"}]}
    """
    spec = json.loads(spec_path.read_text(encoding="utf-8"))
    runners = [Path(r) for r in spec["runners"]]
    mutations = spec["mutations"]
    if not mutations:
        raise MutationError("empty mutation matrix — nothing was verified")

    survivors: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        for mutation in mutations:
            target = REPO / mutation["file"]
            backup = Path(tmp) / "backup.py"
            shutil.copy2(target, backup)
            try:
                _apply(target, mutation["old"], mutation["new"])
                killed: list[str] = []
                for runner in runners:
                    killed.extend(_run(runner))
            finally:
                shutil.copy2(backup, target)

            if killed:
                print(f"{mutation['name']}: killed by {len(killed)}")
                for k in sorted(set(killed)):
                    print(f"    {k}")
            else:
                print(f"{mutation['name']}: SURVIVED — no test died")
                survivors.append(mutation["name"])

    _clear_bytecode(REPO / "aether")
    if survivors:
        print(f"\n{len(survivors)} mutation(s) SURVIVED: {survivors}")
        print("A surviving mutation means that guard is not verified by any test.")
        return 1
    print(f"\nall {len(mutations)} mutations killed")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("spec", type=Path, help="path to a mutation matrix JSON spec")
    args = ap.parse_args()
    try:
        return run_matrix(args.spec)
    except MutationError as exc:
        print(f"MUTATION HARNESS ABORTED: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

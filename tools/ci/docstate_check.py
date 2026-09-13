#!/usr/bin/env python3
"""docstate_check.py — DDR-1063.

Assert that the LIVE-STATE tables' size/headroom pairings are internally
consistent: stated_size + stated_headroom == the kernel.bin ceiling.

WHAT THIS DOES NOT DO, deliberately (DDR-1063 §5.1): it does NOT compare the
stated size to build/kernel.bin. That check would redden on every commit that
changes the kernel before the docs are updated -- i.e. on correct in-progress
work -- and a check that reddens on correct work gets removed, not obeyed.

It catches exactly the defect that was found: ONE HALF OF A PAIR UPDATED AND THE
OTHER CARRIED FORWARD. CLAUDE.md stated the post-quantum kernel's size beside the
PRE-post-quantum kernel's headroom and overstated the remaining budget by
102,400 B, in the one file sessions are told to trust without re-deriving.

VACUITY (DDR-1063 §5.3): a regex check over prose that matches nothing passes
forever. This one COUNTS its findings and FAILS ON ZERO, and prints every pairing
it checked with file:line so a reader can confirm it looked where they think.

The ceiling is READ FROM THE MAKEFILE, never hardcoded: a literal would keep
passing after a future stage-2 window raise while describing a bound that no
longer exists.

Scope is the two live-state tables only. docs/build_status.md and
docs/BUILD_TRACKER.md are dated append-only logs -- each entry was correct at its
own commit -- so they are deliberately NOT inputs.
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]

# The live-state tables. NOT the append-only history logs -- see module docstring.
INPUTS = [
    "CLAUDE.md",
    "docs/PRE_LAUNCH_CHECKLIST.md",
]

# A number as these documents write it: 1,572,864 or 1572864.
NUM = r"(\d{1,3}(?:,\d{3})+|\d+)"

# A size/headroom pairing on one logical line. The documents phrase it as
#   "<size> B against the <ceiling> B [size] gate -- <headroom> B ... headroom"
# with optional markdown emphasis around any of the three numbers.
PAIR_RE = re.compile(
    NUM + r"\s*B\**\s+against\s+(?:the\s+)?\**" + NUM +
    r"\s*B\**\s+(?:size\s+)?gate\b.{0,80}?\**" + NUM + r"\s*B\**\s*(?:of\s+)?\**headroom",
    re.IGNORECASE | re.DOTALL,
)


def as_int(s):
    return int(s.replace(",", ""))


def ceiling_from_makefile():
    """DDR-1063 5.2: the bound comes from the gate that enforces it."""
    mk = (ROOT / "Makefile").read_text(encoding="utf-8", errors="replace")
    m = re.search(r'wc -c < \$\(KERNEL_BIN\)\)"\s*-le\s*(\d+)', mk)
    if not m:
        print("docstate: FAIL - could not read the kernel.bin ceiling from Makefile",
              file=sys.stderr)
        print("docstate:        expected the size gate's `-le <bytes>` test.", file=sys.stderr)
        sys.exit(2)
    return int(m.group(1))


def main():
    ceiling = ceiling_from_makefile()
    print("docstate: ceiling=%d B (read from Makefile size gate)" % ceiling)

    checked = 0
    bad_sum = 0
    bad_ceiling = 0
    files_read = 0

    for rel in INPUTS:
        p = ROOT / rel
        if not p.exists():
            print("docstate: FAIL - input missing: %s" % rel, file=sys.stderr)
            return 1
        files_read += 1
        text = p.read_text(encoding="utf-8", errors="replace")
        for m in PAIR_RE.finditer(text):
            size, stated_ceiling, headroom = (as_int(g) for g in m.groups())
            line = text.count("\n", 0, m.start()) + 1
            checked += 1

            # The ceiling quoted in prose must be the real one, or the pairing is
            # describing a bound the build does not enforce.
            if stated_ceiling != ceiling:
                print("docstate: %s:%d ceiling=%d != Makefile %d"
                      % (rel, line, stated_ceiling, ceiling), file=sys.stderr)
                bad_ceiling += 1
                continue

            total = size + headroom
            if total != ceiling:
                print("docstate: %s:%d size=%d headroom=%d sum=%d != %d (off by %+d)"
                      % (rel, line, size, headroom, total, ceiling, total - ceiling),
                      file=sys.stderr)
                bad_sum += 1
            else:
                print("docstate: %s:%d size=%d headroom=%d sum=%d OK"
                      % (rel, line, size, headroom, total))

    # DDR-1063 5.3: zero findings is a FAILURE, not a pass. Without this arm,
    # "the check is wired up" and "the check matches nothing" are the same rc.
    if checked == 0:
        print("docstate: FAIL - no size/headroom pairing found (checked %d file(s))"
              % files_read, file=sys.stderr)
        print("docstate:        Either a live-state table lost its kernel.bin row,",
              file=sys.stderr)
        print("docstate:        or its wording drifted past PAIR_RE. Do not delete",
              file=sys.stderr)
        print("docstate:        this check to make that go away - fix whichever it is.",
              file=sys.stderr)
        return 1

    # Distinct remedies: a wrong ceiling in prose and a wrong subtraction are
    # different defects, and printing one remedy for both sends a reader to the
    # wrong line. (Found by M3 -- the first draft printed "recompute headroom"
    # for a ceiling drift, which is not the fix.)
    if bad_ceiling or bad_sum:
        print("docstate: FAIL - %d inconsistent pairing(s) of %d checked"
              % (bad_ceiling + bad_sum, checked), file=sys.stderr)
        if bad_ceiling:
            print("docstate:        %d quote a ceiling the Makefile does not enforce."
                  % bad_ceiling, file=sys.stderr)
            print("docstate:        If the stage-2 window was raised, update the prose",
                  file=sys.stderr)
            print("docstate:        AND recompute every headroom against the new bound.",
                  file=sys.stderr)
        if bad_sum:
            print("docstate:        %d have a headroom that is not ceiling - size."
                  % bad_sum, file=sys.stderr)
            print("docstate:        Recompute headroom from the size in the SAME edit;",
                  file=sys.stderr)
            print("docstate:        never carry a derived quantity forward.", file=sys.stderr)
        return 1

    print("docstate: OK - %d pairing(s) checked, 0 inconsistent" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())

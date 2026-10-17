#!/usr/bin/env python3
"""resize_check.py — assert DDR-997's fixed-edge invariants from a serial log.

Reads PRADYOS_RESIZE_FIX lines, which the compositor emits at every resize
commit carrying BOTH the before and the after geometry:

    PRADYOS_RESIZE_FIX id=1 edge=4 x0=140 y0=140 w0=64 h0=64 x=172 y=140 w=32 h=64

The load-bearing assertion is the fixed-edge equality (DDR-997 §6), not "the
width changed": a width-only check passes on a broken origin, which is exactly
the M1 mutant. Each line is self-consistent — it carries its own x0/w0 — so a
repeated drag is a second independent observation, not a corruption of the
first.

CORRECTED (DDR-1042): that last sentence held only for a repeat of the SAME
arm. A FIX line does not say which arm produced it — the arm is inferred from
the edge bitmask — so a retried arm whose abandoned round grabbed a different
edge injects a foreign record into another arm's evidence. The checks are
therefore split: invariant() must hold for every record, liveness() for at
least one. See the ARM CONTAMINATION note in main().

usage: resize_check.py <logfile> <surface-id> <arm> [<arm> ...]
"""
import sys

RZ_N, RZ_S, RZ_W, RZ_E = 1, 2, 4, 8
KEYS = ("edge", "x0", "y0", "w0", "h0", "x", "y", "w", "h")
CLAMP_MIN = 32                      # DDR-718's floor, imposed not negotiated


def parse(path, sid):
    tag = "PRADYOS_RESIZE_FIX id=%s " % sid
    out = []
    with open(path, "r", errors="replace") as fh:
        for ln in fh:
            if tag not in ln:
                continue
            d = {}
            for tok in ln.split():
                k, _, v = tok.partition("=")
                if k in KEYS:
                    try:
                        d[k] = int(v)
                    except ValueError:
                        pass
            # Serial output interleaves mid-line in this tree (§INV.23), so a
            # partial line is a real possibility. Drop it rather than reading a
            # missing field as zero — a zero x0 would make the invariant pass by
            # accident.
            if all(k in d for k in KEYS):
                out.append(d)
    return out


def invariant(arm, r):
    """DDR-997's actual invariant: the FIXED EDGE HELD. Must hold for EVERY
    observation carrying this arm's edge bit, whichever drag produced it —
    a resize that moves the edge it is supposed to pin is wrong no matter who
    asked for it, and this is the half the M1/M2 mutants break."""
    bad = []
    if arm in ("e", "s"):
        if r["x"] != r["x0"] or r["y"] != r["y0"]:
            bad.append("origin moved on an %s drag: (%d,%d) -> (%d,%d)"
                       % (arm.upper(), r["x0"], r["y0"], r["x"], r["y"]))
    elif arm == "w":
        if r["x"] + r["w"] != r["x0"] + r["w0"]:
            bad.append("RIGHT EDGE MOVED on a W drag: x+w=%d, was x0+w0=%d "
                       "(M1: resize without the move, or M2: clamp after origin)"
                       % (r["x"] + r["w"], r["x0"] + r["w0"]))
    elif arm == "n":
        if r["y"] + r["h"] != r["y0"] + r["h0"]:
            bad.append("BOTTOM EDGE MOVED on an N drag: y+h=%d, was y0+h0=%d "
                       "(M1: resize without the move, or M2: clamp after origin)"
                       % (r["y"] + r["h"], r["y0"] + r["h0"]))
    else:
        bad.append("unknown arm %r" % arm)
    return bad


def liveness(arm, r):
    """Did THIS observation show the arm doing what it was asked to do?

    Separate from invariant() deliberately — see main()'s ARM CONTAMINATION
    note. These are statements about the INJECTOR's intent, not about the
    compositor's correctness, so they need only be true of the arm's OWN drag,
    and requiring them of every same-edge record is what made a retried arm
    fail a different arm.
    """
    bad = []
    if arm == "e":
        if r["w"] <= r["w0"]:
            bad.append("E drag did not widen: w0=%d w=%d" % (r["w0"], r["w"]))
    elif arm == "s":
        if r["h"] <= r["h0"]:
            bad.append("S drag did not heighten: h0=%d h=%d" % (r["h0"], r["h"]))
    elif arm == "w":
        if r["w"] != CLAMP_MIN:
            bad.append("W arm did not reach the %d px floor (w=%d) — the arm is "
                       "not exercising the clamp, so M2 would not be caught"
                       % (CLAMP_MIN, r["w"]))
        if r["x"] == r["x0"]:
            bad.append("origin did NOT move on a W drag (x=%d) — M1" % r["x"])
    elif arm == "n":
        if r["h"] != CLAMP_MIN:
            bad.append("N arm did not reach the %d px floor (h=%d)"
                       % (CLAMP_MIN, r["h"]))
        if r["y"] == r["y0"]:
            bad.append("origin did NOT move on an N drag (y=%d) — M1" % r["y"])
    else:
        bad.append("unknown arm %r" % arm)
    return bad


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    path, sid, arms = sys.argv[1], sys.argv[2], sys.argv[3:]
    recs = parse(path, sid)
    # DDR-997 §13.4 — an arm that never ran is not an arm that failed.
    #
    # The injector abandons arms once its total budget is spent and names them
    # in a sidecar beside the log. Without this the gate printed "FAIL — arm n:
    # no PRADYOS_RESIZE_FIX line" for an arm the harness never injected, in the
    # same words a genuinely broken arm produces (CI run 33247210328). Those are
    # different facts and a reader cannot separate them from the old message.
    #
    # It still exits non-zero: a run that could not test every arm has not
    # passed. What changes is that it says WHY, so nobody root-causes the
    # compositor for a budget overrun.
    skipped = set()
    try:
        with open(path + ".skipped", "r") as fh:
            skipped = {a.strip() for a in fh.read().split() if a.strip()}
    except OSError:
        pass
    want = {"e": RZ_E, "s": RZ_S, "w": RZ_W, "n": RZ_N}
    # An E drag must not also carry W, etc. — otherwise a corner grab would be
    # scored as an edge drag and the origin assertions would be wrong.
    excl = {"e": RZ_W, "s": RZ_N, "w": RZ_E, "n": RZ_S}
    rc = 0
    for arm in arms:
        if arm in skipped:
            print("[resizeall] INCOMPLETE — arm %s NEVER RAN: the injector's "
                  "budget was exhausted before it started. This is not a "
                  "failure of the arm and says nothing about the compositor; "
                  "it means the run needed more time than the gate allows "
                  "(DDR-997 §13.4)." % arm)
            rc = 1
            continue
        cands = [r for r in recs
                 if (r["edge"] & want[arm]) and not (r["edge"] & excl[arm])]
        if not cands:
            print("[resizeall] FAIL — arm %s: no PRADYOS_RESIZE_FIX line with "
                  "edge&%d for id=%s (%d FIX lines seen in total)"
                  % (arm, want[arm], sid, len(recs)))
            rc = 1
            continue
        # ARM CONTAMINATION (CI run 33623855907, shard 9, tip 87321b0).
        #
        # This loop used to require EVERY clause of check() of EVERY record
        # carrying the arm's edge bit, on the reasoning quoted in the module
        # docstring: "a repeated drag is a second independent observation".
        # That reasoning is right for a repeated arm e. It is wrong here,
        # because NOTHING IN A FIX LINE SAYS WHICH ARM PRODUCED IT — the arm is
        # inferred from the edge bitmask alone.
        #
        # What happened: the injector missed arm w's press ("no RESIZE_TRACK
        # within 20s — compositor never observed the drag; retrying this
        # round") and retried. The abandoned attempt had already dragged the
        # pointer to BETA's east handle, so the compositor saw a legitimate
        # EAST grab and committed edge=8 w 157 -> 150. The retry then did arm w
        # correctly. So arm e — which had itself succeeded, 64 -> 157 — was
        # failed by a record another arm's discarded round produced.
        #
        # The split fixes it without weakening anything:
        #   invariant() must hold for EVERY record. It is DDR-997's actual
        #     property (the fixed edge held) and it is what M1 and M2 break, so
        #     the mutants are still caught by every observation.
        #   liveness() need hold for at LEAST ONE. It says the injector managed
        #     to perform the drag it intended — a statement about the harness,
        #     not the compositor — so a foreign record cannot fail it, and an
        #     arm that never really ran still cannot pass it.
        failed = False
        for r in cands:
            for msg in invariant(arm, r):
                print("[resizeall] FAIL — arm %s: %s" % (arm, msg))
                print("            line: %s" % " ".join(
                    "%s=%d" % (k, r[k]) for k in KEYS))
                failed = True
        live = [r for r in cands if not liveness(arm, r)]
        if not live:
            print("[resizeall] FAIL — arm %s: no observation shows the drag the "
                  "injector intended (%d candidate record(s) carried edge&%d, "
                  "none of them live). Reasons for the last one:"
                  % (arm, len(cands), want[arm]))
            for msg in liveness(arm, cands[-1]):
                print("            %s" % msg)
            print("            line: %s" % " ".join(
                "%s=%d" % (k, cands[-1][k]) for k in KEYS))
            failed = True
        if failed:
            rc = 1
        else:
            r = live[0]
            print("[resizeall] arm %s OK — (%d,%d %dx%d) -> (%d,%d %dx%d), %d "
                  "live of %d observation(s)"
                  % (arm, r["x0"], r["y0"], r["w0"], r["h0"],
                     r["x"], r["y"], r["w"], r["h"], len(live), len(cands)))
    return rc


if __name__ == "__main__":
    sys.exit(main())

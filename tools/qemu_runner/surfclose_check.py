#!/usr/bin/env python3
"""surfclose_check.py — assert DDR-998's ask-then-force protocol.

Arm A (courteous owner): the three lines must appear IN ORDER —

    PRADYOS_WM_CLOSE_REQ id=0 gen=N     compositor asked
    PRADYOS_SURF_SAVED id=0             owner flushed state
    PRADYOS_WM_CLOSE id=0 owner=1       owner closed itself

Order is checked by line index, not by presence. Presence alone would pass a
compositor that asked and force-closed in the same breath, which is the whole
thing the grace exists to prevent.

Arm B (owner ignores it): the surface must still die, on the compositor's
deadline rather than the owner's cooperation —

    PRADYOS_WM_CLOSE id=1 owner=0 secs=S frames=F

S and F are reported, not asserted: they are the measured grace the owner
declined to use (§NON-NEGOTIABLE 17 — a verdict needs a denominator).

usage: surfclose_check.py <logfile>
"""
import re
import sys


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    with open(sys.argv[1], "r", errors="replace") as fh:
        lines = fh.read().splitlines()

    def find(pat):
        """First line index matching pat, or -1."""
        rx = re.compile(pat)
        for i, ln in enumerate(lines):
            if rx.search(ln):
                return i
        return -1

    rc = 0

    # --- arm A: ask, save, self-close, in that order ---
    i_req = find(r"PRADYOS_WM_CLOSE_REQ id=0 ")
    i_sav = find(r"PRADYOS_SURF_SAVED id=0\b")
    i_cls = find(r"PRADYOS_WM_CLOSE id=0 owner=1\b")
    missing = [n for n, i in (("WM_CLOSE_REQ", i_req),
                              ("SURF_SAVED", i_sav),
                              ("WM_CLOSE owner=1", i_cls)) if i < 0]
    if missing:
        print("[surfclose] FAIL — arm A: missing %s for id=0" % ", ".join(missing))
        forced = find(r"PRADYOS_WM_CLOSE id=0 owner=0")
        if forced >= 0:
            print("            ALPHA was FORCED instead: %s" % lines[forced].strip())
        rc = 1
    elif not (i_req < i_sav < i_cls):
        print("[surfclose] FAIL — arm A: out of order (req@%d save@%d close@%d). "
              "The owner did not get its grace." % (i_req, i_sav, i_cls))
        for i in sorted((i_req, i_sav, i_cls)):
            print("            %d: %s" % (i, lines[i].strip()))
        rc = 1
    else:
        print("[surfclose] arm A OK — asked@%d, saved@%d, owner closed@%d"
              % (i_req, i_sav, i_cls))

    # --- arm B: the owner ignored it and was forced anyway ---
    i_breq = find(r"PRADYOS_WM_CLOSE_REQ id=1 ")
    forced = None
    for ln in lines:
        m = re.search(r"PRADYOS_WM_CLOSE id=1 owner=0 secs=(\d+) frames=(\d+)", ln)
        if m:
            forced = m
            break
    if i_breq < 0:
        print("[surfclose] FAIL — arm B: BETA never got a PRADYOS_WM_CLOSE_REQ")
        rc = 1
    elif forced is None:
        print("[surfclose] FAIL — arm B: BETA was asked but never forced. A "
              "grace with no deadline is a veto, which DDR-998 §9 forbids.")
        selfclosed = find(r"PRADYOS_WM_CLOSE id=1 owner=1")
        if selfclosed >= 0:
            print("            BETA closed ITSELF — it is supposed to ignore "
                  "type 4, so this arm is no longer testing the deadline.")
        rc = 1
    else:
        print("[surfclose] arm B OK — forced after %s s / %s frames of unused "
              "grace" % (forced.group(1), forced.group(2)))

    # A stale-generation drop is not a failure, but it must never go unseen.
    for ln in lines:
        if "PRADYOS_WM_CLOSE_STALE" in ln:
            print("[surfclose] note — %s" % ln.strip())
    return rc


if __name__ == "__main__":
    sys.exit(main())

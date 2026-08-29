#!/usr/bin/env python3
"""yieldstall_scan.py — pair DDR-994 [yieldstall] OPENED lines with their
RESOLVED partners and report spins that never ended.

WHY THIS EXISTS. `[yieldstall]` is deliberately NOT in GLOBAL_FORBIDDEN
(DDR-994: it reddened four shards on a signal never shown to be fatal), so a
stall sits SILENTLY inside a log the gate scored as PASSING. Nothing reads it
unless something like this does. DDR-1000 §8.3 uses it to test whether organic
`mnt_lock` stalls survive the DDR-989 vruntime fix.

The reporter emits, per stall:

    [yieldstall] site=<S> spins=<N> ticks=<T> pid=<P> cpu=<C>   (OPENED)
    [yieldstall] RESOLVED site=<S> spins=<N> ticks=<T>          (partner)

An OPENED with no RESOLVED partner **in the same boot** is a spin that never
ended — the whole signal. Equal counts mean the lock contended and recovered,
which is normal and is NOT a finding.

Two things this gets right that a naive version does not:

* **Pairing is per SITE, not per file.** A boot can stall on two different
  sites; comparing totals lets a resolved `selftest` mask an unresolved
  `mnt_lock`.
* **`pid=0` is the gate's own synthetic arm.** `smoke-yieldstall` arm B holds
  `mnt_lock` deliberately and reports `pid=0`. Counting that as organic
  contention would manufacture the very finding this is meant to test for —
  which is exactly the distinction that made DDR-1000 §8 readable.

Note for anyone grepping by hand: `[yieldstall]` is a CHARACTER CLASS in basic
regex, not a literal. `grep -c "[yieldstall]"` matched 430 of ~430 heartbeat
lines once (DDR-989 §9.15). Use `grep -F`.

usage: yieldstall_scan.py <logfile> [<logfile> ...]
"""
import re
import sys
from collections import defaultdict

OPEN_RX = re.compile(
    r"\[yieldstall\] site=(?P<site>\S+) spins=(?P<spins>\d+) "
    r"ticks=(?P<ticks>\d+) pid=(?P<pid>\d+)")
DONE_RX = re.compile(r"\[yieldstall\] RESOLVED site=(?P<site>\S+)")


def scan(path):
    """-> {site: {'opened': [line, ...], 'resolved': n}}"""
    per = defaultdict(lambda: {"opened": [], "resolved": 0})
    try:
        with open(path, "r", errors="replace") as fh:
            for ln in fh:
                if "[yieldstall]" not in ln:
                    continue
                m = DONE_RX.search(ln)
                if m:
                    per[m.group("site")]["resolved"] += 1
                    continue
                m = OPEN_RX.search(ln)
                if m:
                    per[m.group("site")]["opened"].append(
                        (int(m.group("pid")), ln.strip()))
    except OSError as e:
        print("skip  %s — %s" % (path, e))
    return per


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    scanned = with_any = organic = synthetic = 0
    for path in sys.argv[1:]:
        scanned += 1
        per = scan(path)
        if not per:
            continue
        with_any += 1
        for site, d in sorted(per.items()):
            unresolved = len(d["opened"]) - d["resolved"]
            if unresolved <= 0:
                print("ok    %s — site=%s %d opened / %d resolved "
                      "(contended, recovered)"
                      % (path, site, len(d["opened"]), d["resolved"]))
                continue
            # Attribute the unresolved ones to the LAST opens for this site;
            # a resolved partner necessarily belongs to an earlier open.
            for pid, ln in d["opened"][d["resolved"]:]:
                if pid == 0:
                    synthetic += 1
                    print("self  %s — site=%s pid=0 "
                          "(smoke-yieldstall arm B, synthetic)" % (path, site))
                else:
                    organic += 1
                    print("STALL %s — site=%s pid=%d NEVER RESOLVED"
                          % (path, site, pid))
                    print("        %s" % ln)

    print("\nscanned=%d  with_yieldstall=%d" % (scanned, with_any))
    print("organic_unresolved=%d  synthetic_unresolved=%d" % (organic, synthetic))
    if organic:
        print("VERDICT: organic stalls SURVIVE on this kernel — DDR-1000 §8.2 "
              "REFUTED.\n         The wait is independently unbounded, bounding "
              "it becomes the fix, and\n         these lines are the named "
              "mechanism §NON-NEGOTIABLE 3 requires.")
        return 1
    print("VERDICT: no organic unresolved stall across %d log(s) — consistent "
          "with\n         DDR-1000 §8.2 (the DDR-989 fix removed the trigger). "
          "CONSISTENT, not\n         proven: absence carries power only at the "
          "campaign's full N." % scanned)
    return 0


if __name__ == "__main__":
    sys.exit(main())

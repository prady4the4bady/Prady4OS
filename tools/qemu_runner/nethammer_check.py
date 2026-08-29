#!/usr/bin/env python3
"""nethammer_check.py — assert the hammer actually ran CROSS-CPU.

DDR-990 §13, raised by review on PR #14.

The gate already required two distinct PIDs to report `PRADYOS_NETHAMMER_OK`.
That proves two instances FINISHED. It does not prove they ever ran on
different CPUs — `QEMU_SMP=4` offers four vCPUs, it does not place threads.

DDR-990's claim is a **cross-CPU** use-after-free: one CPU walking
`pcb->unsent` inside `tcp_output` while another frees it. If both instances
happened to run on one CPU, the probe cannot exercise that race at all, and the
gate goes green having tested nothing. That is precisely the "validation gate
that may provide false confidence in the race fix" the review named, and it is
a fair hit.

Each instance ORs its CPUID-derived APIC bit into `cpumask` on every iteration
(sampled per iteration, not once at exit, because a thread can migrate — one
reading would report where it ended, not where it ran). The union across
instances must cover at least two distinct CPUs.

usage: nethammer_check.py <logfile> [min_cpus]   (min_cpus default 2)
"""
import re
import sys

RX = re.compile(r"PRADYOS_NETHAMMER_OK\s+.*?cpumask=(\d+)")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    need = int(sys.argv[2]) if len(sys.argv) > 2 else 2
    try:
        text = open(path, errors="replace").read()
    except OSError as e:
        print("[nethammer] FAIL — cannot read %s: %s" % (path, e))
        return 1

    masks = [int(x) for x in RX.findall(text)]
    done = len(re.findall(r"PRADYOS_NETHAMMER_OK", text))
    # A completion line that STARTS but carries no cpumask= was truncated by an
    # interleaving print (§INV.23) — measured once, DDR-990 §14. That is a
    # MEASUREMENT failure, not a CPU-placement failure, and reporting it as
    # "never ran on 2 CPUs" would send the next reader hunting the scheduler
    # for a console bug.
    if done > len(masks):
        print("[nethammer] FAIL — %d completion line(s) but only %d carry "
              "cpumask=." % (done, len(masks)))
        print("            A line was TRUNCATED mid-field, not a same-CPU "
              "run. Look for a foreign print spliced into it (INV.23); "
              "DDR-990 S14 made the line a single write to close this "
              "probe's own seams.")
        for ln in text.splitlines():
            if "PRADYOS_NETHAMMER_OK" in ln and "cpumask=" not in ln:
                print("            truncated: %s" % ln.strip()[:160])
        return 1
    if not masks:
        # Distinguish "no cpumask field" from "no completions at all": the
        # former means an old probe binary against a new gate, which is a
        # different problem from a hammer that never finished.
        print("[nethammer] FAIL — no PRADYOS_NETHAMMER_OK at all.")
        return 1

    union = 0
    for m in masks:
        union |= m
    n = bin(union).count("1")
    print("[nethammer] cpumask union=0x%x over %d instance(s) -> %d distinct CPU(s)"
          % (union, len(masks), n))
    for i, m in enumerate(masks):
        print("            instance %d: cpumask=0x%x (%d cpu(s))"
              % (i, m, bin(m).count("1")))
    if n < need:
        print("[nethammer] FAIL — the hammer instances never ran on %d distinct "
              "CPUs.\n            DDR-990's claim is a CROSS-CPU race; a "
              "same-CPU run cannot exercise it,\n            so a green result "
              "here would be false confidence." % need)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

# DDR-823 — the harness blamed the kernel for host problems (OPEN-9)

**Status:** Implemented
**Date:** 2026-08-03
**Closes:** the misattribution half of OPEN-9. Does **not** close OPEN-9 itself.

## §Problem

`boot_test.sh` launched QEMU with stderr **uncaptured**. When QEMU refused to
start, it exited before emitting a single serial byte, the capture file was
empty, and the harness reported:

```
[smoke] FAIL — kernel sentinel 'NEXUS KERNEL OK' not found.
```

That message names the kernel. The cause was the host.

The dominant trigger is a leaked `qemu-system-x86_64` from an earlier or
concurrent run still holding the image's write lock:

```
qemu-system-x86_64: -device virtio-blk-pci,drive=disk0,bootindex=0:
    Failed to get "write" lock
Is another process using the image [build/pradyos.img]?
```

## §Why this was expensive

It is **indistinguishable from a genuine boot failure at the harness level**,
and it has the three properties most likely to produce a wrong conclusion:

1. **It reproduces.** For as long as the orphan lives, the gate fails *every*
   time. "5/5 red" reads as deterministic evidence.
2. **It clears by itself.** When the orphan is reaped the gate passes again with
   no change to the tree — which reads as "the revert fixed it" or "the host
   recovered", depending on what you were doing at the time.
3. **It is silent about its real cause.** The one line that would have named it
   went to a stream nobody captured.

Those are precisely the recorded OPEN-9 symptoms. It twice caused a working
change to be blamed (DDR-816's entropy work, and the DDR-809 console change),
and the REVERT VERIFICATION RULE exists because of it.

## §Fix

- Capture QEMU stderr to its own temp file.
- Check it **before every other verdict**, including the DDR-791 global probe
  check. If QEMU never ran, nothing downstream carries information — the serial
  log is empty for a host reason, and every assertion after it would report a
  fault that did not occur.
- Report **exit code 3**, not 1. A host-environment failure and a failing gate
  are different events and must not be summed: "5/5 red" means something
  entirely different when all five never booted.
- A second, broader arm catches any other QEMU startup refusal (bad device,
  missing file, unsupported option) by matching QEMU's own
  `qemu-system-x86_64:` fatal prefix — but **only when the serial log is
  empty**, so a mid-run warning can never mask a real boot result.

## §Gate — three arms, all run

| arm | condition | expected | observed |
|---|---|---|---|
| **A** | a second QEMU deliberately holds the image lock | `HOST-ENV FAIL — STALE QEMU HOLDING IMAGE LOCK`, exit **3** | ✅ exactly that, and the old "sentinel not found" no longer appears |
| **B** | clean host, no contention | normal `PASS`, exit 0 | ✅ `PASS — saw 'NEXUS KERNEL OK'` |
| **C** | `make smoke-selftest` (DDR-785 harness self-test) | unchanged | ✅ early exit still works, still cannot mask a late forbidden pattern |

Arm C matters as much as A: this change inserts code into the verdict path that
every one of the 117 gates runs, so the assertion that the *existing* semantics
are untouched is not optional.

## §What this does and does not close

**Closes:** the misattribution. A host problem now says so, in its own words,
with its own exit code.

**Does NOT close OPEN-9.** OPEN-9 is "`smoke-shell` fails locally, passes CI".
The leaked-QEMU hypothesis explains every recorded symptom, but it has not yet
been caught red-handed *on a `smoke-shell` failure* — the evidence comes from
`smoke-x25519` runs. What this change buys is that the next occurrence will
**identify itself** instead of being mistaken for a kernel fault. That is the
condition under which OPEN-9 can actually be confirmed or refuted, rather than
argued about.

## §Operational rule, learned by violating it

**Never run two QEMU gates concurrently on one checkout.** They share
`build/pradyos.img` and the serial capture path. Several measurements in the
previous session were invalidated this way, including a `smoke-x25519` PASS that
therefore cannot be trusted. The harness now names the failure, but it cannot
make concurrent runs correct.

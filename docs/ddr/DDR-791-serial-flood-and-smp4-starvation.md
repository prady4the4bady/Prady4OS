# DDR-791 — serial-console flooding, and the `-smp 4` starvation signature behind BUG-1

**Status:** evidence recorded; root cause NOT yet pinned. No code change made.
**Date:** 2026-07-28
**Relates to:** BUG-1 (intermittent `-smp 4` gate failures), DDR-783/785/788
(timeout budget), DDR-790 (log-ring eviction).

## Why this exists

Four CI runs on `dev/phase1` went red, each on a **different** gate, none of
them on a commit that touched kernel code:

| run | gate that failed | head |
|---|---|---|
| 30326561378 | `smoke-rqstress` (q35 `-smp 4`) | `981c5ca` |
| 30323686134 | `smoke-swapgs` (q35 `-smp 4`) | `ff6d1d4` |
| 30303017178 | `smoke-dmesg` (q35) | `c0005b7` |
| 30302837305 | `smoke-blkmq` (q35 `-smp 4`) | `e7e52fd` |

All four gates pass on the six most recent commits. "A different gate each
time, only in CI, never locally" is the BUG-1 signature, so the useful question
is not "which gate is broken" but "what makes an arbitrary gate miss its
sentinel".

## Finding 1 — the serial console is ~70% binary garbage

Measured locally, one 40 s boot of `build/pradyos.img`:

```
total serial bytes = 97 421
non-printable      = 80 640   (83%)
blob runs          = 42 × 3 188 bytes = 68 103 bytes
```

The blob decodes unambiguously as the **`syscallfuzz` user ELF image**. Its
`.rodata` is present verbatim — `06 09 0b 1b 35 40 42 43 47 48 49 4a 4b` as
8-byte little-endian longs is exactly `SAFE[]` from `user/syscallfuzz.c`,
immediately followed by `WILD[]` (`ef be ad de` = `0xdeadbeef`, and `AAAA` =
`0x41414141`).

So something reads that ELF and writes its bytes to the console, 42 times per
boot.

### What it is NOT (both killed by A/B experiment)

The obvious suspect was the fuzz probe itself — `user/syscallfuzz.c` issues
wild-pointer syscalls, and `SYS_WRITE` is on its `SAFE` list. Two controlled
runs, identical boot window, identical measurement:

| arm | non-printable bytes |
|---|---|
| baseline | 80 640 |
| `SYS_WRITE` (6) removed from `SAFE[]` | 80 640 |
| wild-pointer branch disabled entirely (bad NSI numbers only) | 80 640 |

Byte-identical. **The fuzz probe does not emit the blob.** The file's own
comment ("wild bufs fault at copyin (-EFAULT), so nothing is emitted") is
correct; the blob merely happens to land next to `PRADYOS_FUZZ_OK` in the log,
which is what made it look causal. The emitter is elsewhere and is still
unidentified — `user_boot_from_sfs()` writes each ELF into SFS and reads it back
through a 256 KiB PMM buffer, which is the nearest path that has the bytes in
hand, but nothing there prints the buffer, so this is not yet a conclusion.

### Why it matters even so

* It is 70% of serial bandwidth, on every gate, all boot long.
* DDR-790 already proved this exact pressure evicts `smoke-dmesg`'s marker from
  the last-4 KiB log ring — and `smoke-dmesg` is one of the four failures above.
* On failure `boot_test.sh` `cat`s the whole log into the job output, so CI logs
  are megabytes of binary, which is why these runs are hard to read.

## Finding 2 — a starvation signature, not a timeout-budget shortfall

`smoke-rqstress` allows **180 s** and still never printed `[smp] rqstress OK`.
The tail of that boot is:

```
PRISM_READY
prism> [smp] user-on-AP probe t=556
[smp] user on AP OK t=556
[blk] multi-inflight OK
PRADYOS_INPUT_TIMEOUT
```

Separately, the same job log contains:

```
AGENT_METRICS FAIL: agent never observed as scheduled
```

That string **is** declared `FORBIDDEN_SENTINEL` — but only for
`smoke-agentmetrics`. Every boot runs every in-kernel probe, so it appeared
during a different gate's boot, where nothing was watching for it, and the run
was not failed by it.

Two independent probes in the same run therefore report the same thing: **under
`-smp 4` on a loaded CI runner, work that should be scheduled is not observed to
run.** That is a far better fit for the four scattered failures than "each gate
needs a bigger timeout" — raising windows (DDR-788) did not retire this class.

## What is NOT concluded

* The emitter of the binary blob. Two hypotheses were tested and both are dead;
  guessing a third and "fixing" it would be patchwork.
* Whether the flooding *causes* the starvation or merely co-occurs with it. The
  flooding is real and worth removing on its own merits, but no experiment here
  establishes causation, and `smoke-dmesg` is the only one of the four with a
  proven mechanical link (DDR-790's ring eviction).

## Next steps, in order

1. Bisect the blob emitter by instrumenting the console write path with the
   caller's identity (not by disabling suspects one at a time — that already
   cost two runs for two dead ends).
2. With the console quiet, re-measure whether `AGENT_METRICS FAIL` and the
   rqstress miss still reproduce under `-smp 4`.
3. Only then decide whether BUG-1 is a scheduler defect or an observation
   artefact. Do not widen timeouts further; DDR-788 already showed that does not
   retire this class.

## Coverage gap worth closing regardless

`AGENT_METRICS FAIL` is only forbidden on one gate. Any `... FAIL` sentinel a
probe can print should be forbidden on **every** gate that boots the same image,
otherwise a genuine failure is silently tolerated by whichever gate happens to
be running. That is a harness change, tracked here so it is not lost.

# DDR-1192: `smoke-iso-userspace` keeps its evidence when it fails

Status: **implemented** in this PR, approved by the operator (#45 comment
6497687748, item 3). Diagnostics only: no assertion, sentinel or bound moves.
Platform: QEMU on GitHub-hosted runners. Nothing here is verified on real hardware.

## 1. Why

`smoke-iso-userspace` failed on `2a49a73` (run 40230621123, shard 0) on
`[iso-user] FAIL: surface lifecycle`. The cause is still **unexplained**
(#45 comment 6497687748). The run left two things to read:

- the gate's `tail -30 build/iso_user.log`, which held 30 heartbeat lines with
  pids 31 and 33 alternating on one CPU, and nothing from the compositor or the
  surface client;
- no artifact. `build/iso_user.log` lives only on the runner, and
  `build-and-boot` uploads nothing.

So the one failure has no evidence that could name a mechanism. If it happens
again, the same would be true.

## 2. Change

1. **Makefile.** On the two graphics FAIL lines (`no framebuffer from the ISO` and
   `surface lifecycle`), after the existing tail, the gate prints every line of
   `build/iso_user.log` that carries a marker printed by `user/compositor.c` or
   `user/surfacetest.c`, up to the last 120 lines, plus the count. The marker
   list is read from those two sources at gate time
   (`grep -ohE 'PRADYOS_[A-Z0-9_]+'`, 75 markers today, saved to
   `build/gatelogs/iso_surf_markers.txt`), so a new marker needs no edit here.
2. **ci.yml.** `build-and-boot` uploads `build/iso_user.log` as
   `iso-user-log-shard-<n>` when the shard fails (`if: failure()`, and only once
   the build artifact arrived). Only shard 0 runs this gate, so other shards find
   no file (`if-no-files-found: ignore`). The log is kept for 14 days.

Neither runs on a green shard. A pass costs nothing.

## 3. Evidence

This is not a gate, so it gets no mutant in the sense of CLAUDE.md rule 3. It
does need to be seen working on a real failure, because a FAIL-only path that
was never taken proves nothing.

- **Local:** the macro run against a hand-made log printed exactly the three
  marker lines and skipped the heartbeat and noise lines. Not a boot.
- **CI demonstration (L1):** a branch on this head whose `surfacetest` prints
  `PRADYOS_SURFACE_CLIENT_0K` (a zero, not an O) must
  - fail shard 0 on `[iso-user] FAIL: surface lifecycle`;
  - print the DDR-1192 block, with compositor lines in it;
  - upload `iso-user-log-shard-0`.

  The run is recorded on the PR. This head must also pass with no change to any
  gate.

## 4. Stacking

This branch is on DDR-1191 (#97, the `ubuntu-24.04` pin), because no
`pradyos-ci` run can reach a boot without it. Once #97 is in `dev/phase1`, that
part of the diff is a no-op.

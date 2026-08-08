# DDR-865 — the local kernel build is blocked by Windows Defender, not by code

**Status:** Accepted
**Date:** 2026-08-07
**Scope:** Developer environment. Also closes Group 7 item 41.

## The symptom

`make kernel` on the Windows checkout fails at the final step:

```
llvm-objcopy -O binary build/kernel.elf build/kernel.bin
llvm-objcopy: error: 'build/kernel.elf': Invalid argument
```

CI is green on the same commit, so the code is fine.

## The cause

Not objcopy. **Nothing** can read the file — `cp` fails identically, and Windows
itself reports:

> Operation did not complete successfully because the file contains a virus or
> potentially unwanted software.

**Windows Defender quarantines `build/kernel.elf`.** A freestanding x86_64 kernel
ELF — no libc, raw entry point, executable stack sections — matches a heuristic.
The file is left visible with a plausible size, which is why the first read of
the failure looks like a toolchain bug: `ls` shows 1,017,984 bytes and every
read returns `EINVAL`.

## Decision — build on WSL's native filesystem

The repository stays on `/mnt/c` (git, editing). The **build** happens in
`~/pradyos-build` on ext4, mirrored with rsync.

Chosen over the alternative deliberately: adding a Defender exclusion would work,
but it is a change to the operator's security posture, and that is theirs to make
rather than a side effect of a build fix. Building on ext4 also matches CI's
filesystem semantics, so a local pass means slightly more than it did before.

**Measured result: the full `image` target builds, and `make smoke` passes in
0.6 s** — DDR-785's early exit fires as soon as the sentinel appears, so local
gating is far cheaper than the ~2–3 min per gate previously assumed. Local
kernel verification is viable, which materially changes what can be built
without waiting on CI.

### One rsync trap, recorded because it cost a build

`--exclude 'build/'` is **not** anchored: it also matched `tools/build/`, which
holds `toolchain.mk`, and the first build died with
`No rule to make target 'tools/build/toolchain.mk'`. The pattern must be
`/build/`. Trivial, and it looked exactly like a missing file in the repo.

## Group 7 item 41 — Wayland compositor: SUPERSEDED, not required

Recorded here as the operator pre-approved it and no code is involved.

The in-house C framebuffer compositor (`user/compositor.c`, DDR-704) already
renders both ambiances, handles input, windowing, focus and drag, and is proven
by ~25 gates (`smoke-compositor`, `-visual`, `-focus`, `-drag`, `-alttab`,
`-glassblur`, and the rest). A wlroots/Wayland port would replace working,
gate-proven code with a large out-of-tree dependency chain (libdrm, EGL, pixman)
for no capability this release needs.

**Status: superseded by 8.1, not required for v1.0.0.** It should not appear in
release notes as an unmet item.

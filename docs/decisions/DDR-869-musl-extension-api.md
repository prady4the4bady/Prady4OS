# DDR-869 — the musl extension API header (Group 6 item 32)

**Status:** Accepted
**Date:** 2026-08-08
**Scope:** `user/include/pradyos.h`, `tools/build/gen_nsi_header.sh`, Makefile.
Ring-3 only.

## The problem

musl gives ring-3 the POSIX surface. PRADYOS adds ~50 calls on top — agents,
surfaces, the vault, the audit log, mode switching — and every ring-3 program
reached them by re-declaring `SYS_*` numbers and its own inline syscall stub.
**Around forty probes carry that boilerplate**, each an independent chance to
mistype a number.

That is not hypothetical. Writing `user/ftrunctest.c` in this same session, I
declared `SYS_FSTAT 20`; it is **9**. The probe would have failed loudly, but
only because it happened to check the return value.

## Decision

**The numbers are generated, never copied.** `tools/build/gen_nsi_header.sh`
emits `build/musl/include/pradyos_nsi.h` from `kernel/syscall/syscall.h`, which
stays the single source of truth. `pradyos.h` includes it and adds typed
wrappers.

A hand-maintained table in a header would be a second list that drifts silently
— the defect this project has hit five times (DDR-817, -822, -825, -833, -835).
Generating makes disagreement impossible rather than unlikely.

**The generator fails rather than emitting a partial header.** It aborts on zero
parsed defines, and cross-checks its own count against the kernel header's. An
empty or short header still *compiles* for any program that happens not to use
the missing call, so the breakage would surface later and somewhere unrelated.

**Wrappers return the RAW kernel value** — `>= 0` on success, negative errno on
failure — and deliberately do **not** set `errno` and return `-1` the way musl
does. Ring-3 probes assert on exact values (`== -EINVAL`), and collapsing every
failure to `-1` would make those assertions impossible. That is precisely how
DDR-867's negative-length check went untested for a while.

**It is not a libc.** POSIX calls musl already provides are not re-wrapped;
competing wrappers would give two ways to do the same thing that differ in
errno handling.

## Verification

The anti-drift property is the claim worth testing, so it was tested directly:
`SYS_FTRUNCATE` was renumbered **94 → 95** in the kernel header, and

- the generated header immediately reported `95`, and
- `smoke-ftruncate` **passed at the new number** with no userspace file touched.

Ring-3 followed the kernel automatically. A copied table would have kept saying
94 and the probe would have called the wrong syscall.

`user/ftrunctest.c` is converted to the header as proof it works in a real
probe — a header nothing uses is unverified. It dropped seven hand-copied
defines and its private syscall stub.

Regression green: `smoke-ftruncate`, `smoke`, `smoke-shell`, `smoke-user`.
Zero warnings under `-Werror`. Generator reports 91 syscall numbers, matching
the kernel header exactly.

**Group 6 item 32 complete.**

## Deliberately not done

The other ~39 probes are **not** converted. Each conversion is a behaviour-
neutral edit that still needs its gate re-run to prove it, and doing forty in
one commit would put a large untested diff under a single verdict. They can move
individually as each is next touched.

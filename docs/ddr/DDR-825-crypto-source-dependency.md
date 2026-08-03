# DDR-825 — DDR-822's fix stopped one directory short

**Status:** Implemented
**Date:** 2026-08-03
**Found by:** adding `fe25519.c` and watching a build succeed that never ran.

## §Problem

DDR-822 replaced a hand-written prerequisite list with
`$(wildcard user/*.c) $(wildcard user/*.h)`. That fixed `user/`.

**The probe ELFs also link `kernel/crypto/*.c`**, and **the Makefile itself**
decides which objects get linked. Neither was a prerequisite of
`$(KERNEL_BIN)`. So:

- editing a crypto source produced `make: Nothing to be done for 'image'.`
- editing the *recipe* that links it did the same
- and the next gate ran against the **previous** binary

Same defect as DDR-822, one directory over. The wildcard fixed the directory
that had just burned us and stopped there.

## §How it surfaced

While extracting the shared field layer for Ed25519 (DDR-821 §Sequencing):

1. `kernel/crypto/fe25519.{c,h}` created, `x25519.c` reduced to the group law.
2. Makefile edited to compile `fe25519.c` and link `fe25519_user.o`.
3. `make image` → no errors. `ls` reported `x25519test.elf` at 14040 bytes,
   *different* from the 14056 before, which read as a successful rebuild.
4. **`build/fe25519_user.o` did not exist.**

The `.elf` had changed size for an unrelated earlier reason; the link that was
supposed to include the new object never ran. Had the freshness check
(`[ elf -nt source ]`) not been part of the pre-gate routine, the next
`smoke-x25519` would have exercised the OLD x25519.c — the one that still had
the field layer inlined — and its PASS would have said nothing about the
refactor it was supposed to be regression-testing.

That is the precise trap DDR-822 was written about, and it was caught only
because rule 4 exists.

## §Fix

```make
USER_ALL_SRCS := $(wildcard user/*.c) $(wildcard user/*.h) \
                 $(wildcard kernel/crypto/*.c) $(wildcard kernel/crypto/*.h) \
                 Makefile
```

`Makefile` is listed deliberately. A recipe change alters what gets linked, so
the recipe is an input to the artefact in exactly the way a source file is.
Leaving it out means "I changed which objects are linked" does not trigger a
relink — which is how step 3 above reported success.

**Verified:** after the fix, `make image` compiles `fe25519.c`, produces
`build/fe25519_user.o` (8256 B), relinks `x25519test.elf`, and the freshness
assertion passes.

## §Why this keeps happening — sixth instance

| # | Where | Silent drop |
|---|---|---|
| 1 | `ci.yml` gate list (DDR-817) | 8 gates never ran in CI |
| 2 | Makefile `user/` sources (DDR-822) | 14/31 probes tested stale |
| 3 | `user/` `_start` attribute (DDR-823) | a new probe reintroduces a #GP |
| 4 | `syscall_register()` (DDR-823) | NSI ≥ 80 registered into the void |
| 5 | `check_global_forbidden()` (DDR-824) | the `op=` line naming the defect |
| 6 | **crypto sources + Makefile (here)** | **a build that reports success and does not run** |

Every one is the same shape: **something that must stay in sync with reality is
maintained by hand, and drifting is silent and looks like success.**

DDR-822 is instructive as a *partial* fix. It correctly identified the pattern
and correctly derived the list — for one directory. The generalisation ("what
else feeds this artefact?") was not made, so the same bug survived in the
adjacent directory and cost another debugging cycle.

**The rule extends: when deriving a list to replace a hand-maintained one, ask
what else belongs in that list before declaring it fixed.**

## §Residual gap, stated rather than left implicit

`$(KERNEL_CS)` still enumerates kernel C sources by hand elsewhere in the
Makefile. That list is *checked* by the linker — a missing object is an
undefined symbol, which is loud — so it does not have this failure mode. It is
recorded here so a future reader does not mistake its absence from this fix for
an oversight.

# DDR-811 — SHA-256 in the kernel, validated against published vectors

**Status:** design accepted; implemented in this slice.
**Date:** 2026-08-01
**Prerequisite for:** DDR-812 (§S5 metric lockbox), §J-03 (audit chain),
DDR-813 (ACC), DDR-814 (AGS). None of those can be built before this.
**Relates to:** DDR-795 (`metric_page`, already declares `METRIC_ROOT_LEN 32
/* SHA-256 */`), DDR-810 (which identified this as a missing prerequisite).

## §Problem

Four downstream features need a cryptographic hash and none can proceed without
one. The tree has none.

Tree check, and a false positive worth recording so nobody repeats it: a naive
`grep -rli sha256 kernel/ tools/` returns **23 files**, which looks like the
primitive already exists. Every one of them is inside
`tools/graph_mcp/node_modules/` — third-party JavaScript dependencies of the
graph MCP server — plus the graph's own sqlite index. Both are gitignored and
untracked. There is **no** hash primitive in kernel or host tooling.

## §Design

`kernel/crypto/sha256.c` + `kernel/crypto/sha256.h`. New subsystem directory,
consistent with the no-flat-files-in-`kernel/` rule.

* **Pure C. No hardware acceleration**, no SHA-NI, no AES-NI, no `-msse4.2`.
  This is not conservatism: the same object must build for **riscv64 and
  aarch64** (ADR-034 ports), neither of which has x86 crypto extensions. A
  hardware-dependent path would compile on x86_64, pass its gate there, and fail
  at runtime on the other two targets — the worst possible failure shape,
  because the gate that should catch it is the one that passes.
* **No dynamic allocation.** The caller supplies `sha256_ctx` on the stack. The
  kernel's allocator is not available at every call site this will eventually
  have (early boot, shutdown path, panic path).
* **No stdlib.** Kernel types only, matching every other file in `kernel/`.
* Three-call API: `sha256_init` / `sha256_update` / `sha256_final`. `update` is
  streaming, so a caller can hash a 4 KB record or a whole kernel image without
  either buffering it or knowing the length in advance.

## §Test vectors — the whole point of this slice

Four vectors from FIPS 180-4 / the NIST CSRC examples:

| # | input | why it is here |
|---|---|---|
| 1 | `"abc"` | the canonical single-block case |
| 2 | `""` (empty) | length-zero padding; a surprising number of implementations get the "pad an empty message" case wrong |
| 3 | `"abcdbcde…nopq"` (56 bytes) | **two-block**, and 56 bytes is exactly the boundary where the length field no longer fits in the first block, forcing an extra padding block |
| 4 | 1,000,000 × `'a'` | multi-block streaming plus a bit-length that exceeds 32 bits' worth of bytes — catches length-counter truncation and `update`-across-block-boundary bugs that vectors 1–3 cannot |

Vector 4 is **not optional**. Vectors 1–3 all fit in two blocks and would pass
against an implementation whose length counter overflows or whose `update`
mishandles a partial buffer carried across calls.

**On provenance, stated plainly:** these digests are written from knowledge of
the published constants, not copied from a fetched document — this environment
has no network. That is exactly why the gate compares raw bytes: if any constant
here is wrong, arm C fails and the error surfaces immediately rather than
shipping. An implementation and a vector that are wrong in the *same* way is the
only silent failure mode, and that cannot happen when the vectors are published
constants and the implementation is written independently of them.

## §Blast radius

At the time of writing: **none**. `kernel/crypto/sha256.c` has no callers. The
only integration points are the new `user/sha256test.c` probe and the Makefile
object list. Downstream callers arrive in DDR-812/813/814 and §J-03; each will
record its own blast radius.

The probe links the **same source, compiled a second time** with the user code
model — a literal shared `.o` is impossible (kernel objects are
`-mcmodel=kernel`, user objects `-mcmodel=large`). The point that matters holds:
there is ONE implementation. A probe that reimplemented SHA-256 would test the
probe, not the kernel.

`sha256.o` is deliberately **not** in the kernel link yet — the kernel has no
caller until DDR-812, and an unreferenced object in the image is dead code.

## §Gate — `smoke-sha256`

Opt-in via the DDR-804 fw_cfg pattern (`QEMU_PROBES=sha256`).

* **Arm A** — primitive unlinked → **the artefact cannot be built at all**.
  (Originally written as "fails to link"; see Results — that phrasing was
  vacuous twice before it became true.)
* **Arm B** — present, but vector 4 (1M `'a'`) wrong → **FAIL**. This arm is the
  reason the 1M vector exists; it is what catches padding and length-counter
  bugs the short vectors miss.
* **Arm C** — all four vectors match byte-for-byte → **PASS**.

`FORBIDDEN_SENTINEL: PRADYOS_SHA256_STUB`, appended to `GLOBAL_FORBIDDEN`
(append-only, S3).

**Mechanism metric:** the probe compares all 32 digest bytes against the
embedded constant and prints the *index of the first mismatching byte* on
failure. Asserting "the function returned 0" or "the digest is non-zero" would
pass against an implementation that returns a fixed array, which is precisely
the stub this gate exists to reject.

## Results — and three corrections the A/B needed before it tested anything

Final verification, kernel `efd50b84c863`: `smoke-sha256`, `smoke`, `smoke-user`,
`smoke-fs`, `smoke-syspipe`, `smoke-sigpipe`, `smoke-shell` all **PASS**. All
four FIPS 180-4 vectors match byte-for-byte.

| arm | kernel | verdict |
|---|---|---|
| A — primitive unlinked | *(no artefact produced)* | **cannot build** |
| B — partial-block carry broken | `94d9ea8dcdf9` | **FAIL** |
| C — correct | `efd50b84c863` | **PASS** |

### The probe was testing less than this document claimed

The §Test-vectors section says the 1M-`'a'` vector exercises `update()` carrying
a partial block across calls. The first probe fed it in **64-byte** chunks —
block-aligned, so `buflen` is 0 on every call and the carry path never executes.
The gate passed while testing nothing of the sort.

Now fed in 1000-byte chunks (`1000 = 15*64 + 40`), so every call leaves a
40-byte remainder the next must top up, and the boundary walks all 64 offsets
over the run. This is also what makes arm B a real bug class rather than an
arbitrary corruption: arm B breaks exactly that carry, vectors 1-3 are
unaffected, and only the 1M vector catches it.

### Arm A took three attempts, each vacuous in a different way

1. **"build fails to link"** — vacuous. The kernel has no `sha256` caller yet, so
   removing the object from the kernel link changes nothing. This also exposed a
   real defect: an unreferenced object in the kernel image is dead code
   (CLAUDE.md). `sha256.o` is therefore **not** in the kernel link; it joins in
   DDR-812, its first caller.
2. **Drop it from the PROBE link** — also vacuous. `ld -nostdlib` with a linker
   script does not error on undefined symbols; it resolves them to 0 and emits a
   binary. An assumption about the toolchain, not a fact about it.
3. **Assert on the gate** — returned PASS with a kernel SHA **identical to arm
   C**. That is DDR-791's tell: editing the Makefile does not invalidate
   `sha256test.elf` or `user_image.o` by timestamp, so the arm re-ran arm C's
   binary. Only after deleting the derived artefacts did it rebuild — and then
   the build cannot complete at all, which is the strongest form of the
   assertion.

Arms B and C were sound throughout, because editing `sha256.c` *does* trigger a
rebuild. Only arm A was broken, and **printing the SHA per arm is the only
reason it was caught**.

### On vector provenance

The four digests were written from knowledge of the published constants; this
environment has no network. The gate comparing raw bytes is what makes that
safe: a wrong constant fails arm C immediately. Since the implementation was
written independently of the constants, both being wrong in the *same* way is
not a reachable failure mode — so arm C passing is evidence for both.

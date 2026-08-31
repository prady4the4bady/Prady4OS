# DDR-1024 — OPEN-13: DDR-986's instrument, built and proven

**Status:** IMPLEMENTED + mutation-proven. **Not a fix** — it converts the next
OPEN-13 occurrence from *"some 128-byte object, somewhere"* into two named call
sites, which is what §NON-NEGOTIABLE 3 requires before a fix is permitted.

---

## 1. Why now

DDR-986 designed this instrument in full and it was **never built** — `grep` for
`__builtin_return_address` in `kernel/mm/kheap.c` returned nothing. OPEN-13 is
one of three still-open issues, and DDR-1023 has just established that OPEN-2's
local reproduction route is exhausted and its remaining evidence is CI-side. The
same is true of OPEN-13: **one capture, in CI, never locally.** An instrument is
the only thing that makes the next one readable.

Built to DDR-986's design as written, including both of its self-corrections.

## 2. What was built

**The freeing return address is captured at the PUBLIC boundary and threaded
down** — `kfree()`, `pcb_free()`, `cap_free()`, `ipc_free()` each take
`__builtin_return_address(0)` and pass it through `kfree_locked`/`pool_free` into
`cache_free(c, ptr, site)`.

This is DDR-986 §4/§5's correction and it is load-bearing: `cache_free` is
`static` and reached through two different wrappers, so a builtin *inside* it
would name `kfree_locked` or `pool_free` rather than the caller — and then
`freed_by` and `now_by` would be two different stack frames, making any
comparison between them meaningless.

**Storage: offset 16 of the free object.** Layout while free is `next@0`,
`canary@8`, so 16..23 is the first unused slot. Written after the `memset`, into
a line that `memset` just touched, so it costs no additional cache line.

**Guarded by `obj_size >= 24`.** The 16-byte class and the dedicated `cap` cache
(also 16) have no room and keep today's `ptr=`/`objsize=` output unchanged.
OPEN-13 is the 128 class, so the one case on record is covered.

**On whenever `KHEAP_DEBUG` is on**, i.e. shipped — not opt-in. DDR-986 §3 is
right about why: `cache_free` already walks the whole free list (up to 31 entries
for the 128 class) and `memset`s 128 bytes on *every* `kfree` under
`KHEAP_DEBUG`; one 8-byte store is noise beside that. And an opt-in instrument
would be **off in CI, the only place OPEN-13 has ever appeared** — it would
guarantee its own uselessness.

## 3. Proven — M1

A probe-gated deliberate double-free of a 128-class object, two `kfree` calls
from the same function. Mutant kernel `18ecdfe77265e799`:

```
[kheapdf] first free
[kheapdf] second free (expect the detector)
[kheap] double-free ptr=0x0000000007FD1BA0 objsize=0x0000000000000080
        freed_by=0xFFFFFFFF800052DC now_by=0xFFFFFFFF800052F4
```

Resolved against **that exact binary** (§NON-NEGOTIABLE 18, DDR-986 §5):

| field | address | symbol |
|---|---|---|
| `freed_by` | `0xFFFFFFFF800052DC` | `fs_test_thread + 0x2FBC` |
| `now_by` | `0xFFFFFFFF800052F4` | `fs_test_thread + 0x2FD4` |

`0x18` apart — the two injected call sites. Both fields are populated, distinct,
and resolve to real call sites. `objsize=0x80` is the class OPEN-13 was seen in.

**This is what the instrument is for:** the original capture gave only
`objsize=0x80`, which DDR-980 correctly noted is a *generic* kmalloc class (any
`kmalloc(65..128)`), so the size→structure mapping did not resolve. Two call
sites do resolve.

## 4. Measured

Baseline kernel **`0e9dfefadf54d6ba`**, `-Werror` clean, **1,134,986 B** —
byte-for-byte the same size as before the change, since the store lands in
existing free-object space.

Gate suite on the baseline, one hash verified before and after each run:
`smoke-shell` (73-pattern forbidden scan clean), `smoke-blkmq`, `smoke-fsrm`,
`smoke-blk-integrity` — all PASS. `hygiene_check.sh` ALL THREE PASSED.
`smoke-fsrm` and `smoke-blk-integrity` were chosen deliberately: they are the
heaviest `kfree` traffic in the suite, so they exercise the new store on the
common path rather than only the detector branch.

## 5. What this does NOT do

It does not fix OPEN-13, does not explain it, and **must not be reported as
having closed it**. There is still exactly one capture, from CI, on a docs-only
commit (so not a regression), and no mechanism is named.

What changes is that the next occurrence is diagnostic instead of dead-ended.
When one appears: resolve **both** addresses against the exact kernel binary that
produced the log — every build loads at the same base, so an address alone does
not identify a build — and only then does §NON-NEGOTIABLE 3 permit a fix.

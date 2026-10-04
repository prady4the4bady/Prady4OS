# DDR-1158 — KASAN use-after-free regression gate (`smoke-kasan-uaf`)

Operator-requested: DDR-1155 §4 / PR #27 comment 5971011693 asked for a standard
use-after-free regression test of the IMP-B PMM poison. This is that test. It is
**NOT** a reproduction of U1/U2/U3 and it **closes nothing** (DDR-1155 §5) — it is
a regression net under a mechanism that already works, exactly as asked.

## §1 The gap (measured, not asserted)

IMP-B stamps `PMM_POISON` (`0xDEADBEEFDEADBEEF`) over every qword of a freed block
in `pmm_free_pages` (`kernel/mm/pmm.c`, the `#ifdef KASAN` fill loop; `KASAN ?= 1`
is the Makefile default). The only gate that watched this was `smoke-pmm-poison`,
and its `EXTRA_SENTINEL` is **`[pmm] poison enabled`** — the init-time *banner*,
printed from `pmm_init`, not from the free path. So a regression that dropped the
fill loop while leaving the banner would pass `smoke-pmm-poison` unchanged.

That is not hypothetical: §4's mutant drops the fill and `smoke-pmm-poison` stays
green on it. The banner-only gate cannot see a dead poison fill, which is the one
thing it exists to protect.

## §2 The design

`pmm_kasan_uaf_selftest()` (new, `pmm.c`, called from `kmain` right after
`pmm_selftest`) does a **deliberate use-after-free READ** and asserts the read
returns the poison rather than the stale live value:

```c
uint64_t pg = pmm_alloc_page();
volatile uint64_t *vp = (volatile uint64_t *)(uintptr_t)pg;
const uint64_t live = 0x1111111122222222ULL;   /* obviously not the poison */
... cli window ...
vp[1] = live;                 /* live write at offset 8 while allocated */
pmm_free_page(pg);            /* IMP-B stamps PMM_POISON over the frame   */
uint64_t seen = vp[1];        /* DELIBERATE use-after-free read, offset 8 */
... sti window ...
seen == PMM_POISON ? PRADYOS_KASAN_UAF_OK : KASAN_UAF_FAIL
```

It is **deterministic and coalescing-independent**, which is why it reads *its own
frame's offset 8* and not offset 0:

- the fill loop stamps **every** qword of the block;
- `list_push` rewrites only **offset 0** (the free-list `next`);
- XOR-buddy coalescing touches only the buddy's and the merged head's **offset 0**.

So offset 8 of the frame just freed is `PMM_POISON` whether or not it coalesced —
no assumption about allocator state. The frame lives in the identity-mapped
`[16 MiB, 1 GiB)` window, so the UAF read cannot fault. The `#else` (KASAN=0) arm
prints `PRADYOS_KASAN_UAF_SKIP`; no gate requires the OK sentinel on a KASAN=0
build, so the skip is harmless.

## §3 A self-deadlock caught and fixed while building this (the real finding)

The first draft wrapped the free+read in pmm's own `irq_save()` / `irq_restore()`.
It **hung the boot** — serial stopped after `pmm_selftest` with neither sentinel,
QEMU killed by timeout. Localised with `[kuaf] A..G` probes to *inside*
`pmm_free_page` (between "wrote" and "freed").

Root cause: under ADR-030 stage 1, pmm's `irq_save()` / `irq_restore()` are macros
that `spin_lock_irqsave(&g_pmm_lock)` / `spin_unlock_irqrestore` — names kept for
call-site compatibility, but they **take the non-recursive `g_pmm_lock`**. Holding
it across `pmm_free_page()`, which re-acquires it, **self-deadlocks the CPU.**

Fixed with a **bare CLI window** (inline `pushfq; pop; cli` … `push; popfq`) rather
than pmm's lock-wrapping macros. The window only has to stop a timer-driven re-hand
of the frame between the free and the read; APs are not online this early in
`kmain`, so one CPU's own interrupts are the only concern. A real hazard of calling
a lock-taking helper from a test that then calls into the same lock — recorded so
the next test author does not reach for `irq_save()` by its misleading name.

## §4 Proof (all on the `dev/phase1-seyp3n` tree, INV.18)

Shipping binary **`af38fc632e726793`**, 1,474,954 B, warning-clean at `-Werror`.
(Size unchanged from the DDR-1153 figure — the ~60-line addition is absorbed by
existing page padding; per DDR-1097 the size cannot discriminate binaries, the
hash can, and it moved.)

- **Green and non-vacuous** (read from the serial capture, DDR-1041, not from rc):
  `PRADYOS_KASAN_UAF_OK poison detected seen=0xDEADBEEFDEADBEEF` — the UAF read
  returned the exact `PMM_POISON` value.
- **Mutant discrimination on a distinct binary.** Dropping the fill loop (zero
  iterations, clean at `-Werror`) gives binary **`23495912fde0f335`**:
  - `smoke-kasan-uaf` **FAILS** — `KASAN_UAF_FAIL uaf read not poison
    seen=0x1111111122222222 live=0x1111111122222222` (the UAF read saw the stale
    live value);
  - `smoke-pmm-poison` **still PASSES** (rc=0) — the banner is untouched.
  That pair is §1's gap made concrete: the new gate catches exactly what the old
  one is blind to.
- **Revert is bit-for-bit.** Restoring the fill rebuilds to `af38fc632e726793`,
  byte-identical to the shipping binary.
- **Regression set green** on the shipping binary: `smoke-blkmq`,
  `smoke-rqstress-liveness`, `smoke-blk-integrity`, `smoke-shell`; plus
  `hygiene_check.sh` (all ten static checks) and `ci-shard-check`.

Gate registered on **shard 3** (`tools/ci/gate_shards.txt`), strict, beside
`smoke-pmm-poison`, 15 s budget.

## §5 NOT CLAIMED

- **This reproduces nothing and closes nothing.** It is not U1/U2/U3 (DDR-1155 §5
  — those are residual scheduler/freed-TCB signatures; this is a direct test of a
  mechanism that already works). OPEN-1/2/12/13 are untouched; no open issue moves.
- **No behaviour change.** The poison fill, the free path and the banner are all
  exactly as before; §3's fix is confined to the new selftest's own code.
- **Not KASAN shadow memory.** The gate name tracks the `KASAN` build flag that
  already guards the poison fill; this is a poison regression test, not a shadow
  allocator.
- **F2/F1/F3 are NOT landed here.** They remain PROPOSED (PR #27) awaiting explicit
  operator approval. This DDR is the one item PR #27 comment 5971011693 approved.
- No new sentinel in `GLOBAL_FORBIDDEN`; the gate uses a required `EXTRA_SENTINEL`,
  and the check is deterministic with its own gate asserting both directions, so it
  cannot hide in a green run (DDR-1065's reasoning, vs DDR-981/1049 intermittents).

# DDR-1076 — `fast_memset`, and the broadcast defect no gate in this tree can see

**Status:** ACCEPTED (design; implementation and proof in §5–§7)
**Date:** 2026-09-06
**Supersedes on one point:** DDR-1075 §4.4 — **corrected in §1 below.**

---

## 0. Summary

DDR-1075 §4 measured the asymmetry: `memcpy` routes to `fast_memcpy`
(ERMS / `REP MOVSQ`, DDR-871) while **`memset` is a byte-at-a-time C loop**
(`string.c:4-9`), paid on **every `kfree`**. This builds the missing half.

Two things are recorded here that DDR-1075 did not have right:

| § | |
|---|---|
| **1** | **DDR-1075 §4.4's trap is CORRECTED.** `rep stosb` is architectural — ERMS is a *performance* hint, not an enabling feature — so **both dispatch arms are correct on every x86_64 CPU** and "the default must be the generic path" is a claim about speed, not correctness. |
| **2** | **The real trap is the byte broadcast, and it is worse** — because it is invisible to **every gate in this tree** and to the kheap debug machinery itself. Measured, not asserted. |

---

## 1. CORRECTION to DDR-1075 §4.4

DDR-1075 §4.4 wrote:

> *"The dispatch variable's **default must be the safe generic path**, exactly
> as `fast_memcpy`'s is — and a mutant that reverses the default **would pass
> every gate**, because the CI CPU advertises ERMS, so the wrong default is
> never taken there."*

The second half is true and the first half is **wrong in kind**. Reading
`fast_memcpy.asm` in full settles it:

```asm
fast_memcpy:
    cmp qword [rel fast_memcpy_have_erms], 0
    je .fallback
    mov rcx, rdx
    rep movsb            ; <- architectural on every x86_64 CPU
    ret
.fallback:
    ...rep movsq + byte tail
```

**`rep movsb` / `rep stosb` are base x86_64 string instructions.** ERMS
(CPUID.7:EBX bit 9) advertises that the microcode implementation is *fast*;
it does not gate the instruction's existence or correctness. So a build that
took the ERMS arm on a pre-ERMS CPU would produce **the correct answer,
slowly** — it is not a fault, and there is no fault to hunt.

Stating it as a correctness trap was wrong in the direction that costs time:
it would have sent a future session looking for a fault that cannot occur, on
hardware CI does not run. The DDR-1074 shape (a diagnostic instruction that
convicts on a reading a correct system produces), one level down.

**What survives from §4.4:** the mutation observation. A mutant reversing the
default *is* undetectable by every gate — it is simply also harmless.

---

## 2. THE REAL TRAP, and it is measured

`memset(void *dst, int c, size_t n)` fills **bytes** with `(unsigned char)c`.
A `rep stosq` bulk moves **eight bytes per iteration from RAX**, so `c` must be
**broadcast to all eight lanes**:

```
RAX = c * 0x0101010101010101
```

Get that broadcast wrong — the classic error is `movzx rax, sil` and no
multiply, leaving `c` in the low byte and **zeros in the other seven** — and:

* **every zero fill is still correct**, because 0 broadcasts to 0 under any
  formula that maps 0 → 0;
* only a **non-zero** fill is wrong, and then only in the `rep stosq` bulk.

### 2.1 How much of this kernel is a zero fill? Measured, not assumed

```
$ grep -rn "memset(" kernel/ --include=*.c | wc -l
73
$ grep -rn "memset(" kernel/ --include=*.c | grep -v 'memset([^,]*, *0[,)]'
kernel/mm/kheap.c:174:   memset(ptr, POISON_FREE, c->obj_size);     /* 0xDD */
kernel/main.c:1349:      memset((void *)(uintptr_t)bp, 0xB6, 4096);
kernel/main.c:2911:      memset((void *)(uintptr_t)cbuf, 0x5A, 65536);
```

The first number is **grep hits, not call sites**: one of the 73 is
`string.c`'s own definition, so it is **72 call sites, three of them non-zero**
— and **not one of the three has its bytes verified anywhere**:

* **`kheap.c:174`** — `POISON_FREE` is `0xDD`, written on **every `kfree`**.
  `grep -rn POISON_FREE kernel/` returns exactly **two** lines: the `#define`
  and this write. **Nothing ever reads it or compares it.** The double-free
  detector checks `KHEAP_CANARY` at offset 8, which is written *after* the
  memset and would be correct regardless.
* **`main.c:1349`** — a 4 KiB `0xB6` buffer handed to `vfs_write`; the
  assertion is `== 4096`, a **return code**. The content is never read back.
* **`main.c:2911`** — a 64 KiB `0x5A` buffer written 40× in the SFS churn; the
  assertion is about the **B+tree and the allocator**, not the bytes.

`PMM_POISON` is not affected: `pmm.c:227` writes it with an explicit
`p[i] = PMM_POISON` store loop, not through `memset`.

### 2.2 What that means

**A broadcast defect would be invisible to all 177 gates, and invisible to the
kheap debug machinery whose poison it corrupts.** It would ship looking
correct, and the first symptom would be a use-after-free that the poison was
supposed to make obvious failing to look obvious.

So the gate arm with a **non-zero fill is load-bearing**, and this is the
reason — measured in advance, in the DDR-1039 / DDR-1058 / DDR-1068 discipline
of checking an arm can fail *before* writing it. The zero-fill arms are the
decorative ones here, and they are kept only because `n`-boundary bugs (§3.3)
are orthogonal to the broadcast and need their own coverage.

---

## 3. Design

### 3.1 One probe, two consumers — the flag is SHARED, not duplicated

`fast_memset` reads **`fast_memcpy_have_erms`**, the flag DDR-871's
`fast_memcpy_init()` already sets from CPUID.7:EBX bit 9 (`main.c:3721`).
No second probe, no second flag, no second init call to forget.

That is DDR-1037's `fd_ready_mask` reasoning applied to a feature bit: two
consumers of one predicate cannot drift, and a duplicated CPUID probe is a
second thing to keep in step for no gain. The cost is that
`fast_memset.asm` depends on a symbol defined in `fast_memcpy.asm`; both are
in `KERNEL_ASMS`, so the link order does not arise.

**Pre-init behaviour is therefore already decided and is correct:** the flag
lives in `.data` initialised to 0, so any `memset` before `main.c:3721` takes
the `REP MOVSQ`-shaped fallback — which is correct everywhere (§1), and is
still vastly better than the byte loop it replaces.

### 3.2 The broadcast, and why AL falls out for free

```asm
    mov   r8, rdi                      ; save dst for the return value
    movzx rax, sil                     ; RAX = c & 0xFF
    mov   r9, 0x0101010101010101
    imul  rax, r9                      ; RAX = c replicated into all 8 lanes
```

`rep stosq` consumes **RAX** (all eight lanes) and `rep stosb` consumes
**AL** — and AL is the low byte of the broadcast, which is `c`. So **one
register serves both paths and the byte tail**, with no reload. Worth stating
because the obvious alternative (broadcast only on the fallback path) leaves
the tail `rep stosb` reading whatever RAX held.

`DF` is clear on entry by the SysV ABI, so no `CLD` — the same reasoning
`fast_memcpy.asm` records, and for the same reason (a `CLD` costs a pipeline
flush on some microarchitectures for a condition already promised).

### 3.3 COST (DDR-870 item 44 convention — static figures are hardware-true)

| | |
|---|---|
| broadcast | 3 instructions |
| dispatch | 2 instructions + 1 branch |
| ERMS path | 2 after dispatch + return, 0 stack traffic |
| fallback path | 5 after dispatch + return, 0 stack traffic |
| locks / serialising instructions / allocation | none |

Against the byte loop it replaces, which is **~4 instructions per byte**
(load, store, increment, compare-and-branch): for the 4096 B slab-growth
zero that is ~16,000 retired instructions against **~10 plus one string
operation**. That is the hardware-true claim, and it is a *static instruction
count*, not a speedup — per DDR-1075 §1, a speedup figure is not producible in
this environment and must not be quoted.

**§NON-NEGOTIABLE 17's denominator:** total = the static counts above;
per-event = per `memset` call, independent of `n` except for the microcoded
string operation itself.

---

## 4. The gate

**NO NEW GATE.** The arms go on **`smoke-bench`** (`Makefile:990`, shard 8,
**strict**) — the gate that already exists for exactly this subject, DDR-870's
asm-path benchmark. This is the DDR-1039 reasoning (`smoke-readline` should not
exist; the arm belongs on `smoke-shell`) and DDR-1067's and DDR-1070's after it.

`memset_selftest()` runs unconditionally beside the other boot self-tests
(`main.c:3757-3764`), **before `smp_start_aps()` at :3824**, so it is
single-CPU and needs no locking.

Arms, each verifying **every byte** of the region plus a **guard byte on each
side** (so an over-run or under-run is caught, not just a wrong value):

| n | why |
|---|---|
| 0 | must write **nothing** — a `rep` with RCX=0 is a no-op, and a length bug here is a one-byte clobber |
| 1, 7 | shorter than one 8-byte word: the fallback's bulk count is 0 and the whole fill is the tail |
| 8 | exactly one word, tail count 0 |
| 9 | one word + a 1-byte tail — the case a dropped tail gets wrong |
| 4095 | 511 words + a 7-byte tail, the maximum tail |
| 4096 | the real page-zero size, tail count 0 |

Each `n` is run with **fill `0x00` and fill `0xA7`**. The non-zero fill is the
arm §2 exists for; `0xA7` is chosen distinct from `POISON_FREE` (`0xDD`),
`0xB6` and `0x5A` so a stray match in a log cannot be confused with one of the
three shipped fills.

Sentinel: `PRADYOS_MEMSET_OK cases=<N>` where **N is reported by the probe**,
not written as a literal in the Makefile — the DDR-1054 rule, so a future
edit that silently drops arms cannot leave the gate green.
`PRADYOS_MEMSET_FAIL` is a **FORBIDDEN_SENTINEL**, so a failure names itself
rather than being an absence (DDR-1066's discipline).

---

## 5. Mutation plan — the mutants must fail DIFFERENT arms

| | mutation | must fail | must NOT fail |
|---|---|---|---|
| **M1** | drop the `imul` (broadcast becomes `c` in the low byte, zeros above) | the **`0xA7`** arms at every `n >= 8` | every `0x00` arm — **this is the point**: M1 is the defect §2 says no existing gate can see, and its passing the zero arms is the proof that the non-zero arm is load-bearing |
| **M2** | drop the fallback's byte tail (`and rcx,7` / `rep stosb`) | `n = 1, 7, 9, 4095` | `n = 0, 8, 4096` |

If M1 and M2 fail the same arms, the arms are not independent and the design
is wrong — the DDR-1044 M2/M3 lesson, where a mutant landing on an already-
covered arm left the intended arm unproven.

**M1 must be run on the ERMS-taken path too.** The CI CPU advertises ERMS, so
the shipped path is `rep stosb`, which uses AL only and is **immune to a
broadcast bug** — meaning M1 would pass on the default model. The gate
therefore must exercise the **fallback** as well, and the only honest way to
do that is a second boot with the flag forced clear. Rather than add a debug
knob (an opt-in instrument is OFF where it matters — DDR-1010/DDR-1043), the
self-test **calls the fallback path directly by clearing and restoring the
shared flag around a second pass**, so both paths are covered in one boot on
one CPU model. This is stated as a design consequence, not discovered later.

---

## 6. Not claimed

* **No performance measurement.** §3.3 is a static instruction count; no
  speedup figure is produced or quoted (DDR-1075 §1).
* **No defect is fixed.** `memset` was always *correct*; it was slow. Nothing
  in §2 reports a present bug — §2 describes what a **future** broadcast
  defect would do, and why the gate must be able to see it.
* **`memmove` is deliberately untouched**, for DDR-871's stated reason: `rep`
  string ops copy strictly forward, so an overlapping backward move would
  corrupt the tail. `memset` has no overlap question.
* **`memcmp`, `strlen`, `strncmp` are untouched** — out of scope, and none
  sits on a per-`kfree` path.
* No open issue moves. OPEN-1 / OPEN-2 / OPEN-12 / OPEN-13 untouched.

---

## 5. What shipped

* **`arch/x86_64/fast_memset.asm`** — the dispatch above, reading
  `fast_memcpy_have_erms`. Added to `KERNEL_ASMS` and to the link line.
* **`kernel/string.c`** — `memset` now `return fast_memset(dst, c, n);`. The
  byte loop is gone. `memmove`, `memcmp`, `strlen`, `strncmp` untouched.
* **`kernel/main.c`** — `memset_selftest()`, called beside the other boot
  self-tests, immediately after `sharedpte_selftest()`.
* **`Makefile`** — `smoke-bench` gains the two arms.

**One design change was made after the code was written and before it was
built**, and it is recorded rather than quietly fixed: the first version had
pass 0 use whatever the CPU reported and pass 1 force the fallback. That is
wrong here for a reason that has nothing to do with the CPU —
**`memset_selftest()` runs at the `:375x` self-test block and
`fast_memcpy_init()` is at `:381x`**, so the flag is still `0` at that point
and *both* passes would have taken the fallback. Both values are now forced
explicitly, which also removes the design's dependence on the CPU model.

## 6. Proof

**Baseline (pre-fix tree):** `2c4868b2f5f0d00a`, 1,290,634 B.
**Fixed:** `fd913d083446b0d1`, **1,290,634 B — size unchanged.** The additions
fit inside the image's existing page padding, so `CLAUDE.md`'s
size/headroom pair (282,230 B) is **untouched** and `ci-docstate-check` is
unaffected. Verified by rebuild, not assumed.

**Non-vacuity checked by reading the capture back, not inferred from rc=0**
(DDR-1041): `PRADYOS_MEMSET_OK cases=28` appears **exactly once** in
`smoke-bench`'s own log, between `PRADYOS_SHAREDPTE_OK` and the
`[fwcfg] probes="bench"` line — i.e. in the boot self-test block where it was
placed, not somewhere a different gate wrote it.

| | mutation | kernel | result |
|---|---|---|---|
| **M1** | `imul` dropped — broadcast leaves `c` in the low byte, zeros above | `c3e8244cae53fa29` | rc=2, `PRADYOS_MEMSET_FAIL pass=1 n=8 off=2 got=0 want=167` |
| **M2** | fallback byte tail dropped | `99142d089c923e61` | rc=2, `PRADYOS_MEMSET_FAIL pass=1 n=1 off=1 got=60 want=0` |

Reverting either returns `fd913d083446b0d1` **bit-for-bit**.

### 6.1 What M1 proves, and it is the whole argument

M1 is the defect §2 says no existing gate can see. **It failed only at
`n=8`, `fill=0xA7`, in `pass=1`** — meaning it had already **passed**:

* the entire **`pass=0`** sweep (all 14 cases), because that pass is
  `rep stosb`, which consumes AL and is **immune to a broadcast bug**. Without
  the forced two-pass design this mutant would have gone undetected on the CI
  CPU. The second pass is load-bearing, not belt-and-braces;
* `n = 0, 1, 7` in pass 1, because below 8 bytes the fallback's bulk count is
  zero and the whole fill comes from the tail's `rep stosb` — AL again;
* **`n = 8` with `fill = 0x00`** — the zero fill, which is correct under the
  broken broadcast. **That single passing case is the measurement**: it is
  §2's claim reproduced, that a broadcast defect is invisible to every
  zero-fill site, and therefore to 70 of this kernel's 73 memset call sites
  and to all three that are non-zero but unverified.

`off=2 got=0` is the little-endian signature of `RAX = 0x00000000000000A7`:
byte 0 correct, every byte above it zero. **The failure names its own
mechanism** rather than merely reporting a mismatch.

### 6.2 The mutants land on different arms

M1 fails a **non-zero** fill at `n=8` with a **wrong value**; M2 fails a
**zero** fill at `n=1` with **nothing written** (`got=60` is the `0x3C`
background). Different length, different fill, different failure mode — so
neither mutant is carrying the other, which is the check DDR-1044's M2/M3
lesson exists for. In particular M2 is caught by a *zero*-fill arm, so the two
arms are independently live.

### 6.3 One property that arrived as a side effect, stated as such

The guard byte places the region at `&g_ms_buf[1]`, an **odd address**, so
`rep stosq` runs on an unaligned destination throughout. That is
architecturally fine and means the arms cover an unaligned fill — but it is a
**consequence of the guard**, not something the design set out to test, and it
is recorded that way rather than claimed as coverage that was reasoned about
in advance.

## 7. Regression

`memset` is on every allocation path in the kernel, so the set is broader than
a narrow change would need:

| gate | rc |
|---|---|
| `smoke-shell` | **0** — full PASS line, `PRISM_READY … pipes … erase … quoting … wait, clean, no panic` (§NON-NEGOTIABLE 4) |
| `smoke-blkmq` | **0** |
| `smoke-rqstress-liveness` | **0** |
| `smoke-blk-integrity` | **0** |
| `smoke-bench` | **0**, with the capture read back (§6) |

`kernel_after == fd913d083446b0d1` after the run — the binary the gates
tested is the binary that was built, so none of them rebuilt a different one
(DDR-1035's discipline).

Hygiene **ALL SEVEN**. `GLOBAL_FORBIDDEN` **76, deliberately unchanged** —
§4's reasoning. `make image` rc=0, zero warnings at `-Werror`.

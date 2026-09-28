# DDR-1121 — THE SHARD-7 FREEZE HAS TWO WITNESSES AND THEY NAME DIFFERENT LOCKS

**Assessment + correction of my own DDR-1120 §6, one commit ago. Docs-only: no
code change, no gate, no new sentinel, `kernel.bin` NOT rebuilt.**

**NO FIX, NO MECHANISM NAMED, OPEN-2 DOES NOT CLOSE (§NON-NEGOTIABLE 3).**

---

## §0 — ARTEFACT AND PROVENANCE

CI **34766468421**, the `pull_request` suite on `5f3ac9e`, **shard 7
`smoke-smpsched`** — the *second* red on that head, which DDR-1120 §6 recorded
as a different site from the shard-5 `[schedcheck]` fire and deliberately did
not pool with it. That separation stands and is reinforced here.

**§INV.18 satisfied without a rebuild, and the check is stated rather than
assumed.** The local `build/kernel.elf` re-derives under `llvm-objcopy -O
binary` to **`6af029b001e6e6db`**, byte-for-byte the on-disk `build/kernel.bin`
at 1,319,306 B; and `git diff --name-only 5f3ac9e HEAD -- Makefile '*.c' '*.h'
'*.asm' '*.S' '*.ld'` returns **ZERO files**. So every address below is resolved
against the exact binary that produced the capture. The shard's own post-gate
step printed `kernel.bin: OK`.

---

## §1 — THE FINDING THAT MADE THIS DDR NECESSARY: THE DUMP WAS ALREADY THERE

DDR-1120 §6 closed on this sentence:

> *"it is precisely the case DDR-1060 built `waiters=` for and the lock dump is
> ordered after the `[apfreeze]` line **so the next occurrence should carry
> it**."*

**It was already in this occurrence.** The capture carries a complete
`PRADYOS_LOCKSTAT` block, thirteen locks, emitted immediately after `shot=1`:

```
PRADYOS_LOCKSTAT overflow=0
PRADYOS_LOCKSTAT lock=0xFFFFFFFF8014DC60 hits=1   waitavg=191394 waitmax=191394  waiters=0
PRADYOS_LOCKSTAT lock=0xFFFFFFFF80158894 hits=967 waitavg=6302   waitmax=446782  waiters=0
PRADYOS_LOCKSTAT lock=0xFFFFFFFF8014DC90 hits=20  waitavg=9245   waitmax=87245   waiters=0
PRADYOS_LOCKSTAT lock=0xFFFFFFFF8014DCC0 hits=41  waitavg=555    waitmax=1691    waiters=0
PRADYOS_LOCKSTAT lock=0xFFFFFFFF8014DCD8 hits=54  waitavg=909    waitmax=13475   waiters=0
PRADYOS_LOCKSTAT lock=0xFFFFFFFF80147FE1 hits=25  waitavg=391128 waitmax=1025301 waiters=0
PRADYOS_LOCKSTAT lock=0xFFFFFFFF80147210 hits=1   waitavg=272489 waitmax=272489  waiters=0
PRADYOS_LOCKSTAT lock=0xFFFFFFFF80159F08 hits=3   waitavg=127539 waitmax=339619  waiters=0
PRADYOS_LOCKSTAT lock=0xFFFFFFFF80147D38 hits=1   waitavg=241913 waitmax=241913  waiters=0
PRADYOS_LOCKSTAT lock=0xFFFFFFFF8015A328 hits=160 waitavg=25270  waitmax=115885  waiters=0
PRADYOS_LOCKSTAT lock=0xFFFFFFFF8014DCA8 hits=73  waitavg=1607   waitmax=17028   waiters=1
PRADYOS_LOCKSTAT lock=0xFFFFFFFF8015A748 hits=348 waitavg=29479  waitmax=934626  waiters=0
PRADYOS_LOCKSTAT lock=0xFFFFFFFF8014C690 hits=5   waitavg=14616  waitmax=35868   waiters=0
```

**`overflow=0`, so the dump is COMPLETE** — no CPU failed to claim a slot, and
no contended lock is missing from the list. **Exactly one lock of thirteen has a
waiter.**

The shape of the mistake is worth naming because it is not forgetfulness: the
DDR-1120 analysis worked from the `[apfreeze]` line and its backtrace, which is
what the forbidden-pattern scan puts under *"matching lines"* and *"40 lines of
context before the first match"*. **The dump is neither** — it sits in the
replayed capture body, above the summary the scan prints. So the instrument's
output was present, and the reading habit that found the `[apfreeze]` line did
not reach it. **DDR-1088's class, one level in:** that DDR established the panic
report had never reached a CI job log because the scan showed *leading* context
for the *first* pattern; here the report DID reach the log and the reader
stopped at the same summary block.

---

## §2 — WHAT THE ONE WAITER RESOLVES TO

`struct rq` is declared at `kernel/proc/sched.c`:

```c
struct rq { spinlock_t lock; struct tcb *head, *tail; };
```

`spinlock_t` is one byte, so with alignment `sizeof(struct rq) == 24 == 0x18`,
and `static struct rq g_rq[PERCPU_MAX]` puts `g_rq[n].lock` at `g_rq + 0x18n`.
The dump carries `g_rq+0x0`, `+0x18`, `+0x30`, `+0x48` — **four runqueue locks
at `-smp 4`**, which is the arithmetic confirming the stride rather than an
assumption about it.

**`g_rq` IS AMBIGUOUS IN THIS BINARY AND WAS DISAMBIGUATED, NOT ASSUMED:**
`llvm-nm` reports **two** defined symbols named `g_rq` — `0xffffffff80165fb0`
(`virtio_rng.c`'s `static struct virtq g_rq`) and `0xffffffff8014dc90`
(`sched.c`'s array). Both are `static`, so both appear under the same name. The
address in the dump belongs to the **scheduler's**.

| dumped address | symbol | what it is |
|---|---|---|
| `0xFFFFFFFF8014DC60` | `g_heap_lock+0x0` | kheap |
| `0xFFFFFFFF80158894` | `g_sched_lock+0x0` | the global scheduler lock |
| `0xFFFFFFFF8014DC90` | `g_rq+0x0` | **runqueue 0** |
| `0xFFFFFFFF8014DCA8` | `g_rq+0x18` | **runqueue 1 — THE ONLY WAITER** |
| `0xFFFFFFFF8014DCC0` | `g_rq+0x30` | runqueue 2 |
| `0xFFFFFFFF8014DCD8` | `g_rq+0x48` | runqueue 3 |
| `0xFFFFFFFF80147FE1` | `g_console_lock+0x0` | console |
| `0xFFFFFFFF8014C690` | `g_pmm_lock+0x0` | PMM |
| `0xFFFFFFFF80159F08` | `g_inst+0x408` | **vblk unit 0 `compl_lock`** |
| `0xFFFFFFFF8015A328` | `g_inst+0x828` | **vblk unit 1 `compl_lock`** |
| `0xFFFFFFFF8015A748` | `g_inst+0xC48` | **vblk unit 2 `compl_lock`** |
| `0xFFFFFFFF80147210` | `demo_ep+0x38` | IPC demo endpoint |
| `0xFFFFFFFF80147D38` | `sub_a+0x220` | bcast subscriber |

`g_inst = 0xffffffff80159b00`, `sizeof(struct vblk) == 0x420`, and `compl_lock`
sits at `+0x408` within it — so `+0x408`, `+0x828`, `+0xC48` are units 0, 1 and
2. **All three read `waiters=0`, including unit 2**, which is the unit named in
every one of this capture's `[vblk] compl wait timeout unit=2` lines.

---

## §3 — THE FROZEN CPU MUST HAVE A LIVE `waiters` INCREMENT, DISASSEMBLED

```
[apfreeze] cpu=2 ticks=162 rip=0xFFFFFFFF80049A52 if=0 rsp=0x07CB7BA0 pid=22 shot=1
           bt=0xFFFFFFFF80021405,0xFFFFFFFF8002139C,0xFFFFFFFF8002149E,0xFFFFFFFF80021094
```

`rip` resolves to **`spin_lock_contended+0x92`**. Disassembled in this binary:

```
spin_lock_contended+0x1F:  callq <ls_slot_for>          ; slot -> -0x18(%rbp)
spin_lock_contended+0x28:  cmpq $0x0, -0x18(%rbp)       ; no slot -> g_ls_overflow++
spin_lock_contended+0x5B:  lock xaddq %rax, 0x20(%rdx)  ; *** waiters++ ***
spin_lock_contended+0x77:  callq <ls_rdtsc>             ; t0
spin_lock_contended+0x80:  xchgb %al, (%rcx)            ; \
spin_lock_contended+0x88:  cmpb  $0x0, %al              ;  |  THE SPIN LOOP
spin_lock_contended+0x8A:  je    +0x97                  ;  |
spin_lock_contended+0x90:  pause                        ;  |
spin_lock_contended+0x92:  jmp   +0x80                  ; /  *** THE FREEZE RIP ***
spin_lock_contended+0x97:  callq <ls_rdtsc>             ; acquired
spin_lock_contended+0xCB:  negq / lock xaddq 0x20(%rdx) ; *** waiters-- ***
```

**The freeze RIP is the `jmp` that closes the spin loop** — after the increment
at `+0x5B` and before the decrement at `+0xCB`. `waiters` is `0x20` into
`struct ls_slot`, which matches the declared field order (`key, hits,
wait_total, wait_max, waiters`).

So the frozen CPU **has** an outstanding `waiters++` on whatever lock it is
spinning on, and it cannot have failed to claim a slot because `overflow=0`.

`lock_stat.c`'s own comment states the same contract in advance:

> *"A frozen CPU leaves a permanent +1 here, **on exactly the lock it is stuck
> on**, and that is the answer DDR-1006 §7 asked for."*

**And the keying is exact, checked not assumed:** `ls_slot_for` keys on
`(uint64_t)(uintptr_t)l` — the lock address **verbatim** — linear-probes, and on
a lost CAS re-checks the same slot for the same key before moving on. There is
no hash and therefore no aliasing; a slot cannot be attributed to the wrong
lock.

**By elimination, the `+1` on `g_rq[1].lock` is the frozen CPU's.**

---

## §4 — EXCEPT THAT THE BACKTRACE SAYS OTHERWISE, ALSO INSTRUCTION-EXACT

| frame | address | symbol |
|---|---|---|
| `bt[0]` | `0xFFFFFFFF80021405` | `spin_lock+0x25` |
| `bt[1]` | `0xFFFFFFFF8002139C` | `spin_lock_irqsave+0x1C` |
| `bt[2]` | `0xFFFFFFFF8002149E` | `submit+0x2E` |
| `bt[3]` | `0xFFFFFFFF80021094` | `vblk_read+0x34` |

A coherent four-frame chain landing on real call sites — not the shape a garbage
frame-pointer walk produces. And `submit+0x2E` is not merely *near* the lock
acquisition, it is **the return address of it**, with the lock pointer visible
two instructions earlier:

```
submit+0x1E:  movq  -0x10(%rbp), %rdi     ; rdi = v
submit+0x22:  addq  $0x408, %rdi          ; rdi = &v->compl_lock   <-- offset 0x408
submit+0x29:  callq <spin_lock_irqsave>
submit+0x2E:  movq  %rax, -0x30(%rbp)     ; <-- bt[2] is exactly here
```

**The `$0x408` is in the instruction stream.** The backtrace says, with no
inference at all, that the frozen CPU is blocked on a **`compl_lock`** — and
every `compl_lock` in the dump reads `waiters=0`.

---

## §5 — THE CONTRADICTION, STATED AS ONE RATHER THAN RESOLVED BY PREFERENCE

Four propositions, each measured in this binary against this capture:

1. The freeze RIP is inside `spin_lock_contended`'s spin loop, **after**
   `waiters++` and **before** `waiters--`.
2. `overflow=0`, so the frozen CPU claimed a slot.
3. `ls_slot_for` keys on the lock address verbatim, so its slot is that lock's.
4. `bt[2] = submit+0x2E` is the return of `spin_lock_irqsave(&v->compl_lock)`,
   with the `+0x408` offset visible in the disassembly.

(1)+(2)+(3) require some `compl_lock` to read `waiters>=1`. (4) names which one.
**The dump says all three read `waiters=0`, and the single waiter in the entire
system is a runqueue lock that `submit` does not take.**

These cannot all hold. **This capture cannot say which one fails, and no
mechanism is named here.** What is established is narrower and is the whole
deliverable: **the shard-7 freeze is no longer "a lock wait, unexplained" — it
is a lock wait whose two independent witnesses name different locks**, with both
witness chains verified instruction-exact rather than argued.

### §5.1 — Readings considered and NOT chosen

* **"The `+1` is an unrelated transient waiter on `g_rq[1]`."** Possible on its
  own — a brief legitimate wait caught by the snapshot — **but it does not
  rescue the picture**, because the frozen CPU's own `+1` would then have to
  appear somewhere as well, and it appears nowhere.
* **"The backtrace is stale."** `spin_lock_contended` establishes `rbp`
  (`push rbp; mov rsp,rbp`), the walk is taken at NMI time with the RIP, and the
  chain is coherent across four frames. Nothing supports calling it stale beyond
  the fact that it is inconvenient, which is not evidence.
* **"`waiters` is relaxed, so the store may not be visible."** The increment is a
  `lock xaddq`, a real atomic RMW, and on x86-64 TSO the value is visible to the
  reader. Refused.

**A matching shape is not a mechanism (DDR-1056), and a contradiction is not a
defect report.** Which instrument is wrong is the open question this records.

---

## §6 — WHAT THE NEXT OCCURRENCE NEEDS, RECORDED AND NOT BUILT

The dump is **per-lock and not per-CPU**, and DDR-1060 §4 refused per-CPU for a
measured reason that still holds: a per-CPU `waiting_on` needs to know which CPU
is executing, and both routes are documented hazards — `this_cpu()` reads
`%gs:0` and DDR-1010 caught a broken SWAPGS discipline as one of OPEN-2's own
producers, while `lapic_id()` is invalid pre-LAPIC (DDR-1055).

**That refusal is not revisited here**, and the reason is this DDR's own finding:
a per-CPU field would tell us *who* waits, and the disagreement above is about
*which lock* — so it would not have resolved this capture. What would is a
**`key=` echo beside the `waiters` count on the frozen CPU's own slot**, i.e.
the lock address the frozen CPU believes it claimed, read back from its slot. If
that echoes `g_inst+0xC48` while the table shows the `+1` on `g_rq+0x18`, the
table's attribution is wrong; if it echoes `g_rq+0x18`, the backtrace is.

**NOT BUILT.** It is a change to the hottest primitive in the kernel on the very
path OPEN-2 lives in — the cost DDR-1047 refused and DDR-1060 respected — and it
would be designed off **one** occurrence of this site. It needs its own DDR, its
cost measured, and a forced mutant.

---

## §7 — NOT CLAIMED

* **NO FIX. NO MECHANISM. OPEN-2 DOES NOT CLOSE**; no open issue moves
  (OPEN-1/2/12/13 untouched).
* **NO DEFECT IS ALLEGED IN `lock_stat.c`.** `ls_slot_for`, the increment
  ordering, the decrement placement and the overflow counter were all read and
  are all correct for what they were built to do. §5 records that *something*
  must be wrong; it does not say it is this file.
* **NO DEFECT IS ALLEGED IN `virtio_blk.c`** — `submit`'s locking is the
  documented locks-4 pattern and nothing here disputes it.
* **NO RATE.** One occurrence of this site, ever.
* **NOT POOLED with the `[schedcheck]` family.** DDR-1120 §6's separation stands
  and is strengthened: that is a halt loop in `schedule_locked`, this is a spin
  in `spin_lock_contended`, different RIPs, different backtraces, different
  instruments.
* **NOT ATTRIBUTED to `5f3ac9e` and NOT EXONERATED either** (DDR-1042).
* **DDR-1120 IS NOT WITHDRAWN.** Its §1–§5 stand untouched; §6's *reading* of the
  shard-7 red as "a lock wait" is **confirmed** by the RIP. What is corrected is
  one clause — that the dump would come *next* time — and the correction is made
  in place rather than by deletion, so the record shows what was believed.
* **DDR-1060 IS NOT CRITICISED.** Its instrument is what produced this result;
  `waiters=` did exactly what it was built to do and printed a number nothing
  else in the system could have printed.
* **NO code change, NO gate run, `kernel.bin` NOT rebuilt** — so the
  size/headroom pair and `ci-docstate-check` are unaffected; `GLOBAL_FORBIDDEN`
  77, 179 gates, 79 probe ELFs, no new sentinel and no new clause, so **the set
  of frames that fire is unchanged**.
* **A TOOLING NOTE, recorded because it has now cost time twice:** resolving
  these addresses with `awk` failed — `strtonum()` is a **gawk** extension and
  this host's `/usr/bin/awk` is **mawk**. That is exactly the defect DDR-1079
  fixed in `tools/ci/sym_at.sh` by rewriting it in `python3`. The same trap is
  waiting in any ad-hoc `awk` one-liner written to resolve a RIP. Use `python3`.

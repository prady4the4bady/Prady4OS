# DDR-1129 — DDR-1128'S `[apfreeze]` RIP RESOLVES TO A **DELIBERATE HALT**: THE CPU DETECTED A CORRUPT `next->rsp` AND STOPPED ON PURPOSE

**Status:** addresses resolved; **DDR-1128 §3's reading CORRECTED AT THE SITE**
(DDR-1110's rule). Docs-only in this commit — no code change, no gate,
`kernel.bin` not rebuilt for shipping. **NO FIX, NO MECHANISM NAMED, OPEN-2 DOES
NOT CLOSE**, no open issue moves.

---

## 1. §INV.18 IS SATISFIED — TWO INDEPENDENT WAYS, NOT ONE

§INV.18 requires resolving against **the binary that produced the capture**.
DDR-1128 §6 recorded this as owed and blocked (no toolchain). The blocker was
`nasm` alone; it is now installed, and the build reproduces **bit-for-bit**:

| build | expected | measured | source of the expectation |
|---|---|---|---|
| baseline `make image` | `25f4dae4a3f90bcb` | **`25f4dae4a3f90bcb`** | DDR-1126's own commit message |
| `touch kernel/proc/sched.c && make image OPEN2_HUNT=32` | `ca8107ec7f5d8de7` | **`ca8107ec7f5d8de7`** | the campaign's `kernel_pinned=` |

The baseline row is not decoration: it is what makes the toolchain's fidelity a
**measurement** rather than an assumption, before the hunt hash is trusted. Both
are 1,319,306 B — **the size cannot discriminate them** (DDR-1097), only the
hash, and both hashes landed.

Independently, the job log states its own provenance:
`[job] built with OPEN2_HUNT=32 lane=0 ref_requested=dev/phase1-seyp3n
tree_sha=a390eabbf99ad08ac70469befce74ccd7e348172` and
`[campaign] kernel_pinned=ca8107ec7f5d8de7`. The two agree.

Resolved with `python3`, never `awk` (DDR-1079, re-paid by DDR-1121).

---

## 2. THE FIVE ADDRESSES

| field | address | resolves to |
|---|---|---|
| `rip` | `0xFFFFFFFF80016D91` | **`schedule_locked + 0x671`** |
| `bt[0]` | `0xFFFFFFFF80013C01` | `schedule + 0x11` |
| `bt[1]` | `0xFFFFFFFF80013B48` | `sched_ap_enter + 0x178` |
| `bt[2]` | `0xFFFFFFFF800495FA` | `smp_ap_entry + 0x30A` |
| `bt[3]` | `0x0000000007FA9023` | **below the first text symbol — NOT a code address** |

`bt[3]` is left unresolved deliberately. It sits near `rsp=0x07FA7938`, so it is
stack data, exactly as DDR-1128 §6 step 3 predicted; forcing it onto a symbol
would manufacture a frame that does not exist.

---

## 3. THE RIP IS A `hlt` LOOP. THE CPU STOPPED ON PURPOSE.

```
ffffffff80016d84:  lea    -0x170(%rbp),%rdi
ffffffff80016d8b:  call   ffffffff8000b660 <kline_emit>
ffffffff80016d90:  f4                        hlt
ffffffff80016d91:  e9 fa ff ff ff            jmp  ffffffff80016d90   <-- THE FROZEN RIP
```

That is `kernel/proc/sched.c:1857-1858`, `for (;;) __asm__ volatile("hlt");`,
reached **only** from inside `if (bad)` at `:1723`, where

```c
int outside = (nrsp < nbase) || (nrsp + 64u > nbase + STACK_SIZE);   /* :1688 */
int bad     = (nrsp & 7u) || outside;                                /* :1689 */
```

This is **DDR-1105's `next->rsp` validity check**, extended by DDR-1115 and
DDR-1118. CPU 3 was about to `context_switch` into a thread whose saved stack
pointer failed bounds or alignment, refused to load it, printed a diagnostic,
and halted itself.

**The pinned RIP across all four shots is therefore not "spinning" in any
scheduler sense.** It is a two-instruction halt loop. DDR-1128 §3 read the
pinning as *spinning rather than running-but-masked*; that dichotomy was the
wrong pair — the third possibility, **stopped on purpose**, is what happened,
and it is the one the address settles.

---

## 4. THE CORRECTION TO DDR-1128 §3

DDR-1128 §3 recorded `if=0`, `masked=0`, `svr=0x1FF`, `swen=1`, `isr48=0`,
`irr48=1` as *"the field-for-field shape DDR-981 recorded, stated as a shape and
nothing more."* It was careful to call the site unresolved and to claim nothing
from the shape. **It was still pointing the wrong way, and the address says so.**

* **`if=0` is not DDR-981's mechanism here.** DDR-981 is `RFLAGS.IF` cleared by
  `SYSCALL` entry and carried across a context switch, so a yield-spinning ring-3
  thread never takes another interrupt. That cannot apply: this CPU is on the
  **AP bring-up path** (`smp_ap_entry → sched_ap_enter → schedule →
  schedule_locked`), not returning from a ring-3 syscall, and it is in a
  deliberate `hlt` loop rather than a yield spin.
* **`irr48=1` (timer pending, undelivered) is a consequence, not a clue.** A
  halted CPU with interrupts off accumulates a pending timer by construction.
  The same three fields appear whenever any CPU halts deliberately, which is why
  they matched DDR-981's shape without meaning it.
* **This is the DDR-1019 class**, not the DDR-981 class: an `[apfreeze]` that is
  the *symptom of a prior deliberate halt*. DDR-1019 wrote the rule this DDR is
  an instance of — *"`[apfreeze]` has at least three distinct producers … resolve
  the RIP against its own binary before matching on the sentinel name; the offset
  differs per binary."* DDR-1019's own halt was `idt.c:697`, the losing branch of
  DDR-979's panic latch. **This is a fourth producer: `sched.c:1857`.**

**What DDR-1128 got right and is NOT withdrawn:** the census reading (exactly one
frozen CPU, `cpu=3` at `ticks=155` while 0/1/2 climbed) is unaffected — a
deliberately halted CPU is still a frozen CPU, and DDR-1123's per-CPU census
still fired for real for the first time. Its `NO-CHURN`-is-a-consequence
reading, its refusal to pool with DDR-1127, and its DDR-1126 statistics are all
untouched. **DDR-1127 is untouched in full.**

---

## 5. THE MECHANISM IS NAMED BY A LINE THE HUNT DID NOT PRINT

The halt at `:1857` is immediately preceded by `kline_emit` of a line that
begins, at `:1732`:

```
[schedcheck] next->rsp invalid tid=… pid=… rsp=… base=… rflags=… r15=… ret=…
             rq_on=… disp=… saves=… halting.
```

That line carries **exactly the fields DDR-1118 derived for the double-resume
question** — `rq_on` (1 means the thread is *also* sitting in a runqueue while
this CPU resumes it), and `disp`/`saves` (where `disp == saves + 2` is a thread
switched in twice with no save between). It is the diagnosis. It sits **one line
above** the first `[apfreeze]`.

**The campaign did not print it.** `tools/ci/open2_hunt_campaign.sh:35`:

```sh
SIGNALS='\[ringwalk\]|\[apfreeze\]|panic_stage=|NEXUS KERNEL PANIC|gs FAIL'
```

`[schedcheck]` is **not in that set**, and `:71` caps the printed hits at
`head -5`. So the job log shows four copies of the symptom and zero copies of
the diagnosis. This is the hunt reporting the freeze and dropping the reason for
it — and `[schedcheck]` has been in `GLOBAL_FORBIDDEN` since DDR-1105, so the
project already treats it as load-bearing everywhere except here.

**The scheduled check-in asked, first of four things, to check for
`[schedcheck]`.** It is the right question, and the job log cannot answer it.

---

## 6. WHAT THIS DOES AND DOES NOT SAY ABOUT OPEN-2

**Says:** DDR-1105's guard **worked**. A corrupt `next->rsp` was caught *before*
`context_switch` loaded it, which is the difference between a contained halt
with a printed diagnostic and an unbounded jump to a garbage stack. This capture
is therefore evidence of **TCB or stack-pointer corruption** on the AP path, and
the `[schedcheck]` fields are built to discriminate its cause.

**Does not say:** that this is OPEN-2's root cause, or that OPEN-2 is one defect.
**One occurrence.** The field *values* are unread — `rsp`, `base`, `rq_on`,
`disp`, `saves` are all still in the artifact, and without them "corrupt
`next->rsp`" names a detector's verdict, not a mechanism. §NON-NEGOTIABLE 3
forbids a fix on this.

Nothing here changes DDR-1128 §5's arithmetic on DDR-1126: p = 0.143, the
1-in-10 95% upper bound of 39.4% still overlaps DDR-1127's `<4.87%`. **Not a
regression, not an exoneration** (DDR-1042).

---

## 7. STILL OWED

1. **The artifact** — `open2-hunt-lane-0`, ID `10603519323`, 78,836 B, 11 files,
   **retention expires 2026-10-04**. It holds `run-7.log.fail-6189`, which
   contains the `[schedcheck]` line with its values. **This container cannot
   fetch it:** the egress proxy rejects
   `productionresultssa9.blob.core.windows.net` with `connect_rejected`, and the
   GitHub MCP surface here has no artifact-download tool. A session with either
   blob egress or `gh` should pull it **before 2026-10-04** — after that the
   values are gone and only a fresh reproduction can recover them.
2. **Add `\[schedcheck\]` to `SIGNALS`** and raise the `head -5` cap, so the next
   fire reports its own diagnosis. Designed here, **not shipped in this commit**
   (NON-NEGOTIABLE 5: DDR before code).
3. **Local reproduction is not available here either** — `qemu-system-x86_64` is
   not installed in this container. Noted so the next session does not assume it.

---

## 8. NOT CLAIMED

* **NO FIX, NO MECHANISM NAMED, OPEN-2 DOES NOT CLOSE.** One occurrence, values
  unread.
* **NOT claimed that `[schedcheck]` fired** — that is an *inference from the
  address*, and a strong one (the halt loop is reachable only from inside
  `if (bad)`, whose only exit is through that `kline_emit`), but the line itself
  has not been read. If the artifact shows no `[schedcheck]`, this section is
  where that contradiction gets recorded.
* **NOT claimed that DDR-1128 was wrong in substance** — its census, its
  statistics and its refusals stand. One reading in its §3 is corrected.
* **DDR-981, DDR-1019, DDR-1105, DDR-1118, DDR-1123, DDR-1127 are NOT withdrawn.**
* **NO RATE for OPEN-2** on this or any binary.
* `OPEN2_HUNT` not retuned; no code change, no gate, no new sentinel;
  `GLOBAL_FORBIDDEN` 77, 179 gates, 79 probe ELFs.

# DDR-1136 — THE 1,200-BOOT DISPATCH WAS SIZED PAST ITS OWN TIMEOUT, AND WHAT IT SHOWED ANYWAY

**Measurement and address resolution only.** No code change in this DDR. `kernel.bin` in the
shipping tree was never rebuilt; it read `25f4dae4a3f90bcb` before and after.
**There is NO FIX and NO MECHANISM is named. OPEN-2 does not close, and no open issue moves.**

Artefact: hunt run **35643638290**, `workflow_dispatch`, `ref=dev/phase1-seyp3n`
(`tree_sha=d1bd866`), `lanes=20 runs=60 hunt=32`, on the pinned binary
**`ca8107ec7f5d8de7`**. Every lane's first line states that pin.

---

## §1 — THE DEFECT IS MINE, IN HOW I SIZED THE DISPATCH

`open2-hunt.yml:122` sets `timeout-minutes: 180`. A clean hunt boot takes **180 s**, because
per DDR-1043 the never-appearing `FORBIDDEN_SENTINEL` makes every boot burn its full window.
So 60 runs need **180 minutes of campaign alone**, before counting checkout, the toolchain
install and the build. **The dispatch could not finish inside its own job timeout**, and I
chose `runs=60` without doing that arithmetic.

What happened, measured from the job logs:

- **17 lanes were cancelled** at 22:15:38–56. Each had printed runs 1–59 and was inside
  run 60. None of them printed a `[campaign] DONE` line. Each one still uploaded its
  artifact, since the upload step runs `if: always()`.
- **3 lanes completed** (lanes 5, 10 and 14). **They finished because each had one or two
  signal runs that ended in about 10 s**, which gave back enough time for run 60.
  **This is a selection effect: the completed lanes are exactly the lanes that found
  something.** Their DONE lines must not be read as a random sample.
- **Lane 9 is the one exception, and I can report it but not explain it.** It started
  ~1 h 53 m after the other lanes and finished all 60 runs *clean* at 00:09:30
  (`DONE runs=60 signal_runs=0 churn_runs=60`). Its campaign alone ran 3 h 00 m 08 s, and I
  have not established why it was not cancelled. So the defect is stated as **marginal, not
  total**: 60 runs × 180 s sits exactly on the timeout.

**The rule going forward:** `runs × 180 s` plus about 10 minutes of setup must stay under
`timeout-minutes`, so **`runs ≤ 55`** at the current timeout. The next dispatch is sized
to that.

**DDR-1132's discriminator was applied before reading any log.** 20 of 20 lanes are
non-success, but this is **not** a setup failure: every lane built the right tree, pinned
the right hash and printed per-run lines. The 20/20 comes from the timeout, which is a
**third cause** of an all-lanes-red dispatch, after DDR-1132's unresolvable ref and a
genuine find. The rule is sharpened: **when every lane is red, read one lane's last lines
before reading any finding into it.**

## §2 — THE DENOMINATOR, STATED HONESTLY

Every lane was read (MCP `get_job_logs`; the zipped run-log download is **blocked** — see §7):

| lanes | outcome | runs verified |
|---|---|---|
| 0, 1, 2, 3, 4, 6, 7, 8, 13, 15, 16, 17, 18 | cancelled, runs 1–59 all `churn clean` | 13 × 59 = 767 |
| 11, 12, 19 | cancelled, one signal run each | 3 × 59 = 177 |
| 5, 10, 14 | completed, own DONE lines quoted in §3 | 180 |
| 9 | completed clean, `DONE runs=60 signal_runs=0 churn_runs=60` | 60 |

- **1,184 boots, all on one binary, every one read.** None of them showed NO-CHURN except
  as the consequence of a fire, so DDR-1130's no-filesystem trap was not in play.
- For lanes 13, 15, 16 and 18, "clean" rests on each log's total length matching a clean
  lane's (599 lines; lane 7 has 598, and its tail was read). **The individual run lines of
  those four lanes were not printed out.** The claim is stated at that strength.

## §3 — THE SIGNAL RUNS, ONE BY ONE

The own-DONE lines for the three completed signal lanes:

- lane 5: `DONE runs=60 signal_runs=2 churn_runs=58`
- lane 10: `DONE runs=60 signal_runs=1 churn_runs=59`
- lane 14: `DONE runs=60 signal_runs=1 churn_runs=59`

### §3.1 `[schedcheck]` fires (the deliberate halt at `schedule_locked+0x671`, sched.c:1857)

| lane/run | tid | disp − saves | rq_on | r15 | ret | rflags − rsp |
|---|---|---|---|---|---|---|
| 10/10 | 22 | 30 − 28 = **2** | 0 | `…16842` | `…16DCF` | **0x30** |
| 14/51 | 22 | 12 − 10 = **2** | 0 | `…1668D` | `…16DCF` | **0x30** |
| 5/1 | 22 | 10 − 8 = **2** | 0 | `…1668D` | `…16DCF` | **0x30** |
| 11/3 | 22 | 4 − 2 = **2** | 0 | `…16842` | `…16DCF` | **0x1C0** |

The addresses are resolved against the a390eab `OPEN2_HUNT=32` rebuild (§6):
- `…16842` = `schedule_locked+0x122`, the return from `call switch_wait_offcpu_sched`.
- `…1668D` = `finish_task_switch+0xd`.
- `…16DCF` = the return after `call local_irq_restore`.

**Every fire in this dispatch reads `disp = saves + 2`.** Adding DDR-1133 and DDR-1134, the
project has now seen **six** saves+2 fires, and **all six read `rq_on=0`**. So DDR-1118's
double-resume precondition is absent **6 of 6**.

**§3.2 — DDR-1134 §3's CO-OCCURRENCE IS BROKEN. This cuts against my own last DDR, and it
is stated plainly for that reason.**
- DDR-1134 recorded that *every* saves+2 fire carried `r15 = …16842` and no saves+1 fire
  did, at n=2 against n≥3, and said so.
- Lanes 14 and 5 are saves+2 fires with **`r15 = …1668D`**, which is the value a genuine
  consumed frame predicts.
- The count is now **4 of 6** with `…16842` and **2 of 6** with `…1668D`. The rule
  "saves+2 ⇒ `switch_wait_offcpu_sched`'s return address in r15" **does not hold**.
- **What this does to DDR-1133 §10.3's four readings:**
  - Reading (ii) is a lost non-atomic `switches_away++`. It predicts saves+2 with no
    relationship to what sits in the r15 slot.
  - A saves+2 whose frame is otherwise the ordinary consumed-frame shape (lanes 14 and 5)
    is **exactly what (ii) predicts**, and it counts against any reading that makes the
    `switch_wait_offcpu_sched` path the carrier.
  - **This does not establish (ii).** It removes the one datum that pointed away from it.
    DDR-1134 said the correlation pointed away from (ii) and did not refute it; the
    correlation is now gone.
- **DDR-1134 is not withdrawn.** Its §3 was correct at n=2 and stated its own strength.
  This is the n≥3 it said was owed, arriving with the other answer.

**§3.3 — THE rflags INVARIANT BREAKS FOR THE FIRST TIME, on lane 11.**
- Until now, `rflags slot = rsp + 0x30` held in all **eight** fires across three binaries,
  and DDR-1133 and DDR-1134 called it the structural invariant.
- Lane 11 reads `rsp=0x07D036E0 rflags=0x07D038A0`, which is **`rsp + 0x1C0`**.
- Its other fields match DDR-1134's lane 7 (tid 22, disp=4 saves=2, r15 `…16842`,
  ret `…16DCF`).
- Its backtrace resolves to **`schedule+0x11 ← yield+0xbd ← reaper_thread+0x108 ←
  thread_trampoline+0x34`**, the reaper. This is the **second** saves+2 fire on the
  reaper's own `yield()`, and both carry `…16842`.
- `0x1C0` is not a slot this chain writes at `S+0x00`. **No reading is offered.** A value
  in a spent frame is litter (DDR-1056). What is recorded is only that the invariant is
  **not universal**, so it must stop being described as one.

### §3.4 Lane 19 run 9 — `[apfreeze]` with NO `[schedcheck]`, and it is not new

- `rip=0xFFFFFFFF80049FB2` resolves to **`spin_lock_contended+0x92`**.
- `bt` resolves to **`spin_lock+0x25 ← spin_lock_irqsave+0x1c ← submit+0x2e ←
  vblk_read+0x34`**.
- That is **the same chain at the same offsets** as DDR-1120 §6 / DDR-1121 / DDR-1122's
  shard-7 freeze, now on a **different binary**. It was resolved against its own binary
  (§INV.18), not matched by offset.
- **And it carries the same census shape:** `ticks[0=1500,1=162,2=160,3=1476]`, **two**
  frozen CPUs (1 and 2), which is DDR-1122's two-victim finding reproduced.
- DDR-1123's census is what makes that readable at all. Before it, this line would have
  been read as one frozen CPU.
- **This producer is distinct from `[schedcheck]` and is not pooled with it** (DDR-1019).
- This log was printed by the old printer, so it contains no lock dump.

### §3.5 Two panics of lane-12 shape, and one that is different

- **Lane 5 run 16.** `NEXUS KERNEL PANIC` banner at capture line 215, `panics_silent=0
  panic_stage=3`. The `[apfreeze] rip=…CAF7` = `isr_dispatch+0xfe7` is the winner's
  terminal halt (DDR-1099's fifth producer). The census `ticks[0=1500,1=1480,2=1478,3=155]`
  shows one frozen CPU.
  **This is DDR-1134 lane 12's shape exactly, down to line 215, and the report body is
  again absent from the job log**, because this dispatch used the old printer. It is the
  second time that defect has cost a panic body. DDR-1135 is the fix, and it lands in the
  same push as this record.
- **Lane 12 run 39 is different.**
  - `NEXUS KERNEL PANIC` at line 239 with `panics_silent=1→2 loser_cpu=3 loser_vec=6`.
    Vector 6 is **#UD**.
  - **`loser_rip=0x0000000007D03BF3` is an address inside tid 22's kernel stack**
    (0x07D00000–0x07D04000), i.e. a CPU executing from stack memory.
  - That is the outcome DDR-1099 §6 predicted for a wrong frame whose RFLAGS slot is
    benign: a return to a stale address. Here the address happens to be stack data that
    decodes as an invalid opcode.
  - **Stated as a consistency, not a mechanism.** The loser's RIP says where it faulted,
    not how it got there.
  - Heartbeats after the panic show `bails≈4400/window` against `calls≈5300`. That is
    downstream of a CPU lost mid-panic and is **not attributed**.

## §4 — NO POOLED RATE

This dispatch contains four distinct producers:
- `[schedcheck]` ×4
- the spin_lock/vblk `[apfreeze]` ×1
- a completed panic ×1
- a silent-loser #UD panic ×1

These are not the same event, and a pooled rate across them is not claimed (DDR-1019,
DDR-1042). DDR-1132 §5's figure is not revised.

For the record only: `[schedcheck]` fired 4 times in **1,184** boots on one binary.

## §5 — NOT CLAIMED

- NO fix and NO mechanism. OPEN-2 does not close. §3.2 removes one datum and establishes
  nothing.
- DDR-1131 §3, DDR-1133 and DDR-1134 are **not withdrawn**. The corrections are recorded
  here and noted at the sites rather than rewritten (DDR-1110).
- NO defect is alleged in `switch_wait_offcpu_sched`, `reaper_thread`, `yield`, the
  `on_cpu` handshake, `spin_lock_contended`, `lock_stat.c`, `virtio_blk.c` or the
  campaign script. The workflow's timeout is correct; my `runs` input was wrong.
- NO code change. `kernel.bin` is `25f4dae4a3f90bcb`, unchanged. GLOBAL_FORBIDDEN is 77.
  179 gates.

## §6 — ADDRESS RESOLUTION (§INV.18)

- **Rebuild.** a390eab (the last build-input commit) was built at `OPEN2_HUNT=32` in a
  **detached worktree in the scratchpad**. It reproduces **`ca8107ec7f5d8de7` bit for bit**
  and builds warning-clean.
- **Submodules.** musl and lwip were linked in from the main tree. That is valid only
  because `git ls-tree` shows the submodule pointers **identical at a390eab and HEAD**; the
  matching hash is the actual proof.
- **Resolution.** Addresses were resolved against `llvm-nm -n` in **python3**, never awk
  (DDR-1079/1121).
- **Cleanup.** The worktree was removed afterwards. The shipping `build/kernel.bin` read
  `25f4dae4a3f90bcb` before and after.

## §7 — BLOCKED HOSTS, REPORTED NOT ROUTED AROUND

- The zipped run-log download was **refused**:
  `results-receiver.actions.githubusercontent.com`, 403 `connect_rejected`.
- The per-lane artifacts are on `productionresultssa*.blob.core.windows.net`, which DDR-1129,
  DDR-1130, DDR-1133 and DDR-1134 all measured as blocked.
- Per `/root/.ccr/README.md`: *"Do not retry or route around it — report the blocked host"*.
  Everything above was read from per-job logs.

## §8 — ALSO RECORDED: the `github-advanced-security` red on PR #17 is not this repo's

- Check run 106478526387 on `d1bd866` failed **before analysing anything**. The Copilot
  code-scanning service returned `CAPIError: 400 The requested model is not supported`.
- No finding was produced about any file, and nothing in the tree can change that
  outcome.
- It is recorded and not acted on. It is not a `pradyos-ci` check and does not bear on the
  3-green criterion.

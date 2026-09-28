# DDR-1130 — THE HUNT CAMPAIGN, RUN STANDALONE, BOOTS A KERNEL WITH NO FILESYSTEM, SO THE CHURN THE RACE REQUIRES CAN NEVER RUN

**Status:** measurement + design. **No code change in this commit** (NON-NEGOTIABLE 5:
DDR before code). No kernel change, `kernel.bin` NOT rebuilt, no gate, no new
sentinel. **NO FIX, NO MECHANISM NAMED FOR OPEN-2, OPEN-2 DOES NOT CLOSE, no open
issue moves (OPEN-1/2/12/13 untouched).**

---

## 1. The artefact

A local 10-run campaign was launched on the pinned `ca8107ec7f5d8de7` — the
`OPEN2_HUNT=32` build of the CR0.WP kernel, i.e. **the binary that actually
produced DDR-1128's `[apfreeze]` in CI** — to try to capture the `[schedcheck]`
field VALUES that DDR-1129 §7 item 1 records as owed.

It was **stopped after 2 runs**, because those two runs establish that it cannot
produce the workload at all:

```
[campaign] kernel_pinned=ca8107ec7f5d8de7 runs=10 smp=4
[campaign] run=1 rc=0 [hb] t=17500 NO-CHURN clean
[campaign] run=2 rc=0 [hb] t=17500 NO-CHURN clean
```

**Two runs is not a rate and none is claimed.** What the two runs establish is
not a rate but a **structural** fact, and one run would have sufficed for it.

---

## 2. The mechanism, read in the source rather than inferred from the symptom

The QEMU command line the campaign built is itself the evidence — **one drive:**

```
timeout 180 qemu-system-x86_64 -machine q35 \
  -drive if=none,format=raw,file=build/pradyos.img,id=disk0 \
  -device virtio-blk-pci,drive=disk0,bootindex=0 \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -smp 4 -no-reboot -display none -monitor none \
  -serial file:build/gatelogs/open2hunt/run-3.log
```

`build/pradyos.img` is the MBR boot image and is **not mountable** — DDR-972's own
comment says so (*"stands in for the boot disk, which is an MBR image and is not
mountable either"*). `build/sfs.img` does not exist in the worktree, and
`boot_test.sh:191` attaches SFS only `if [ -f build/sfs.img ]`. So the guest has
exactly one block device and it cannot be mounted.

`fs_test_thread` then **returns** (`kernel/main.c:1409-1417`):

```c
    for (unsigned j = 0; j < blk_count(); j++) {
        int id = vfs_mount(j);
        if (id >= 0) { mnt = id; blk = (int)j; break; }
    }
    if (mnt < 0) {
        kputs("[fs] no mountable filesystem found\r\n");
        return;                      /* <-- everything below is UNREACHABLE */
    }
```

`smpuser_proof()`, `blkmq_proof()`, `smp_blk_integrity()` and **`rqstress_proof()`**
all sit ~1,500 lines further down **inside that same function**, after
`[boot-stamp] B proofs-begin` (`main.c:2910-2914`). They are not slow. They are
**unreachable**.

`rqstress_proof()` is **not** probe-gated — checked, not assumed: the only call
site is `main.c:2914`, unconditional. So nothing about `QEMU_PROBES`, timing, or
`OPEN2_HUNT` tuning can reach it once that `return` is taken.

### 2.1 Three independent witnesses in the capture, not one

| witness | run-1 | run-2 | on a real hunt boot |
|---|---|---|---|
| `[fs] no mountable filesystem found` | line 154 | present | absent |
| `[boot-stamp]` lines | **0** | **0** | present (stamp B precedes the proofs) |
| `rqstress` occurrences | **0** | **0** | `[smp] rqstress OK` |
| heartbeats with `ymask=0` | **35 / 35** | **35 / 35** | ~13M (DDR-1096 §4.3) |

The `ymask` column is the one worth keeping, because it is **DDR-1096's own
recorded signature** for this condition: that DDR observed `rqstress OK` absent
and *"`ymask` read **0** against ~13M on a clean hunt boot — a different operating
regime."* The heartbeat says the same thing the mount line says, from a different
instrument, 35 times per run.

And the block layer is **healthy** — `[blk] write/read round-trip OK` appears once —
so `blk_count() >= 1`, which is why DDR-972's memory-backed ramdisk branch
(guarded on `blk_count() == 0`) correctly did not fire. Nothing is broken. The
configuration is simply not the one the workload needs.

---

## 3. Why the hunt is the one configuration that reaches this, and CI is not

The gates get the data disks **as Make prerequisites**:

```
smoke-rqstress: $(IMG) fat-image sfs-image
```

The hunt campaign is a **shell script**. It has no prerequisites, and
`grep -niE 'sfs|fat|image|disk|prereq' tools/ci/open2_hunt_campaign.sh` returns
**nothing at all** — the script has no notion that the disks exist.

The workflow supplies them **in a separate step** (`open2-hunt.yml:201-202`):

```yaml
      - name: Build the data disks
        run: make fat-image sfs-image ext4-image >/dev/null
```

So **CI is correct and standalone use is not**, and that asymmetry explains a
number already on record: DDR-1127 measured **60 of 60 runs with churn** on GitHub
runners. It was not luck and it was not the runners being faster — the workflow
satisfies the precondition and a bare `bash tools/ci/open2_hunt_campaign.sh`
does not.

**DDR-972's safety argument is correct and this is outside its stated scope.** It
argued the no-mount branch is *"UNREACHABLE under the gate suite — that is the
safety argument for the change, not a hope"*, because every gate boots through
`boot_test.sh`, which attaches at least one virtio-blk device. True of the gates.
The hunt also boots through `boot_test.sh`, but `boot_test.sh` attaches the *data*
disks **conditionally on their existing**, and it is the Makefile — which the hunt
does not go through — that builds them.

---

## 4. What this does and does NOT do to DDR-1097 §7.2

**DDR-1097 §7.2 is NOT invalidated, and its starvation reading stands for its own
runs.** The arithmetic settles it: that DDR measured NO-CHURN on **1 run in 3**,
which means **2 runs in 3 DID churn** — impossible on a tree without the data
disks, where the rate is 3 in 3 by construction. So DDR-1097's tree had them, its
observation was of a genuinely different phenomenon, and its refusal to retune
`OPEN2_HUNT` on three local boots was the right call then and remains so.

What is added is narrower and is the reusable part:

> **NO-CHURN now has at least two causes, and the classifier reports them
> identically.** (a) Boot starvation — intermittent, DDR-1097's attribution,
> genuinely about the hunt pauses. (b) **No mountable filesystem** — a *setup*
> precondition, **deterministic at 100%**, nothing to do with timing.

A reader of `churn_runs=0` cannot tell which they have. DDR-1097 built
`churn_runs=` precisely so that `signal_runs=0` would have a denominator
(NON-NEGOTIABLE 17), and it does that job honestly — it reported NO-CHURN and
NO-CHURN was true. The gap is that the **cause** is ambiguous, and one of the two
causes makes every further run worthless while looking exactly like the one that
does not.

**This also sharpens DDR-1127 §3 reading (2) rather than contradicting it.** That
reading said `OPEN2_HUNT=32` may not widen the window as much as assumed, and that
*"the 1-in-3 starvation was a property of that HOST, not of `OPEN2_HUNT=32`"*.
Correct — and a second, sharper reason now exists for why a local NO-CHURN figure
must not be pooled with a CI one: they can be **different phenomena entirely**.

---

## 5. The finding: the hunt asserts one precondition and not the other

The workflow already carries a precondition assertion, and it is there for
exactly this class of failure (`open2-hunt.yml:205-206`):

```yaml
      - name: Assert the campaign will pin the binary the previous step built
        run: sha256sum -c build/hunt.sha256
```

DDR-1097 §6 built that because a stale binary would make the hunt *"report clean
forever, stably wrong"* — a **false clean**. The data-disk precondition has the
**identical failure shape**: report clean forever, stably wrong, with no signal
possible because the workload never runs. It has **no assertion of any kind**.

And the trap was already named, by name, in this project, before either:
**DDR-1096 §4.3** built a continuous-churn harness, hit `[sfs] mount failed`, and
refused it —

> *"A reproduction obtained on a kernel that cannot mount its filesystem would not
> be attributable to OPEN-2."*

— and `open2-hunt.yml`'s own comment above the disk-build step **cites that DDR by
number** as the reason the step exists. So the defence was designed, justified,
documented, and placed **in the one path a local run does not take**.

That is the DDR-1117 shape one step across: there, the hunt workflow existed and
was structurally unstartable; here, the hunt's precondition defence exists and is
structurally unreachable from the command a human actually types.

**And this case is worse than DDR-1096 §4.3's.** There, the kernel booted and ran
a degraded workload, so the objection was *attribution*. Here the workload
**never executes**, so it is not a mis-attributable experiment — it is **not an
experiment**.

---

## 6. The remedy, designed here and NOT shipped in this commit

`tools/ci/open2_hunt_campaign.sh` asserts its data-disk precondition **before run
1** and refuses with a named reason, in the shape of the hash assertion that is
already there.

**ASSERT, DO NOT BUILD.** The campaign must not build its own prerequisites, for a
measured reason: DDR-1060 §9 recorded a campaign **voided** because it invoked
`make` and rebuilt the kernel mid-run, changing the binary between recorded runs
with nothing in the report saying so. A campaign that builds is a campaign whose
pin can move. The workflow builds; the campaign checks.

**DO NOT RECLASSIFY THE PER-RUN CASE, and this refusal is the load-bearing half.**
The obvious alternative — score a run carrying `[fs] no mountable filesystem
found` as `SETUP-FAIL` instead of `NO-CHURN` — is **refused**, because once the
disks *do* exist `boot_test.sh` attaches them, and a mount failure then is a **real
kernel defect**. Reclassifying it into a setup bucket would take a genuine defect
and file it under "my environment is wrong" — building the next false clean while
removing this one. The precondition is a property of the *campaign*, checked once;
a mount failure is a property of a *run*, and must stay loud.

### 6.1 The vacuity check, done before the arm was written

*"Assert the campaign still runs"* is **vacuous** — it passes on the unfixed script
in every tree that happens to have the disks, which is every tree anyone has run
it in successfully. The discriminating property is the **refusal**: on a tree with
`build/sfs.img` absent the campaign must exit non-zero **at run 0**, naming the
missing file and the command that produces it, and must **not** boot QEMU at all.
Measured both ways — disks absent → refusal with zero QEMU launches; disks present
→ proceeds unchanged — or the check proves only that it prints something.

---

## 7. What it cost, stated rather than glossed

Two runs at 180 s each, ~6 minutes, plus the wait. Had the campaign been allowed
to finish it would have reported `signal_runs=0 churn_runs=0` over 10 runs — **30
minutes producing a number that bounds nothing**, and a `0` that a later session
could easily have pooled with DDR-1127's CI `0`s, which are from runs that **did**
churn. That pooling is the concrete harm this prevents, and it is the same harm
DDR-1129's `SIGNALS` fix prevented on the other side of the same script.

---

## 8. NOT CLAIMED

* **NO FIX for OPEN-2, NO mechanism named, OPEN-2 DOES NOT CLOSE**, no open issue
  moves. This is about an instrument, not about the kernel.
* **NO defect in the kernel, and none alleged.** `fs_test_thread`'s early return is
  correct, `rqstress_proof()` is correctly unconditional, DDR-972's ramdisk branch
  correctly did not fire, and the block layer round-trips fine.
* **NO defect in `open2-hunt.yml`** — it builds the disks, in the right order,
  with a comment citing the DDR that found the trap. CI is correct.
* **DDR-1097 IS NOT WITHDRAWN OR CRITICISED** — §4 shows its 1-in-3 arithmetic
  requires a tree that *had* the disks, so its measurement was of a real and
  different phenomenon; its `churn_runs=` classifier is what made this visible at
  all, by reporting NO-CHURN honestly on run 1.
* **DDR-1096, DDR-1127, DDR-1128 and DDR-1129 are untouched.** DDR-1096 §4.3 is
  cited as having named the trap first.
* **NO RATE.** Two runs, stopped deliberately; no campaign was completed and the
  local route's exhaustion (DDR-1023) is not revisited.
* **NO change in this commit**: `kernel.bin` not rebuilt, so the size/headroom pair
  and `ci-docstate-check` are unaffected; `GLOBAL_FORBIDDEN` 77; 179 gates; 79
  probe ELFs; no new gate and no new sentinel.
* **DDR-1129 §7 item 1 STAYS OWED.** The `[schedcheck]` field values still exist
  only in artifact `open2-hunt-lane-0` (`10603519323`, expires **2026-10-04**),
  and this container still cannot fetch it — re-measured against the proxy's own
  status endpoint today: `connect_rejected`, *"gateway answered 403 to CONNECT
  (policy denial)"* for `productionresultssa9.blob.core.windows.net:443`, and
  `/root/.ccr/README.md` §"403 / 407 from the proxy" says plainly *"Do not retry
  or route around it — report the blocked host."* Reported here; not routed around.

---

## 6.2 AMENDMENT, same day — EXISTENCE IS NOT SUFFICIENCY, and I hit it within minutes of shipping §6

§6's check tests `[ -f build/fat.img ]`. **That is not enough, and this is not a
hypothetical:** building the disks to run the campaign properly produced the
counter-example immediately.

**The artefact.** `make fat-image sfs-image` in the worktree exited **2**:

```
dd if=/dev/zero of=build/fat.img bs=1M count=64 status=none
mkfs.fat -F 32 -n PRADYOS build/fat.img >/dev/null
/bin/sh: 1: mkfs.fat: not found
make: *** [Makefile:789: fat-image] Error 127
```

`dosfstools` was not installed. The recipe's **first** line is a `dd` of 64 MiB of
zeros and the **second** is the `mkfs.fat` that failed, so what it left behind is
`build/fat.img`, **exactly 67,108,864 bytes, entirely zero** — a file that

* **passes `[ -f build/fat.img ]`**, so §6's assertion is satisfied, and
* **cannot be mounted**, so `fs_test_thread` still returns at `main.c:1414-1417`
  and every run still reports `NO-CHURN`.

That is the *same* silent vacuity §6 was written to prevent, reached through a
check that says it prevented it. A guard that reports success on the condition it
exists to catch is the false-clean class this whole lineage keeps finding
(DDR-1097 §5, DDR-1129 §2) — **arriving this time in my own guard, one commit
after shipping it.**

### 6.2.1 What is checked, and what deliberately is not

| file | check | why |
|---|---|---|
| `build/fat.img` | boot signature `0x55AA` at offset 510 **and** `FAT32` at offset 82 | both lie in the first 512 bytes, one read; `fat-image` runs `mkfs.fat -F 32` so FAT32 is the **declared** format, not an inference |
| `build/sfs.img` | exists and is **non-empty** | it is **legitimately blank by design** — the target's own output says *"16 MiB blank — kernel formats in place"* — so there is no content to check, and demanding one would be wrong |

**The asymmetry is the point.** `sfs.img` gets the weaker check because a stronger
one would be *false*: a correctly-built SFS disk is zeros. Applying the same rule
to both files would have looked tidier and been incorrect for one of them.

### 6.2.2 Vacuity, checked before the arm was written

*"Assert the check still passes on a good tree"* is **vacuous** — it passes on §6's
existing check and on no check at all. The discriminating input is **the
zero-filled 64 MiB `fat.img` the failed build actually produced**, which the §6
check accepts and this one must reject, naming the file and saying the build did
not complete rather than that it is missing.

### 6.2.3 Why not just rely on `make` failing loudly

Considered and rejected as *insufficient*, not as wrong. The build failure **is**
loud — rc 2, a named error. But the campaign's whole hazard is that its result is
**silent and poolable**: 30 minutes later it prints `signal_runs=0 churn_runs=0`,
which a later session can pool with DDR-1127's CI `0`s that came from runs which
**did** churn. A loud failure upstream does not stop a quiet wrong number
downstream — and DDR-1060 §9's voided campaign is the precedent for a report that
looked fine and was not.

**Still ASSERT, not build** (§6 unchanged): the campaign does not run `mkfs.fat`
either. It checks, names what is wrong, and says what to run.

### 6.2.4 NOT claimed by this amendment

* **NOT claimed that `fat.img` is the file that supplies the mount.** Whether a
  blank `sfs.img` alone would let `vfs_mount` succeed is **not measured here**, and
  the check requires both because that is exactly what `smoke-rqstress` declares
  as prerequisites (`$(IMG) fat-image sfs-image`) — matching the gate's own
  declaration rather than a minimal set nobody has established.
* **No defect in `Makefile`'s `fat-image`.** `dd`-then-`mkfs` is the normal shape;
  the missing package is an environment fact, and the recipe reported it correctly.
* **Nothing about OPEN-2 changes.** No fix, no mechanism, no rate; DDR-1129 §7
  item 1 stays owed.

### 6.2.5 MEASURED — three arms, one variable

Run in the `a390eab` worktree, which already held the real data disks and the
pinned hunt kernel `ca8107ec7f5d8de7` (DDR-1128's own binary). **Only the script
varied** (DDR-1042: a mutation that changes two things attributes nothing); the
disks, the kernel and the invocation were identical across arms.

| arm | script | `build/fat.img` | result |
|---|---|---|---|
| **0** | §6, existence-only (`fd98170`) | zero-filled, 67,108,864 B, `sig=0000` | **ACCEPTED** — reached run 1, created `run-1.log`, killed at 30 s by my `timeout` (rc=124) |
| **A** | §6.2, content-checked | **the same file** | **REJECTED** at run 0, `rc=2`, `build/fat.img(not-a-FAT32:sig=0000,type=)`; **no run log, QEMU never launched** |
| **B** | §6.2, content-checked | real, `sig=55aa type=FAT32` | **ACCEPTED** — reached run 1, `run-1.log`, rc=124 |

**Arm 0 is the load-bearing one.** Without it, "the new check catches it" and
"the check was always going to pass" are the same observation — DDR-1126 §6's
rule, which cost a `GATE_RC=0`-before / `GATE_RC=2`-after pair to learn. It
reproduces §6.2's claim rather than arguing it: **the check I shipped one commit
earlier accepts the zero-filled image and boots on it.**

**Arm B is the vacuity control** — the check is not simply refusing everything.

The rejection's discriminating property is the one §6.2.2 named in advance and it
is measured in both directions: **the refusal happens at run 0 and no QEMU starts
at all.** A check that printed a warning and then ran the campaign anyway would
have satisfied a looser arm and produced exactly the 30 minutes of worthless
`churn_runs=0` §7 costed.

Two defects of my own were removed before measuring, both found by re-reading the
diff rather than by any run: two `echo` lines had lost their indentation and sat
at column 0 inside the block, and the guard re-tested `[ -f build/fat.img ]`
when `[ -z "$bad" ]` already implies it — **a redundant test that reads as
necessary**, which is the shape this project keeps flagging elsewhere.

## 9. The fix measured on the campaign it was built for

The 10-run campaign was re-run on the same pinned kernel **with the data disks
present**, and the result is re-derived here from the ten captures on disk rather
than from the terminal line it printed:

```
runs=10  signal_runs=0  churn_runs=10  kernel_pinned=ca8107ec7f5d8de7
```

Every one of the ten carries `[smp] rqstress OK` and three `[boot-stamp]` lines,
and every capture is ~460 lines, so none is the DDR-1023 vacuous-capture case.

**Against the two runs §1 stopped: 0 of 2 with churn. The precondition was the
whole difference**, and that is the measurement that makes §6 worth shipping
rather than merely reasonable.

**`signal_runs=0` here bounds essentially nothing and is not offered as a bound.**
Ten boots against DDR-1128's 1-in-60 give a 95% upper bound of **25.9%** — wider
than DDR-1127's `<4.87%` on 60 boots, so pooling this 0 with those would be the
error DDR-1127's own NOT CLAIMED warns about. What it establishes is that the
instrument now runs the workload; **it says nothing new about OPEN-2's rate**, no
mechanism is named, and DDR-1129 §7 item 1 stays owed.

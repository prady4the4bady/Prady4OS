# PRADYOS — PRE-LAUNCH CHECKLIST

**One document for everything that is deferred, open, uncovered, or awaiting an
operator decision.** Created 2026-09-01 on operator instruction (PR #17), item 3:
*"Consolidate every deferred item into one document — what it is, why deferred,
whether the operator must decide before user testing."*

## How to read this

Every row carries three things, and the third is the point of the document:

| Column | Means |
|---|---|
| **What** | The thing itself, named precisely enough to find in the tree. |
| **Why deferred / open** | The mechanism or the missing subsystem. Not "no time". |
| **Operator decision before user testing?** | **YES** = a person must choose before anyone outside this repo boots the ISO. **NO** = it is recorded, bounded, and safe to carry into user testing as-is. |

A **NO** is not a claim that the item is unimportant. It is a claim that shipping
without it does not create a decision a user's experience depends on, and that
the item is written down here rather than lost.

**Nothing in this document is a fix.** Where a defect is open, it is open; where
a line is uncovered, it is uncovered. §NON-NEGOTIABLE 3 forbids a fix without a
named mechanism from a real failing artefact, and several rows below exist
precisely because that bar has not been met.

---

## SECTION 1 — OPERATOR DECISIONS REQUIRED BEFORE USER TESTING

**§1.1 and §1.2 are the only rows in this document marked YES** — the two
decisions a person must make. §1.3 and §1.4 sit here because they are
*operator-owned* rather than open: §1.3 is a triage whose merges are the
operator's action to take, and §1.4 is a decision the operator has already made
and which is recorded so it is not re-opened by a later session. Everything
outside Section 1 is recorded, not blocking.

### 1.1 — Placeholder branding / logo art must be replaced or licensed

**Status: OPEN — operator decision required. Nothing in the repository ships
art today, and that is the good news; the exposure is upstream of the tree.**

Measured 2026-09-01:

- `find` across the whole working tree for `*.png`, `*.svg`, `*scorpion*`,
  `*logo*` returns **zero files**. There is no `assets/` directory.
- `grep -rni "scorpion|logo|brand|trademark|wordmark"` over all `.md`/`.c`/`.h`
  returns **no branding art reference** — every hit is either the CPUID
  *processor brand string* (`sys_proc.c:137`, `prism.c:51`, `sysinfotest.c`),
  an unrelated identifier, or `TELOPT_LOGOUT` in vendored musl.
- The single design-side reference is `docs/design/LAYER7_UI_UX_BRIEF.md`:
  line 59 specifies a **logo** in the top-left sidebar, and line 104 specifies
  `assets/icons/{sovereign,system}/` (16 "Sovereign Glyphs", 1x/2x + `.json`).
  **Neither exists.** The brief describes art that was never produced, so the
  shipped compositor renders no mark at all.

**So the risk is not in the ISO — it is in the design references.** Any
AI-generated placeholder mark (the scorpion logo, or anything like it) used in
briefs, mockups, screenshots, README art, or promotional material carries
copyright/provenance exposure that a shipped binary would not.

**The decision the operator must make, before any public release or user
testing that produces screenshots:**

1. **Commission or draw an original simple mark** (a geometric wordmark needs no
   illustration and no model), **or**
2. **Buy a properly licensed asset** with a licence file committed beside it,
   **or**
3. **Ship with no mark at all** — which is the current, and safest, state.

Option 3 is what the tree does today and requires no work. Options 1 and 2
require the operator to act; neither can be decided by an implementer.

**Whichever is chosen, the same rule applies to the 16 Sovereign Glyphs** in the
brief §6. They are unbuilt. If they are ever built by a generative tool, they
inherit this same item.

**Do not treat this as closed until an explicit operator answer is recorded
here.** This section exists so the item cannot be lost, which was the operator's
stated reason for asking for it.

### 1.2 — `v1.0.0` is deliberately untagged, and `main` promotion is unstarted

**Status: HELD BY OPERATOR DECISION. Not an oversight.**

`dev/phase1` was fast-forwarded to `ace232f` and the release candidate was
verified on it (BIOS + UEFI ISO arms, plus `smoke-iso-userspace`, which boots a
live OS: SFS root + PRISM + an AETHER agent + a write/read/delete round-trip).
The tag was then **held**, on an explicit decision, because the OPEN-1 campaign
had just found a locally reproducible ring-0 `#PF` (DDR-985, 1/20).

Since then the picture changed but the hold was not lifted:

- Route 2 (that `#PF`) is **CLOSED** at 95% power — 60/60 clean on one kernel
  hash `5349db4d791cc2ab`, against a threshold fixed before the run (DDR-1000).
- Route 3 is **CLOSED** (DDR-990 §9, mutation-checked both ways).
- **Route 1 is still open**, and it is CI-only, so no local campaign can close it.

**The operator must decide whether the original reason for the hold still
holds.** It was "root-cause the `#PF` before tagging"; that `#PF` is closed. What
remains open is a different signature that a local campaign structurally cannot
settle. Continuing to hold is defensible; so is tagging with route 1 named in
the release notes (DDR-1011 §4 already drafted that wording). **An implementer
should not make this call**, which is why it sits in Section 1.

### 1.3 — Six dependency PRs: triage recorded, merging is the operator's action

**Operator instruction (PR #17, 2026-09-01):** each open dependency PR needs
*"merged (if low-risk and CI-green), or explicitly deferred with a reason."*

**One structural fact decides most of this, measured rather than assumed:**
`tools/graph_mcp` runs in its **own CI job** (`ci.yml:196-205` — `npm ci` then
`node server.js selftest`). It is referenced nowhere in the Makefile and by no
`smoke-*` gate. So its npm dependencies **cannot affect a gate result or the
shipped OS image**; their blast radius is a developer graph tool.

| PR | What | Disposition | Reason |
|---|---|---|---|
| **#2** | npm group: `@hono/node-server`, `fast-uri` | **CLOSE — superseded** | Already remediated. `package-lock.json` carries 2.1.0 and 3.1.5, at or above every advisory's fix, and `npm audit` reports 0 vulnerabilities at every severity. Its base is `fd876cd`, far behind `main`. Merging it would be a no-op at best. |
| **#3** | Docker `ubuntu` 24.04 → 26.04 | **DEFER post-1.0** | Not security. `Dockerfile:17` pins 24.04 **deliberately**, so container and WSL builds agree; changing the whole toolchain under 168 gates days from release reintroduces exactly the drift the image exists to remove. |
| **#7** | `actions/checkout` 5 → 7 | **DEFER until after the CI efficiency work** | Used at 5 sites in `ci.yml`. Any `ci.yml` change alters the environment every gate runs in, and §NON-NEGOTIABLE 1 needs **3 greens on one tip SHA** — so merging this resets accumulated release evidence for no release-relevant gain. It also touches the same file the operator's own item 5 (caching + shared build artifact) will rewrite, so merging now buys a conflict. |
| **#8** | `actions/setup-python` 5 → 7 | **DEFER — same reason as #7** | Same file, same evidence-reset argument. |
| **#9** | `sql.js` 1.14.1 → 1.14.2 | **SAFE TO MERGE** | A patch bump, and `package.json:21` already declares `^1.10.0`, so 1.14.2 is **inside the existing range** — this only moves the lockfile pin. Dev-tool scope, cannot reach a gate. |
| **#15** | `web-tree-sitter` 0.22.6 → 0.26.13 | **HOLD — the only one with real breakage risk** | `package.json:23` pins it **exactly** (`"0.22.6"`, no caret), so this is a genuine minor-series jump in a WASM parser whose API changed across those versions. It can break `node server.js selftest`. Still **not release-blocking** — the graph tool is not in the OS — but it should be merged and watched deliberately, not swept in with the others. |

**Why I have recorded dispositions rather than merged anything.** These PRs
target `dev/phase1`; this session's mandate is `dev/phase1-seyp3n` and forbids
pushing to another branch without explicit permission. Merging is therefore the
operator's action. The instruction asked for a decision *"recorded somewhere"*,
and this is that record. **#9 is the one I would merge today**; #2 I would close.

### 1.4 — DDR-1019's panic-latch watchdog: DECIDED, deferred, not built

**This is a closed decision, not an open question.** Recorded here because it
was left open by DDR-1019 and the operator closed it explicitly (PR #17,
2026-09-01).

**What it would have been.** DDR-1019 established that one producer of
`[apfreeze]` is the *losing* branch of DDR-979's one-winner panic latch: the
latch is claimed **before** the dump and never released, so a winner that cannot
print silences every later panic and leaves only frozen CPUs. The named repair
is a latch-liveness watchdog, so a losing CPU's crash is not silently swallowed.

**The decision: do not build it on this timeline.** The reason, verbatim in
substance from the operator: the risk of reintroducing the garbled-dump
interleaving that DDR-979 fixed, days from release, outweighs a nicer crash
message for a failure mode already being chased by other means — the SWAPGS
probe (DDR-1010) and the two other `[apfreeze]` producers already distinguished
(DDR-1006's `sched_tick` path, DDR-1010's `sys_mmap` path).

**What remains true and must not be lost:** before reading any new `[apfreeze]`
as a known producer, **resolve its RIP against its own binary** (§INV.18). Three
producers share the label. That discipline is the mitigation now, in place of
the watchdog.

**UPDATE 2026-09-03 — DDR-1049 closes the DETECTION half, not the watchdog.**
The decision above stands: the latch is still claimed before the dump and still
never released, so a winner that cannot print still silences every *later* panic.
What changed is that such a run can no longer pass. DDR-1019's `panic_stage`
field was gated behind `g_panic_extra`, which increments **only in the loser
branch of the CAS** — so a *lone* silent panic printed nothing at all: no banner
(no `GLOBAL_FORBIDDEN` trip), no `panics_silent=`, no `panic_stage=`, and on a
PASS `boot_test.sh` deletes the capture. **The run went green with a panicked CPU
in it.** The predicate is now `g_panic_extra || g_panic_stage` (the winner sets
`g_panic_stage` the instant it claims the latch), and `panic_stage=` is in
`GLOBAL_FORBIDDEN`, so a claimed panic fails its gate either way. M1 proven,
173 gates × 2 CI suites green with the pattern live. **The watchdog is still not
built and is still the right deferral.**

### 1.5 — CI efficiency refactor: CONFIRMED SAFE, with two hazards that must be handled

The operator asked for explicit confirmation **before** implementation, and for
any structural reason the workflow is shaped this way to be stated rather than
silently changed. Queued as item 5 (after this checklist and RUN_EXPERIMENT);
this is the confirmation, not the change.

**The redundancy is real, and larger than described.** Every one of the 10
`build-and-boot` shards independently runs (`ci.yml:80-119`): `apt-get install`
of the full toolchain, `rustup toolchain install nightly` + component + target,
`make toolchain-check`, `make smoke-selftest`, `make musl`, `make lwip`,
`make image`, `make ci-probe-rodata-check`, then its own shard. So it is **10
identical kernel compiles per CI run**, plus 10 apt installs and 10 rustup
installs.

**No structural reason blocks sharing the build.** Checked rather than assumed:
no gate assigned to a shard rebuilds the kernel or alters its flags. The one
gate that does — `smoke-fs-liveness`, which rebuilds with `BSP_LIVENESS=1` — is
**already excluded from the matrix for exactly that reason** (DDR-777/790). So
the premise holds.

**Hazard 1 — `make` is mtime-driven, and this is the failure that matters.**
Each `smoke-*` target has `$(IMG) fat-image sfs-image` as prerequisites.
`actions/download-artifact` does not reliably preserve mtimes, so a downloaded
`build/pradyos.img` can appear *older* than the checked-out sources and make
will rebuild it — either silently keeping the cost, or worse, **producing a
different binary than the build job tested**. That second outcome is not a
performance regression, it is a correctness one: this project's statistical
arguments (DDR-1009's twelve-run table, DDR-1023's 20/20, DDR-1010's 36/36) all
rest on knowing which runs shared one binary.

**Which is why the refactor should make that structural rather than weaken it:**
have the build job publish `sha256sum build/kernel.bin`, and have each shard
**assert the hash it downloaded matches** before running a single gate. Today
"all 10 shards ran the same binary" is inferred from them compiling the same
source; afterwards it would be *checked*. That is a net gain in rigor, not a
trade against it.

**Hazard 2 — `smoke-selftest` must stay per-shard.** It is a setup step in every
shard **by deliberate design** (DDR-785): a shard whose gates trust the boot
harness must have checked the harness first. It is host-only and cheap. Do not
fold it into the build job.

**Unchanged, as instructed:** `fail-fast: false`, the 3-independent-greens rule,
and every gate's timeout, sentinels and test logic. Toolchain caching (apt +
rustup) carries neither hazard — it cannot change what is built — and is the
safer half to do first.

---

## SECTION 2 — OPEN DEFECTS

None of these is closable by effort alone. Each is waiting on evidence, and each
has an armed instrument that makes the next occurrence diagnostic.

| # | Symptom | State | Operator decision? |
|---|---|---|---|
| **OPEN-1** | `smoke-surfdestroy` intermittently misses `PRADYOS_SURFDESTROY_CHURN_OK` | Routes 2 and 3 **CLOSED** (DDR-1000, DDR-990). **Route 1 OPEN**: a CI-only hang whose recorded stopping point is `SYSFSTAT OK`. DDR-1009 §2 found a capture stopping at exactly that point on a *different* gate that **panicked** — so route 1 is not always silent, and DDR-994's "a hang prints nothing" framing is too strong. A stopping point is not a cause. Local reproduction is structurally unable to settle it. | NO — but see §1.2, which is the decision this feeds |
| **OPEN-2 — third CI occurrence, resolved to DDR-1006's site** | `smoke-rtc-smp`, shard 5, `7392d0e` (2026-09-02) | **RIP RESOLVED BEFORE MATCHING, per DDR-1019's rule.** `rip=0xFFFFFFFF8000AE37` → **`isr_dispatch`**; `bt=0xFFFFFFFF8000027A` → **`isr_common.gs_kernel_in`**; the separate panic dump's backtrace is `schedule` ← `sched_ap_enter` ← `smp_ap_entry`. That is **DDR-1006's AP-timer-ISR shape** — **not** DDR-1019's `idt.c:697` halt loop and **not** DDR-1010's `sys_mmap` path. **Second, independent confirmation it is not DDR-1019's:** that producer sets `g_panic_extra`, which surfaces `panic_stage=` in the heartbeat block, and no heartbeat in this capture carries it. **DDR-1049 makes this test strictly stronger for FUTURE captures, and narrows what it proved for THIS one:** at the time, absence of `panic_stage=` excluded only a panic in which something *lost* the CAS — it could not exclude a lone silent winner, because the field was gated behind `g_panic_extra`. Since DDR-1049 the field prints whenever the latch is claimed at all, so from now on its absence excludes *any* claimed panic. The conclusion for this capture is unchanged (DDR-1019's producer is by definition a loser, so `g_panic_extra` would have been set) — but the reasoning was narrower than it read. Accompanied by `[vblk] compl wait timeout unit=2 dest_cpu=3 dest_dticks=0` — cpu 3 frozen at `ticks=72` with `if=0`, the DDR-981 signature, at a site DDR-981 does not cover. **Attribution limit, stated:** unlike the `smoke-nethammer` case, `kernel.bin` was **not** bit-identical to the previous tip — `7392d0e` shipped `SYS_POLL`. So this is not exonerated by diff. What can be said: `sys_poll` is not on the AP timer ISR path, `smoke-rtc-smp` uses neither poll nor epoll, and the signature predates the change. **Symbol resolution caveat (§INV.18):** resolved against a *local* rebuild of `7392d0e` (`a411e1b1b765e15e`); CI's published hash could not be read back out of the job log, so binary identity rests on DDR-1023's established bit-for-bit reproducibility rather than on a direct comparison. | NO |
| **OPEN-2** | `[apfreeze]` in CI | **The label covers at least THREE distinct producers** (DDR-1019). Resolve any new RIP against its own binary before matching: DDR-1019's is the panic-arbitration *loser's* halt loop at `idt.c:697`; DDR-1006's runs through `sched_tick`; DDR-1010's through `sys_mmap`. The original (DDR-981, `yield()` spinning with IF clear) is genuinely fixed. **Local reproduction is EXHAUSTED** — 56 clean runs across the two kernels that matter (36/36 post-probe, 20/20 pre-probe, DDR-1023), including the exact binary the failure was first seen on. The old "~1 in 4" was one session's small sample and has not held up; stop quoting it as a rate. | NO |
| **OPEN-12** | `*** NEXUS KERNEL PANIC *** / component: NEXUS isr` | **Root-cause candidate found and fixed** (DDR-996): `sched_exit` left a thread linked on its per-CPU runqueue and both reap paths unlinked only the all-threads ring, so a TCB could be freed while a queue still pointed at it. 16/16 victims measured, mutation-checked. **NOT closed**: OPEN-12's *original* capture lost its RIP to the interleaving DDR-979 later fixed, so identity is unproven and matching on `component:` alone would be colour-matching. Closes on a clean campaign, not on the fix. | NO |
| **OPEN-13** | `[kheap] double-free … objsize=0x80` → KHEAP PANIC | **Instrument BUILT and mutation-proven** (DDR-1024): the line now carries `freed_by=` and `now_by=`, the first and second frees' return addresses, captured at the public `kfree`/`pcb_free`/`cap_free`/`ipc_free` boundary. `objsize=0x80` is a generic 128-byte class, not a dedicated cache, so "size class → structure" does not resolve. **One CI capture, no mechanism named — NOT a fix.** The next occurrence is diagnostic: resolve BOTH addresses against the exact binary that produced the log. | NO |
| **`smoke-nethammer` intermittent** | timeout on CI **shard 3**, now **twice** — DDR-1009's twelve-run table, and again on `c58f9c2` (2026-09-02) | **Root cause NOT established.** It is one of the four signatures in the DDR-1009 pooled measurement — twelve independent CI suite-runs of one provably identical kernel binary, 9 green / 3 failed, at four gates with four different signatures. Named here so it lives in one place rather than only in a CI comment thread. **Not a release blocker** on its own reasoning: the gate is strict and gating, so a recurrence reddens its suite and cannot pass silently. **Second occurrence adds three facts and no mechanism.** (a) Same gate, same shard, same signature — a timeout, not a hang: heartbeats run unbroken `t=1000`→`t=23500` with ticks advancing, `rqdepth` 1-3, **no `[apfreeze]` and no panic**, so the kernel is alive for the whole window and the sentinel simply does not arrive. (b) It cannot have been caused by the commit it landed on: `git diff --name-only 2ad4a0f c58f9c2` is five files with **zero** under `kernel/`, `user/`, `arch/` or `third_party/`, so `kernel.bin` was bit-identical. (c) The DDR-1035 `if: always()` hash assertion passed on the red shard (`kernel.bin: OK`), which is how (b) was confirmed from CI rather than inferred. One `workflow_dispatch` re-run taken — the single re-run permitted when a failure names a subsystem the diff does not touch. **Still no mechanism.** **THIRD OCCURRENCE 2026-09-03**, `c8c93ed`, shard 3, gate 20 of 21 — the same signature exactly: heartbeats unbroken `t=16000`->`t=23500`, ticks advancing, `rqdepth` 1-2, no `[apfreeze]`, no panic, `kernel.bin: OK`. That commit changes **three `.md` files and nothing else**, so the binary was bit-identical to `9d8fcb9`, which was 32/32 green an hour earlier. **New, and only available since DDR-1049:** the absence of `panic_stage=` in every heartbeat now positively excludes a lone silent panic — before that fix it could not, because the field was gated behind `g_panic_extra`, which increments only in the loser branch of the CAS. Three occurrences is a better-characterised signature, not a cause. **The `workflow_dispatch` re-run on the IDENTICAL SHA `c8c93ed` came back 32/32 green** (run 33726808985), and `74119a0` is green on both suites — so the failure does not reproduce on re-run, which is the test that separates a transient from a real one. Transient is now measured rather than inferred; the cause is still not named. | NO |
| **`smoke-actiondel` intermittent** | `[actiondel] FAIL — no measured line in the capture`, CI shard 1, `c8c93ed` (2026-09-03) | **First occurrence in this shape; root cause NOT established.** The boot itself **PASSes** (`saw 'NEXUS KERNEL OK'`) and the full 120 s window is consumed, then the gate's checker finds no measured line — so the kernel booted and the DDR-1016 probe's output never appeared. It landed on the **same docs-only commit** as the third `smoke-nethammer` timeout, in a *different* CI suite, with `kernel.bin: OK` on both shards: the binary is bit-identical to the 32/32-green `9d8fcb9`, so neither failure can have been caused by that commit. Two independent gates timing out on one push is **consistent with** runner contention, but that is a hypothesis and has **not** been measured. `smoke-actiondel`'s probe does a ring-3 spin with zero syscalls (DDR-1016: a force-pending probe that busy-polls is killed by `AGENT_RATE_MAX`), so it is timing-sensitive by construction — named as a place to look, **not** as a cause. **Not a release blocker on its own reasoning:** the gate is strict and gating, so a recurrence reddens its suite and cannot pass silently. **The `workflow_dispatch` re-run on the identical SHA came back 32/32 green**, so this did not reproduce either. | NO |
| **`resched FAIL ipis=0 ran=1 idle=1`** | intermittent, `smoke-rqstress-liveness` | **A documented sampling ambiguity with no rate bound.** The property under test HELD (`ran=1` — the unblocked thread ran); the IPI term is a stronger claim layered on top, and `idle_seen` is sampled just before `sched_unblock`, so a CPU can leave idle in between and a correct system FAILs. **Deliberately not collapsed to SKIP** — that would green the gate and delete the coverage DDR-1014 built, the trade DDR-1012 and DDR-973 each had to undo. A second sample (`idle2=`) is armed and mutation-proven, so the next occurrence self-diagnoses: `idle2=0` = sampling artefact, `idle2=1` = a real scheduler defect. **No rate has been measured**, and that gap is the honest limit here. | NO |
| **smoke-agents preempt frozen** | `rqdepth=11`, two sentinels missing | **NOT REPRODUCED since instrumentation** (DDR-968, live since `ea4601e`). The witness line prints only on a *failing* boot, so a green boot emits none of it — there is no red artefact to read. The gate is gating (shard 2, not excluded), so a recurrence would redden its suite. Reopens on the first `PRADYOS_AGENT_WITNESS_WAIT` line. | NO |

---

## SECTION 3 — CORRECTIONS: DEFERRAL ENTRIES THAT ARE NOW STALE

`docs/BUILD_TRACKER.md` §"Pre-approved exceptions" still carries three
`[DEFERRED]` entries for work that **has since shipped**. They are corrected
here; the tracker rows themselves are superseded by this section.

| Tracker entry | Correction |
|---|---|
| `ACTION_SEND_IPC` — *"no ring-3 IPC surface … there is no `SYS_IPC_*`"* | **BUILT — DDR-1033.** `SYS_IPC_SEND` / `SYS_IPC_RECV` are NSI 98/99, gated by `smoke-sendipc`, mutation-checked M1/M2/M3 on distinct kernel hashes. Addressing is by roster slot (the same index `SYS_AGENT_ROSTER` already uses, so no new namespace). Two layers: `is_ipc` on `struct tcb` (kernel-set at spawn, never mintable, zeroed in `sched_create` per §NON-NEGOTIABLE 10) plus a `RES_IPC` capability handle. **See §4.1 for what it still does NOT do.** |
| `F#73 sovereign NL UI` — *"blocked on … no windowed terminal client, and `sys_exec.c:47` discards argv/envp"* | **Both blockers are gone.** The windowed terminal client is `user/term.c` (DDR-1027, `smoke-ctrlaltt`); argv/envp marshalling is DDR-1032 (`smoke-execve-argv`), with PRISM's `run` passing arguments through (DDR-1032b). F#73 itself remains unbuilt, but it is now unbuilt *work*, not a blocked item. |
| `ACTION_RUN_EXPERIMENT` — *"`CAP_EXEC` is a `#define` checked nowhere … no `is_exec` on `struct tcb`"* | **BUILT — DDR-1034.** `CAP_EXEC` is now checked (`RES_EXEC` + `cap_authorize`) and `is_exec` is a real `struct tcb` field, kernel-set at spawn and zeroed in `sched_create`. The executor is a bounded integer stack machine with **no memory opcodes at all**, so "no access outside its own stack" is structural rather than a guard. Results go to a separate kernel-written ring; the DDR-812 lockbox is untouched. Gated by `smoke-runexp`, mutation-checked. **Residual, same as SEND_IPC:** the AETHER action path does not call it, so an approved `RUN_EXPERIMENT` has no automatic effect. |

---

## SECTION 4 — MEASURED, RECORDED, NOT FIXED

Residuals that were found by measurement during this work, stated rather than
quietly carried. Each is bounded and none blocks user testing.

### 4.1 — The AETHER action path does not yet call `SYS_IPC_SEND`

DDR-1033 built the ring-3 door. It did **not** wire the AETHER executor to it.
So an approved `ACTION_SEND_IPC` still has **no automatic effect** — a process
must call the syscall itself. This is the honest residual of that DDR and is
recorded here so "SEND_IPC is shipped" is not read as more than it is.

### 4.2 — The IPC capability is coarse by construction, and the limit is stated not implied

Every roster-slot endpoint shares one `res_id`, so the capability grants *"IPC
at all"*, **not** *"send to slot 3 but not slot 5"*. That is a real check and a
coarse one. Per-slot `res_id`s are the extension if policy is ever wanted.
Deliberate, documented in DDR-1033, not a gap discovered later.

### 4.3 — `ACTION_SEND_IPC` auto-approves in sovereign mode with nothing to act on it

Both deferred action types are **in** the enum, so `SEND_IPC` auto-approves in
sovereign mode. This contradicts `aether.h`'s own policy for the six types it
omits. Left alone deliberately: the enum is append-only wire format, and the
entry is bounded and audited (DDR-1021).

### 4.4 — `vmm_protect_range`'s `invlpg` is UNCOVERED

DDR-1031 M2 (drop the `invlpg`) **passed every arm**, and the DDR's own
prediction was wrong: arm B is decided by the child's page tables, not the
parent's TLB, and arm C's write succeeds under a stale writable entry too. A
missing invalidation is only visible as a write that should have faulted and did
not — and on this kernel the observer dies (`idt.c:703` goes straight to
`sched_exit`; there is no SIGSEGV handler). **Recorded as an uncovered line, not
claimed as tested.** A probe of this shape cannot cover it.

### 4.5 — `ptnode_in_use` underflows on every fork

DDR-1003: it counts `++` per **frame** but `--` per **mapping**, and
`vmm_cow_fault` drops its old ref with `pmm_free_page` and no `--`. Read-only
text pages are shared with no COW at all, so they can never rebalance. **Not
fixed**: no gate reads it across a fork, so there is no artefact
(§NON-NEGOTIABLE 3). DDR-1003 §5.1 says what a gate must do — **and warns that
the obvious leak-gate shape is balanced and would pass.**

### 4.6 — The compositor takes ~28 wall seconds to become ready

DDR-1029, measured with a `SYS_CLOCK` stamp: **30 full-screen 1024×768
render+present operations at ~0.93 s each**, all inside `set_ambiance`
(`compositor.c:990`) — 4 announce transitions + 1 settle × 6 frames. Nothing
about the frame loop is pathological. **No fix**: the 24 announce renders each
emit a `PRADYOS_AMBIANCE <name>` sentinel and gates assert on those, so cutting
the renders while keeping the prints would make every one of those assertions
vacuous — the exact failure DDR-1012 removed from `smoke-horizon` and DDR-973
from `smoke-fat32-multicluster`. Options named (leave it / lower `frames` /
assert on framebuffer readback); none is a one-line change.

**This is user-visible.** A desktop that takes half a minute to appear is a
legitimate first-impression concern for user testing. It is a *known cost with a
named mechanism*, not a defect, which is why it is NO rather than YES — but the
operator should know it before showing the desktop to anyone.

### 4.7 — The mouse press latch coalesces, and a missed RELEASE is still missed

DDR-1026's latch is a **bitmask, not a counter**. Repeated clicks between two
polls still coalesce, and a release edge that ends between polls is still lost.
Fixing that needs an event queue, not a latch. `SYS_MOUSE_POLL` exposes current
state by design (DDR-941).

### 4.7b — `mouse_inject.sh`'s readiness sentinel falls through silently

Found while building `smoke-ghostclick` (DDR-1036 §9.3b). The injector polls for
its readiness sentinel and, on expiry, **injects anyway** — so a sentinel that
never appears does not fail the gate; the injector proceeds and clicks against
whatever geometry happens to be in the log. A gate whose readiness condition
silently never held still reports a pass or a normal-looking failure.

The ceiling is now a parameter (`READY_TIMEOUT_S`, default 60 — unchanged for
the seven existing callers). **The fall-through is deliberately unchanged:**
making an expired sentinel fatal is the right shape, but the other gates may
depend on proceeding and changing that days from a release is a wider blast
radius than the DDR that found it should take. Post-1.0, and it needs each of
the seven callers checked rather than a blanket change.

### 4.8 — `mouse_inject.sh` cannot tell a live window from a ghost

The serial log is append-only, so `resolve_geometry` still resolves the last
published geometry for a window that has since closed — and clicks it. Measured
at 45 clicks on a dead window in one capture (DDR-1028). The repair is named,
not built. Gate-harness only; no product impact.

### 4.9 — SFS in-place rewrite of an existing file returns short

DDR-1020 M4: a plain rewrite of an existing SFS file returns short for both a
longer *and* an equal-length payload, while `unlink` + recreate succeeds. The
ADR-032 write budget is **excluded** as the cause, because the `unlink`+create
succeeded at the same point. **Unexplained, unfixed.** A mutant that fails to
perform its own defect is indistinguishable from a gate that catches it — this
was found that way.

### 4.10 — `resched FAIL ipis=0 ran=1 idle=1` is a documented sampling race

DDR-1030. The property under test **held** (`ran=1` — the unblocked thread ran);
the IPI term is a stronger claim about mechanism layered on top. `idle_seen` is
sampled just before `sched_unblock` and a CPU can leave idle in between, so no
kick is owed and a correct system FAILs. **Deliberately not turned into SKIP** —
that would green the gate and delete the coverage DDR-1014 built, the trade
DDR-1012 and DDR-973 each had to undo. A second sample (`idle2=`) is armed and
mutation-proven, so the next occurrence self-diagnoses: `idle2=0` means
sampling artefact, `idle2=1` means a real scheduler defect.

---

### 4.11 — `lock_stat` cannot see `mnt_lock`, the OPEN-1 route 1 prime suspect

DDR-1047 ships spinlock contention accounting (hit counts and **wait time**;
hold time deliberately dropped — an always-on `rdtsc` pair in `spin_lock` could
*move* OPEN-2 rather than measure it, which is the hazard DDR-1010 recorded about
its own probe). Two sites are **not** covered, both found by enumeration:

- `sched.c:787` is a **trylock** in the work-stealing victim scan. It never
  waits, so there is nothing to time — correctly out of scope. Consequence:
  steal-path runqueue contention is invisible.
- **`vfs.c:34` `mnt_lock` is not a `spinlock_t` at all** but a sleep-mutex over a
  bare `busy` byte. So the one lock CLAUDE.md's own Group A row and DDR-994 name
  as the unbounded wait on **OPEN-1 route 1's path** is the prime suspect this
  instrument cannot see.

Not folded in because the quantities are **not commensurable**: a spin-wait is
cycles this CPU burned, a yield-wait is wall time during which the CPU ran other
threads. One `waitavg` column holding both invites a specific, plausible, wrong
comparison with a real number behind it — the DDR-1042 failure mode exactly.
DDR-994's threshold instrument covers the *stuck* case there; `lock_stat` would
have covered the *cumulative* one, and only the first exists.

**No `smoke-lockstat`, deliberately** — the dump prints only on `[apfreeze]`,
which is in `GLOBAL_FORBIDDEN`, so any assertion on it is unreachable on a green
run. Do not create that gate.

### 4.12 — `apt_prepare.sh`'s SIGPIPE race: fixed, cause of its CI silence unexplained

DDR-1048. The shipped resolve check was `apt-cache policy "$p" | grep -q
'Candidate:'` under `set -o pipefail`. `grep -q` exits at the first match and
closes the pipe, `apt-cache` dies of SIGPIPE (141), and the pipeline is non-zero
**although grep matched** — so a package that resolves fine is marked missing.
Measured `PIPESTATUS=(141 0)`; which package loses the race varies per run.
Fixed by capturing the output once and matching it as a string: **0/20 failures**
against every observed run failing for the old form.

**What is NOT established:** CI has been **green** with the racy form, and this
does not claim CI was about to break. Why the race has not fired on the runner is
unexplained. A pipe-buffer explanation was proposed and then **refuted by
measurement** (`apt-cache policy llvm` is 211 bytes, far inside the 64 KiB
buffer). Recorded as an open question rather than given a plausible answer.

### 4.13 — A DDR-1045 premise that does not reproduce

DDR-1045 states *"apt-get update exits non-zero if ANY configured repo fails"*.
Measured on Ubuntu 24.04 (DDR-1048): `apt-get update` exits **rc=0 with four 403
Forbidden** vendor responses, and also with **zero** sources configured. The
DDR-1045 *fix* is unaffected — it tolerates a failing update and then proves the
index usable by resolving every requested package — but its stated rationale is
partly unverified, and the runner's observed exit 100 is left unexplained rather
than explained away.

---

### 4.14 — I/O APIC stage D: deferred, and the blocker is an absent ACPI `_PRT` parser

DDR-1050. The Group A row reads as unbuilt work; most of it is built. DDR-874's
`kernel/apic/ioapic.c` ships MADT parsing, MMIO mapping, `ioapic_route()` and an
Interrupt Source Override table, live at `gsi_base=0 redirs=24 overrides=5`.
**Stage D is the cutover only.**

Three findings, all from measurement rather than reading the row:

1. **The scope is not one line.** Four sites claim ISA IRQs — `console.c:236`
   (**IRQ4, COM1 RX, unconditional** — the path every gate uses to feed PRISM),
   `idt.c:694` (IRQ1 keyboard), and **PCI INTx fallbacks** in `virtio_net`,
   `virtio_input` and `virtio_blk`.
2. **The blocker.** There is **no ACPI `_PRT` parser**. A MADT ISO table
   describes *ISA* overrides, not which GSI a PCI device's INTA# lands on, so
   those three fallbacks cannot be routed correctly. They are dormant here
   (every virtio device takes MSI-X, measured: vec 54, 56–59) — but **dormant is
   not absent**; the fallback exists for a machine without usable MSI-X.
3. **The benefit is already delivered.** `ioapic.c`'s own header says the I/O
   APIC exists so per-CPU affinity has somewhere to be expressed — and MSI-X
   already provides that for every device carrying real traffic (DDR-714C1/C3,
   DDR-771). Stage D would additionally move only the PS/2 keyboard and COM1
   serial: two low-rate lines whose affinity nobody needs, on the highest-risk
   path in the tree.

DDR-1050 §5 records the safe subset and its two traps so they need not be
re-derived: the **EOI must flip to `lapic_eoi()`** (`idt.c:700` calls `pic_eoi`
unconditionally, so the second keystroke would never arrive), with the flag set
*before* the line is armed; and a gate asserting "keys still work" **passes with
the 8259 still in charge**, so it needs a GSI marker plus a PIC-mask readback.

**Not a defect report** — DDR-874's work stands and is exercised. What is
deferred is the cutover.

---

### 4.15 — KASLR: deferred on sequencing, not on a blocker

DDR-1051, and unlike §4.14 this is **a judgment call, not an absent subsystem**.
KASLR is buildable here. The stated precondition is met — W^X is CI-green.

Measured costs: the kernel is `-fno-pic -fno-pie -mcmodel=kernel` at a pinned
`KERNEL_VBASE`, and `llvm-readelf -S build/kernel.elf` reports **zero relocation
sections**, so a virtual slide needs a relink *and* a boot-time relocation
applier that does not exist. It must land in **both** boot paths in one commit
(§INV.13 — PT_HI is implemented twice) or one arm of `smoke-iso-x86` breaks.

**The reason for deferral is neither of those.** KASLR would degrade the one
diagnostic discipline every open defect depends on: §INV.18 and DDR-1019 both
require resolving a RIP against its own binary, `[apfreeze]` has **three**
producers told apart *only* by RIP, and OPEN-1, OPEN-2, OPEN-12 and OPEN-13 are
all read that way. Under a slide, every future CI artefact needs the slide value
to interpret — so the kernel must print it. Printing it every boot returns it to
the attacker; printing it only under a debug flag means **CI stops testing the
configuration that ships**, which is DDR-1040's vacuity trap.

The marginal value is also low *here*: W^X including the identity alias
(DDR-1046), SMEP (DDR-1040) and SMAP (DDR-1041) are shipped and green, and they
remove the primitives KASLR only makes harder to aim.

**Physical-only KASLR** (randomise `KERNEL_PHYS`, keep `KERNEL_VIRT`) needs no
relocations and leaves RIP resolution intact — recorded as the cheaper buildable
variant. But it defends a physical-write primitive DDR-1046 has just removed by
making the alias RO+NX.

**Revisit once OPEN-1/2/12/13 close.** **Not claimed:** that KASLR is
unimportant, or that this kernel is hardened without it.

---

## SECTION 5 — DEFERRED FEATURES

### 5.1 — Pre-approved exceptions (CLAUDE.md §PRE-APPROVED EXCEPTIONS)

These were approved for deferral in advance. All are logged in
`docs/BUILD_TRACKER.md`; reasons reproduced here unchanged. **None requires an
operator decision before user testing** — they were already decided.

| Item | Why deferred |
|---|---|
| Intel HDA audio | optional — no QEMU HDA path in CI |
| Wayland/wlroots compositor | superseded by the shipped custom C framebuffer compositor |
| CMake/Makefile hybrid | post-1.0, awaiting operator sign-off (DDR-843) |
| Apple Silicon / m1n1 | post-1.0 — the aarch64 ISO uses the U-Boot path |
| `arch/aarch64` full port | **boot-only scope per ADR-034** — the ISO uses the boot-only kernel |
| `arch/riscv64` full port | boot-only scope per ADR-034 |
| Cloud bridge activation | post-1.0 (DDR-793) — a security-posture change, not a feature toggle |
| `ACTION_BROWSE_WEB` | needs the cloud bridge above; outbound egress from an agent-capable OS is a posture decision |
| `ACTION_CAPTURE_FRAME` | post-L7, no hardware path |
| `ACTION_SCAN_ENVIRONMENT` | post-L7, needs SLAM3R |
| `ACTION_QUERY_SCENE` | post-L7, no scene graph |
| `ACTION_PARSE_DOCUMENT` | needs a 64 MiB OCR model; no model-shipping path exists |
| `ACTION_EXEC_CODE` | needs a sandboxed interpreter subsystem |
| `CAP_OCR` / `CAP_SCENE` | capability bit defined, enforcement deferred — no subsystem behind it |
| SFS on-disk free-tree | in-memory reclaim shipped (DDR-762-v2); on-disk persistence post-1.0 |
| NVMe completion IRQ | poll-mode is sufficient for the ISO (DDR-774a/b/c) |
| Rust rewrite | not in scope |

**Multi-arch is the one worth reading twice.** ADR-034 scoped aarch64 and
riscv64 as **boot-only**, and DDR-999 assessed full parity and concluded it is
**not achievable** in this timeframe. The ISOs for those architectures package a
kernel that boots and does not run userspace. If the release is described to
users as "multi-architecture", that description must carry the boot-only
qualifier. **That is a wording decision, and it belongs with §1.2.**

### 5.1b — Two scope confirmations from the operator (the `CLAUDE.md` contradiction is now RESOLVED)

**Rust rewrite: deferred to a future release.** The project stays in C for v1.
This matches the existing `[DEFERRED: not in scope]` entry; no conflict.

**Quantum layer: RESOLVED 2026-09-02 — operator decision on PR #17, Part A.**
This item was previously flagged as an unreconciled contradiction between the
operator's statement and `CLAUDE.md` §PHASE 3. It is now settled, and settled in
a more precise way than "no quantum scope exists":

**Quantum *hardware* integration is WITHDRAWN — a speculative future-research
note, not a backlog item.** The operator's reason is architectural, not
priority: quantum hardware is reached over a remote cloud API with queue-time
latency of seconds to minutes, against a kernel that schedules at microsecond
scale, so no version of it improves this OS's own efficiency. `CLAUDE.md`
§PHASE 3 has been rewritten to say so; `docs/BUILD_TRACKER.md` row 10 is marked
WITHDRAWN. Nothing was ever built toward it — no `SYS_QPU_*`, no `CAP_QUANTUM`,
no `smoke-qpu*` — so nothing is unwound. **The two sources no longer disagree.**

**Post-quantum *cryptography* is the opposite ruling, and it is MANDATORY v1
SCOPE.** Same comment, Part B, which explicitly supersedes the earlier
sequencing. Build against NIST's finalized standards — **ML-KEM (FIPS 203)** and
**ML-DSA (FIPS 204)** — to the same bar as everything else here:
mutation-checked, zero warnings, a real non-vacuous gate. Candidates named by the
operator: ML-DSA-signed tamper-evident ledger (F#76's audit chain), PQC-signed
capability tokens, ML-KEM key exchange for `SEND_IPC` if agent messaging ever
crosses machines.

**If it cannot be built to that bar in the time available, the required output is
an explicit, specific blocker** — the DDR-1038 shape, which named exactly why
`SYS_FUTEX` is unbuildable here (no `CLONE_VM`, no `MAP_SHARED` file backing,
fork COWs everything writable, so the two sides would wait on two different
physical words). An honest blocker is an acceptable outcome; filler is not.

**Queue position, from the operator, explicit:** OPEN-2 → the rest of this
checklist → `RUN_EXPERIMENT`-adjacent + Groups A–F → **PQC** → ISO, `main`
promotion, `v1.0.0` tag. PQC lands *before* the ISO is built, not after.

**STATUS: this sub-item is CLOSED as a contradiction and REOPENED as scope.**
It is no longer a docs discrepancy; it is a feature with a queue position.

#### 5.1b.1 — PQC pre-assessment: four facts, measured 2026-09-02

Gathered now rather than at queue position 4, because one of them changes the
whole gate design and finding it late would waste the build.

**1. There is no SHA-3 anywhere in the tree.** `kernel/crypto/` holds SHA-256,
SHA-512, X25519, Ed25519, HKDF, AEAD — and zero Keccak / SHAKE / SHA-3.
**Both** ML-KEM and ML-DSA are built on SHAKE128/256 for expansion, sampling and
hashing, so a Keccak-f[1600] core is a *prerequisite*, not an extra. It is
bounded work (~200 lines plus round constants) but it is the first thing.

**2. Size is not the constraint.** `kernel.bin` is 1,175,946 B against the
1,572,864 B gate — **396,918 B of headroom**. ML-DSA-44 code plus its tables fit
comfortably. (Signature 2420 B, public key 1312 B, so the *data* is small too.)

**3. Authoritative sources are reachable, by exactly one route.**
`raw.githubusercontent.com` serves arbitrary public files (measured: 200,
13,617 B for the Dilithium reference `sign.c`). The GitHub **API** is scoped to
this repository and returns 403 for others, and **csrc.nist.gov is blocked by the
proxy entirely** (403 CONNECT / no route). So reference code and test vectors must
come through `raw.githubusercontent.com`, and any path must be probed rather than
assumed — several plausible KAT paths returned 404 while a known file returned
200, i.e. the repo layout is not what a guess produces.

**4. THE GATE MUST BE KNOWN-ANSWER, NOT ROUND-TRIP. This is the load-bearing
constraint and it is why this note exists.** A gate that signs a message and
then verifies the signature **passes on a completely wrong implementation** —
any self-consistent homegrown scheme satisfies sign→verify, including one that
is not ML-DSA at all and offers none of its security. That is precisely the
dead-arm class this project has now hit nine times: a field whose only reachable
value is the passing one.

Conformance to FIPS 204 can only be shown against **fixed known-answer
vectors**: a pinned seed and message, with the expected public key, secret key
and signature (or their hashes) committed to the repository and asserted
byte-exact — the same shape DDR-1044 arm D used for the machine-check bank
decode, and for the same reason.

**Consequence for planning:** the build order is Keccak/SHAKE → KAT acquisition
and pinning → ML-DSA → the application (ledger signing / capability tokens). If
the KAT step cannot be completed, the honest output is a DDR-1038-shaped blocker
naming *that* — not an unverified implementation with a round-trip gate.

### 5.2 — Group F: AETHER agent behaviours

**The structural fact first, because the tracker got this wrong twice
(DDR-1022):** there is exactly **ONE** agent program, `user/agent_base.c`. The
roster is generic active-bits, and a slot is filled by `SYS_SPAWN_AGENT`
launching that template with a task. The kernel holds no per-agent identity. So
*"11 unbuilt agents"* means **11 domain behaviours, not 11 programs** — and a
stub would gate vacuously.

Also corrected by DDR-1022, having been listed as unbuilt when they were not:

- **F#68 metric lockbox** — **SHIPPED + GATED** (`smoke-lockbox`, shard 7,
  strict, DDR-812).
- **F#76 tamper-evident ledger** — **SHIPPED + GATED TWICE**
  (`smoke-auditchain` shard 0 + `smoke-auditchain-tamper` shard 4, both strict).

Deferred behaviours: F#66 `architect_agent`, F#67 `healer_agent` (RUFLO), F#69
`inventor_agent`, F#70 `tournament_agent`, F#71 subconscious world model, F#72
`verifier_agent`, F#75 lineage memory — each a behaviour with no subsystem
behind it; F#75's gate would duplicate `smoke-agentmem`.

**F#74 capability discovery is blocked differently, and deliberately.**
`agent_caps` exists on `struct tcb` (DDR-982) but is initialised to 0, never
granted, and has no syscall to read it. Building it **reverses DDR-982's
deliberately withdrawn per-slot enforcement**. That withdrawal was a decision,
so un-deferring F#74 is also a decision — but it is post-1.0 either way, so it
is not in Section 1.

**Section 3C action types close at 6 shipped + 2 deferred + 0
buildable-and-unbuilt**, and that tally has been wrong twice (DDR-1017 said
"3 of 8", DDR-1018 said "4 of 8"; both were wrong because
`ACTION_SPAWN_PROCESS` is not one of the eight and `ACTION_REWRITE_AGENT_CODE`
was already gated by DDR-842). Shipped: `READ_FILE`, `DELETE_FILE`,
`QUERY_MEMORY`, `REWRITE_AGENT_CODE`, `PROPOSE_HYPOTHESIS`, `EVOLVE_GENOME`.
`SEND_IPC` — see §3 and §4.1. `RUN_EXPERIMENT` — see §3.

### 5.3 — Unbuilt backlog by group

Measured 2026-09-01 by grepping the Makefile for each named target, because
declaring something unbuilt without grepping has been wrong four times in this
project. **MISSING = no such target exists.** Every row here is post-1.0 work,
none blocks user testing, and none requires an operator decision.

**Group A — kernel completeness (all MISSING):** `smoke-ioapic` (I/O APIC
migration, DDR-714 stage D), `smoke-smep` (SMEP/SMAP `CLAC`/`STAC`), `smoke-wx`
(kernel W^X identity-alias removal, DDR-757 residual), `smoke-mc` (`#MC`
handler), `smoke-kaslr`, `smoke-lockstat`, `smoke-schedtimeout`.

> **Two Group A rows are ALREADY BUILT and must not be rebuilt.** The
> demand-paged user stack is **ADR-038** (`vmm_stack_fault`, `vmm_cow.c:144`;
> `USER_STACK_EAGER_PAGES = 8` is measured, not guessed), gated by
> `smoke-stack-demand` — `smoke-lazystack` does not exist. And "scheduler
> timed-block" is **DDR-955**: the shipped call is `sched_block_timeout()`
> (`sched.c:1434`) with four callers. The genuinely unbounded wait is elsewhere
> and is tracked as DDR-994: `mnt_lock` (`vfs.c:25`) is a bare
> `while (exchange(&m->busy,1)) yield();` with no deadline.

**Group B — storage (`smoke-sfs-persist`, `smoke-sfs-gc`, `smoke-numa` EXIST;
the rest MISSING):** `smoke-sfs-boot-root` (provisioned SFS as default boot
root — this one blocks the Group F audit-persistence row), `smoke-sfs-largefile`,
`smoke-sfs-deepslot`, `smoke-sfs-quota`, `smoke-ext4-write`, `smoke-nas`,
`smoke-pmmpolicy`, `smoke-nvmeirq`.

**Group D update (DDR-1037):** `smoke-poll` now EXISTS — `SYS_POLL` (NSI 102)
shipped, mutation-checked M1/M2/M3 on distinct kernel hashes. Two limits recorded
rather than papered over: **console `POLLIN` is unimplementable here** (this
kernel has no console-input predicate under any name, so `poll()` on stdin can
only report not-ready), and **`timeout < 0` has no gate arm** (blocking forever
needs a second process to unblock it; the probe is single-threaded). It also
**fixed a correctness bug in `epoll_wait()`**: the shared readiness predicate
returned 0 for `FD_VFS`, so epoll on a regular file was answering wrongly —
POSIX says a regular file never blocks.

**Group C — networking (all MISSING):** `smoke-epoll` (proxy-socket
epoll/select), `smoke-udp`, `smoke-netrevoke` (`SYS_NET_REVOKE` / CAP_NET policy
reload), `smoke-tap`, `smoke-ipv6`, `smoke-tls`.

**Group D — userspace (all MISSING except as noted):** `smoke-readline`,
`smoke-pipes`, `smoke-poll`, `smoke-futex`, `smoke-pthreads`, `smoke-mmap6`
(6-arg `mmap` ABI), `smoke-mmap-file`, `smoke-dynlink`, `smoke-iouring`,
`smoke-sigaction`, `smoke-prism-ls`, `smoke-jobctl`.
**Shipped since the backlog was written:** `SYS_MPROTECT` (DDR-1031,
`smoke-mprotect`) and `execve` argv/envp (DDR-1032, `smoke-execve-argv`).
**PRISM `run` was never disabled** — DDR-973 §7; what ADR-024 §D5 deferred is
narrower: init-driven fork+execve *respawn* of PRISM.

**Group E — compositor (`smoke-alttab`, `smoke-perrestore`, `smoke-horizon`
EXIST):** `smoke-maximize` and `smoke-sharedpte` are MISSING. Maximize at real
display size shipped as DDR-1007 under a different gate name; `smoke-sharedpte`
is DDR-1003 §5.1's unbuilt gate — see §4.5, and note the warning there that the
obvious shape would pass vacuously.
**Shipped since:** all-edge resize (DDR-997), `SURF_EV_CLOSE` (DDR-998),
per-window dock restore (DDR-1008), horizon bands (DDR-1012), Ctrl+Alt+T
terminal (DDR-1027). The **vDSO callable reader row was a false gap** — DDR-1005:
the ring-3 reader exists and is gated by `smoke-vdso` (shard 7, strict); only
`vdso_entry.asm` is unbuilt, and that is a security-posture change (the user view
is deliberately `VMM_NX`), not a checkbox.

**Group F gates (all MISSING):** `smoke-auditpersist` (needs the SFS boot root
above), `smoke-agentexec`, `smoke-agentconc`, `smoke-rosterctd`, `smoke-capocr`,
`smoke-capexec`, `smoke-capscene`, `smoke-capnetbrowse`, and the five
spawnability gates that depend on them. `smoke-agentmetrics` EXISTS.

**Group G — assembly optimization (6 of 7 items unstarted):** hot-path `kputc`,
context-switch critical path, TLB shootdown batching, virtio-blk doorbell
batching, IPC fast path, page-table walker SIMD. Each requires **profile first**;
none has a baseline, so none has a claim attached to it. Per §NON-NEGOTIABLE 17,
a performance claim needs a denominator — there is nothing to report yet.

**Group H — release:** `smoke-iso-aarch64`, `smoke-iso-riscv64`, and
`smoke-prad` (the `prad` package manager, NSI 88–90 — **not** 87, which is
`SYS_READ_AUDIT`) are MISSING. The x86_64 ISO is built and verified.

### 5.4 — Gates excluded from the CI shard matrix

**7 excluded, each with a stated reason in `tools/ci/shard_check.sh`.** An
unexplained exclusion is how a gate goes missing, so they are reproduced here.

| Gate | Reason |
|---|---|
| `smoke-aarch64`, `smoke-riscv64` | run by the `arch-bootstrap` matrix job, not `build-and-boot` |
| `smoke-selftest` | DDR-785 self-tests the boot harness. It must run BEFORE any gate that trusts the harness, so it is a setup step in **every** shard rather than one gate in one shard |
| `smoke-sfs-btree-smp4` | DDR-824 OPEN-10 reproduction surface. Registering it now would make CI red on a known-open defect and block unrelated promotions. **Register it when OPEN-10 is fixed** — DDR-964 fixed the cause; this is waiting on promotion evidence, not on work |
| `smoke-agent-live` | developer-run only: needs a live Ollama endpoint on the host, so CI stays in test mode (ADR-027) |
| `smoke-fs-liveness` | DDR-777/BUG-1 diagnostic. It rebuilds the kernel with `BSP_LIVENESS=1`, whose churn fills the 4 KiB dmesg ring and evicts `smoke-dmesg`'s required marker (DDR-790) |
| `smoke-fast` | **not a gate** — a runner that invokes another gate N times via `campaign.sh`. Excluded as infrastructure, not as a skipped test |

---

## SECTION 6 — RELEASE-GATE STATE

Re-measured 2026-09-03 at `32cb8ad`. **Re-measure rather than increment** — the gate count has been wrong three times.

| Quantity | Value | Source |
|---|---|---|
| Gates assigned | **173** across **10** shards | `make ci-shard-check`, re-measured 2026-09-03 |
| Gates excluded | **7**, each with a reason | §5.4 |
| NSI max | **102** (`SYS_POLL`, DDR-1037), next free **103**, table size 128 | `kernel/syscall/syscall.h:181` |
| DDR free range | **DDR-1050+** | §INV.4 |
| `kernel.bin` | **1,175,946 B** against the 1,572,864 B gate — 396,918 B headroom | measured at `32cb8ad` |
| Warnings at `-Werror` | **zero** | `make image` |
| x86_64 ISO | built, BIOS + UEFI arms verified, **boots a live OS** | `smoke-iso-userspace` |
| aarch64 / riscv64 ISO | **boot-only scope** (ADR-034) — see §5.1 | DDR-999 |
| `v1.0.0` | **untagged, held** | §1.2 |
| `main` promotion | **unstarted** | §1.2 |

---

## SECTION 7 — WHAT THIS DOCUMENT DOES NOT DO

It does not close anything. Specifically:

- It does not turn OPEN-1, OPEN-2, OPEN-12 or OPEN-13 into resolved issues. Each
  is waiting on evidence, and three of the four have armed instruments whose
  whole purpose is that the *next* occurrence is diagnostic rather than another
  colour-match.
- It does not claim the deferred items are unimportant. It claims they are
  **written down and decidable**, which is the difference between a deferral and
  a gap.
- It does not substitute for the operator decisions in Section 1. Those two rows
  — branding/licensing, and the `v1.0.0` hold — are the reason this document was
  asked for, and neither can be answered by an implementer.

**Update this file in the same commit as any change to what it records.**

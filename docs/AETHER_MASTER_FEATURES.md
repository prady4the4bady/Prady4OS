# PradyOS AETHER — Master Feature Tables

**Canonical, single source of truth for feature state.** No competing feature list
may exist anywhere in the repo. Mirrored into `SESSION_HANDOFF.md` (task tracker)
and `docs/build_status.md` (gate count/header) in the SAME commit as any code
touching agents, UI, sockets, storage, namespaces, telemetry, scheduling, or
capabilities.

**Status vocabulary:** `shipped` (CI-green today) · `active-slice` (a DDR/ADR is
open and code is in flight) · `planned` (tracked, not started) · `proposed`
(designed, prerequisites not yet answered).

**Last verified against repo:** 2026-08-03, `dev/phase1` @ `1e40464` (tree clean); `main` @ `b823bb5`.
**Corrections this session** — these were recorded as absent and are not:
X25519 (`kernel/crypto/x25519.c`, host vectors pass), SHA-512
(`kernel/crypto/sha512.c`, gate `smoke-sha512` A/B-verified), and the
aarch64/riscv64 bootstraps (both green in CI every run). Full itemisation and
the corrected phase counts live in `docs/BUILD_TRACKER.md`.

**(previous)** 2026-08-02, `main` @ `b823bb5` — two consecutive
CI greens on the exact tip (run 30733620093, attempts 1 and 2). That promotion
carried **DDR-816** (kernel entropy), **DDR-818** (HMAC-SHA256 + HKDF-SHA256) and
**DDR-819** (ChaCha20-Poly1305) in one cycle, on top of DDR-811 (SHA-256),
DDR-812 (metric lockbox) and ADR-035 (bounded W^X carve-out).

**(previous)** 2026-07-27, `main` @ `0d3f2ab` (DDR-787),
113 steps green (CI run 30211536949, conclusion `success`, 105.7 min) — promotes
**blocking pipe semantics**: pipelines no longer depend on scheduling luck
(premature EOF) and no longer truncate at 4 KiB. The big-pipe gate
(`cat /BIG8K.TXT | cat`, ≥180 of 200 payload lines) passed in CI at its tighter
pacing with no wedge.

**(previous)** `main` @ `a87d6ee` (DDR-786), run 30206856237 — multi-stage
pipelines `a|b|c`.

**(previous)** `main` @ `ebd708d` (DDR-785),
113 steps green (CI run 30200918063, conclusion `success`) — promotes DDR-784
(PRISM stderr + `2>`) and DDR-785 (`boot_test.sh` early exit), on top of DDR-782
(kernel `O_TRUNC` + atomic `O_APPEND`), ADR-033/DDR-779 (musl mirror) and DDR-783.
That run also **measures DDR-785's effect: 105.8 min vs the 152.3 min baseline
(run 30193738689) — 46.5 min saved**, matching the ~43 min predicted from the
timeout budget.

---

## Section A — Kernel/OS shipped features (CI-green; do not re-litigate)

All entries below are **shipped**.

### Boot / Kernel / Drivers / Filesystem
- **ADR-038 demand-paged user stack** — eager window `USER_STACK_EAGER_PAGES=8`,
  `#PF`-driven growth, guard page below `USER_STACK_BOT` (gate: `smoke-stack-demand`).
  SHIPPED 2026-08-20: three consecutive CI greens on one SHA `bb19449` —
  runs 32395650883, 32402456422, 32405858449. `main` fast-forwarded to `bb19449`.
- **DDR-955 `sched_block_timeout`** — bounded-wait primitive: TCB `block_deadline`/
  `wake_timed_out`, expiry scan in `sched_tick` over the `->next` ring under
  `irq_save()`, waking via `sched_unblock` outside the walk (gate:
  `smoke-blk-timeout`, 20/20, forbidden-sentinel arm on both timeout strings).
  **PARTIAL — 2 of 4 sites bounded.** virtio-blk slot-wait and compl-wait at 500
  ticks are shipped. `ipc_recv` and `bcast_wait` are deliberately NOT bounded:
  `bcast_wait` returns `void` and fills an out-parameter, so an early timeout
  hands the caller an unfilled buffer it cannot detect, and `ipc_recv`'s `-1`
  already means "cap denied". Both need a return-value contract first; measured
  at 19/20 with two lwIP `#GP` panics before revert.
- MBR two-stage boot → long mode → ring-0 C (gate: `smoke`)
- Kernel relocated to 4 MiB, 768 KiB load window (DDR-733)
- GDT/IDT + exception panic path; 8259 PIC ISA-only; LAPIC/APIC timer @100 Hz (DDR-714A)
- Buddy PMM, SLAB heap, higher-half VMM, per-process CR3, W^X + NX (ADR-003/007/021)
- **Kernel self W^X** — `vmm_protect_kernel()` re-stamps kernel text RX / data NX after boot (DDR-757; gate: kernel-self W^X PTE audit) *(was mis-tracked as Section B#7 until 2026-07-26)*
- **COW fork** — `vmm_fork_address_space_cow()` + `PTE_SW_COW` + PMM refcounts + `vmm_cow_fault()` in the #PF path (IMP-D; gate: `smoke-cowfork`) *(was mis-tracked as Section B#5 until 2026-07-26)*
- **SHA-256 kernel primitive** (DDR-811) — `kernel/crypto/sha256.c`, pure C,
  no hardware acceleration so the same source builds for x86_64/aarch64/riscv64.
  Validated against four FIPS 180-4 vectors incl. 1M `a`; gate `smoke-sha256`.
  Prerequisite for DDR-812, §J-03, ACC and AGS.
- **HMAC-SHA256 + HKDF-SHA256** (DDR-818) — `kernel/crypto/hkdf.{c,h}` on
  DDR-811. Three RFC 5869 vectors; TC2 (82-byte OKM) forces the expand loop
  past T(1) and TC3 covers the NULL-salt branch (HashLen zero bytes, not an
  empty string). Gate `smoke-hkdf`. Not yet in the kernel link — first caller
  is DDR-813. First of ACC's four missing primitives (819/820/821 follow).
- **SHA-512** (DDR-821) — `kernel/crypto/sha512.{c,h}`, FIPS 180-4. Its own
  file, not a parameterised `sha2.c`: 64-bit words, 128-byte blocks, 80 rounds,
  a different constant table, four different rotation triples and a 128-bit
  length field. Four vectors; the 112-byte case (112 = 128-16) is the only one
  whose message ends exactly where the length field goes, and the 1M-`a` case is
  streamed in 1000-byte chunks so the partial-block carry actually runs.
  Gate `smoke-sha512`, A/B-verified. Not in the kernel link — first caller is
  DDR-821 Ed25519.
- **DAG action queue** (DDR-839, Section E) — ✅ SHIPPED, CI-confirmed
  (`PASS smoke-actiondag (120s)`, run 31043474501 on `362cb36`). `SYS_SUBMIT_CHILD_ACTION` (92)
  adds `parent_action_id` to the queue: an agent can plan "fetch, then report"
  up front and the kernel refuses the second until the first is approved.
  **Cycles are structurally impossible rather than checked** — ids are monotonic
  and never reused, and a parent must already exist to be named, so
  `parent_action_id < action_id` always holds. There is no cycle-detection code
  because none is reachable; that is a property of the id allocator, so anyone
  making ids reusable must revisit it. Enforced in BOTH the approval path and
  the sovereign-mode auto-approval path — the gate caught that guarding only
  `aether_approve` left the common path unguarded. A rejected parent needs no
  cascade code: its children simply never pass the check.
  `smoke-actiondag` 20/20 locally, then CI-green.
- **Spawn-depth cap** (DDR-838, Section E) — ✅ SHIPPED, CI-confirmed
  (`PASS smoke-spawndepth (120s)`, run 31028810861 on `83a761a`). `SPAWN_DEPTH_MAX = 3`, enforced in
  `SYS_FORK`. An agent may found a chain three generations deep; the fourth is
  refused with `-EAGAIN`. **Keyed on lineage (`tcb.agent_depth`), not on
  `is_agent`**: fork deliberately does not inherit authority flags, so a cap
  conditioned on `is_agent` would be escaped by one fork — the agent's child is
  not an agent, and everything below it forks freely. The cap would have passed a
  one-level test and contained nothing. `-EAGAIN` rather than `-EPERM` because
  the caller is permitted to fork and has hit a ceiling. Scoped to agent
  lineages, so `init` -> `prism` -> a command are all depth 0 and unaffected
  (`smoke-shell` 5/5 locally and `PASS smoke-shell (60s)` in the same CI run).
  `smoke-spawndepth` 20/20 locally, then CI-green.
- **Code-rewrite approval** (DDR-842, Section E) — ✅ SHIPPED, CI-confirmed
  (`PASS smoke-coderewrite (120s)`, run 31094358972 on `93b51ea`).
  `SYS_APPROVE_CODE_REWRITE` (86)
  requires `CAP_REWRITE` **and** `CAP_SOVEREIGN`. Sovereignty alone is refused, so
  the bit is not decoration; and only `ACTION_REWRITE_AGENT_CODE` may be approved,
  so the call cannot become a general approval bypass. 20/20 local, then CI-green.
- **Tamper-evident audit** (DDR-842, S5) — ✅ SHIPPED, CI-confirmed
  (`PASS smoke-auditchain` + `PASS smoke-auditchain-tamper`, run 31094358972).
  Each entry carries
  `SHA-256(prev || fields)`; `SYS_VERIFY_AUDIT` (93) recomputes the chain and
  reports the INDEX of the first mismatch. Append-only removed the user-space
  write path and said nothing about the bytes on the page; only a recomputable
  chain distinguishes an intact log from an edited one. **Honest limit:** the log
  is circular, so verification covers the retained window — a wrap is a real gap
  in the chain of custody, which is why `AETHER_AUDIT_WRAP` is emitted. The
  durable ledger that survives wrap is F#76 and is not claimed here.
  Building this found a live kernel heap overflow: the ring was allocated 128 KiB
  for a 256 KiB structure. 20/20 local on both arms, then CI-green.
- **Agent checkpoint / resume** (DDR-837, Section E) — ✅ SHIPPED, CI-confirmed
  (`PASS smoke-checkpoint (120s)`, run 31015668039 on `35bab14`). `SYS_CHECKPOINT_AGENT` (84)
  / `SYS_RESUME_AGENT` (85), both CAP_SOVEREIGN. The operator freezes and thaws a
  running agent. **The target blocks ITSELF** at its next syscall boundary, on a
  `tcb.checkpointed` flag observed in `syscall_dispatch`; marking THREAD_BLOCKED
  from the checkpointing CPU would race the scheduler for a thread that may be
  RUNNING elsewhere mid-kernel-work. The honest cost is that a checkpoint is not
  instantaneous — an agent in a long pure-computation loop runs until it calls
  something. Guards: init (pid 1) and self cannot be frozen, and an unknown pid
  is `-ESRCH`, distinct from `-EPERM`. `smoke-checkpoint` 20/20 locally, then
  CI-green.
- **Agent memory** (DDR-836, Section E / 3B) — ✅ SHIPPED, CI-confirmed
  (`PASS smoke-agentmem (120s)`, run 31003118400 on `7f7a9d3`). `SYS_MEMORY_WRITE` (82) /
  `SYS_MEMORY_READ` (83), gated by `CAP_MEMORY` (1<<18) with a `tcb.is_memory`
  flag following the CAP_NET precedent. A bounded key/value store (64 records,
  32-byte keys, 256-byte values) for facts agents keep across actions.
  **It is a SHARED BLACKBOARD, not per-agent storage**: any CAP_MEMORY holder can
  read and overwrite any key, and the capability is the entire boundary. Keying
  records by owner pid was rejected because pids recycle — a new agent would
  inherit a dead one's memories, which is worse than no isolation because it
  looks like isolation. Durable per-agent identity arrives with the roste
  (Section G). `smoke-agentmem` 20/20 locally, then CI-green.
- **Secure credential vault** (DDR-834, table 6.9) — ✅ SHIPPED, CI-confirmed
  (`PASS smoke-vault (120s)`, run 30993915008 on `b2e7836`). `SYS_VAULT_PUT` (87) /
  `SYS_VAULT_GET` (91), both CAP_SOVEREIGN. Cloud-bridge credentials encrypted at
  rest in SFS at `/VAULT.BIN`: `K_vault = HKDF-SHA256(owner_seed, "PRADYOS-VAULT-v1")`,
  each record sealed with ChaCha20-Poly1305. **The record name is the AEAD's
  additional authenticated data** — without that a record could be renamed on
  disk so a request for one credential returned another, while every tag still
  verified. Confidentiality of the values is not integrity of the association.
  A CAP_AGENT get was considered and rejected: it would let one compromised agent
  drain the vault, the exact blast radius the vault bounds. `smoke-vault` 20/20
  locally, then CI-green.
- **ACC session rotation** (DDR-815) — ✅ SHIPPED, CI-confirmed
  (`PASS smoke-acc-rotate (120s)`, run 30966987476 on `7be4b65`).
  `SYS_ACC_ROTATE` (81, CAP_SOVEREIGN).
  Replaces ACC's single global replay floor with a per-agent channel table keyed
  by the agent's Ed25519 verify key — authenticated and carried in-band, unlike a
  pid, which is recycled and would let a new process inherit a dead agent's
  replay floor. Rotation is REVOCATION: it raises the floor past any issuable
  sequence and keeps the slot as a tombstone. Clearing the floor to zero (my
  first implementation) would have made every pre-rotation envelope acceptable
  again — the exact replay hole the syscall exists to close. `smoke-acc-rotate`
  20/20 locally, then CI-green.
- **AGS — Agent Goal Signing** (DDR-814) — ✅ SHIPPED, CI-confirmed
  (`PASS smoke-ags (120s)`, run 30960084022 on `9fb8ea4`). `kernel/aether/ags.{c,h}` + `kernel/syscall/sys_ags.c`
  + `SYS_GOAL_SIGN` (79, CAP_SOVEREIGN) / `SYS_GOAL_VERIFY` (80, CAP_AGENT).
  `sig = Ed25519(owner_seed, SHA-256(goal))` — the goal is hashed first so any
  goal length costs one fixed-size signature and the audit log can store the
  32-byte hash rather than the record. The capability split is the INVERSE of
  ACC's: signing AUTHORISES a goal, so if a ring-3 agent could sign it could
  authorise its own goals and the audit log would show a valid signature on
  something nobody approved (S1). Verification touches only public data, so
  agents may check the goals handed to them. New audit codes `AR_GOAL_SIGNED` /
  `AR_GOAL_REJECTED` are kept distinct from `AR_APPROVE`/`AR_CAP_DENIED` because
  a forged goal and a policy refusal are different facts.
- **ACC — Authenticated Confidential Channel** (DDR-813) — ✅ SHIPPED, CI-confirmed
  (`PASS smoke-acc (151s)`, run 30944847959 on `93ceee7`) — `kernel/crypto/acc.{c,h}`
  + `SYS_ACC_SEAL` (77, CAP_AGENT) / `SYS_ACC_OPEN` (78, CAP_SOVEREIGN). The
  capability split is asymmetric on purpose: opening reveals a PEER agent's
  plaintext, so it is owner-only (S1). Envelope: ephemeral X25519 → HKDF
  `ACC-session-v1`/`ACC-owner-v1` → ChaCha20-Poly1305 with `nonce=eph_pub[0:12]`
  → Ed25519 over `eph_pub||ct||tag`. Two spec bugs fixed at design time:
  `agent_sign_pub[32]` travels IN-BAND (without it the owner cannot verify after
  a reboot — the offline read is the entire point), and the Ed25519 and X25519
  keys are distinct fields that never alias. Gate `smoke-acc` PASSES, shard 5.
- **ChaCha20-Poly1305 AEAD** (DDR-819) — `kernel/crypto/aead.{c,h}`, RFC 8439.
  **Gate `smoke-aead` passes** (shard 4): §2.4.2 keystream, §2.5.2 tag over a
  34-byte message (2-byte final block — the only path reaching the short-block
  code), a seal/open round-trip depending on no published constant, and two
  distinct rejection arms (tampered ciphertext, tampered tag).
  Chosen over AES-GCM because the same source must be correct AND constant-time
  on riscv64/aarch64, which have no AES instructions; ChaCha20 and Poly1305 are
  add/xor/rotate and a 130-bit multiply-accumulate, constant-time as a property
  of the *algorithm* rather than of the compiler. `aead_open` verifies the tag in
  constant time and writes **no plaintext** before verifying. Verified against
  RFC 8439 §2.4.2 (ChaCha20 keystream) and §2.5.2 (Poly1305, 34-byte message).
  Not yet in the kernel link — first caller is DDR-813.
  **Known gap, on the record:** the constant-time property is enforced by
  construction and review, *not* by any gate — a constant-time compare and an
  early-exit `memcmp` reject exactly the same inputs, so no black-box QEMU gate
  can distinguish them. Stated in DDR-819 rather than papered over.
- **X25519 key agreement** (DDR-820) — `kernel/crypto/x25519.{c,h}`, RFC 7748.
  Pure C, 5×51-bit field over `p = 2^255-19`, constant-time Montgomery ladder
  with a masked `cswap`. **Status is `active-slice`, not `shipped`:** all RFC
  7748 vectors pass **on the host** — including a commutativity check that
  depends on no published constant, and small-order-point rejection — but the
  in-QEMU gate `smoke-x25519` does not yet reach its sentinel, so it is
  **excluded from the shard matrix with a stated reason** in
  `tools/ci/shard_check.sh`. Ed25519, ACC and AGS are blocked behind it.
  **Known gap (owner decision D-1, on the record):** constant-time execution is
  by construction and review only. No gate this project can run detects a timing
  channel; QEMU under TCG does not model cache or pipeline timing. Production
  use requires side-channel review.
- **aarch64 / riscv64 bootstrap** (ADR-034) — `kernel/arch/{aarch64,riscv64}/`
  each hold `boot.S` + `start.c` + `kernel.ld`. `make kernel-<arch>` builds
  warning-clean and `smoke-<arch>` boots them under QEMU `virt`; **both are
  green in the `arch-bootstrap` CI job on every run.** Scope per ADR-034 is
  **boot-only** — they reach `kmain` and print the banner. No drivers, no FS, no
  userspace, and the smoke-gate set is deliberately not ported. Stated
  explicitly because "the arch ports work" would over-read this.
- **Bounded W^X carve-out** (ADR-035) — the single, bounded exception to the
  ADR-021 W^X invariant, for §E-05 self-rewriting code. Binding; may only be
  superseded by a new ADR.
- **Kernel entropy** (DDR-816) — virtio-rng primary over the existing generic
  transport, RDSEED secondary (x86-only, CPUID-gated), and **no third source**:
  `rng_bytes()` fails and crypto refuses to start rather than falling back to
  jitter, because a source that silently degrades is worse than one obviously
  absent. `rng_source()` names the active source in the boot log every boot.
  Gate `smoke-rng`; arm B (fixed buffer) is what makes it discriminating.
  Unblocks DDR-813/814/815, all of which need keys and nonces.
- **Metric lockbox** (DDR-812, F#68 §S5) — authoritative record inside the
  DDR-795 `metric_page` frame, which ring 3 maps RO+NX, so tamper-resistance
  is a page-table property rather than a path check. SFS was rejected: the VFS
  gates on `CAP_FS_WRITE`, which every `CAP_SOVEREIGN` process holds.
  `SYS_METRIC_READ` (76) verifies SHA-256 before returning any bytes;
  `-ETAMPER` (133) on mismatch. Gates: `smoke-lockbox` + `smoke-metric`
  (the latter already pins the fault address to the record's own offset).
- **Console input integrity** — `kputc` bounds the UART THRE wait
  (`CONSOLE_THRE_MAX`) and drains the RX FIFO inline, so a burst of kernel output
  can no longer destroy concurrent console input (DDR-809; closes OPEN-8 and the
  DDR-807 S2 violation). The RX ring is now **multi-producer under `g_rx_lock`**,
  single-consumer unchanged. Evidence: baseline `4923c1831f2a` `smoke-shell` FAIL
  4/4 with 1 RX loss per run → fixed `4a1dc378c13e` PASS 3/3 with **0 losses**.
  Gate deliberately **absent** per S11 (QEMU cannot back-pressure the UART, so
  "did not hang" would pass against the broken code too); `smoke-shell` is the
  regression test.
- Per-CPU runqueues + work-stealing scheduler (DDR-SMP-rq-1/2/3); SMP bring-up 4 APs (ADR-029/031)
- MSI-X for all virtio devices (DDR-714C, DDR-771); multi-in-flight virtio-blk (DDR-BLK-1)
- NCS capabilities: `CAP_SOVEREIGN`, `CAP_AGENT`, `CAP_NET` (ADR-009, DDR-731)
- NIA IPC sync/async/broadcast (ADR-010/011)
- FAT32 RW + VFAT LFN (ADR-015/020); SFS CoW B+tree, extents, journal, LZ4, snapshots (ADR-018)
- SFS hierarchical dirs (DDR-738), unlink/rmdir (DDR-741), free-space GC (DDR-762-v2)
- SFS write-budget token bucket 25 MiB/s (ADR-032); cross-reboot persistence (DDR-768/769/770)
- Host `mkfs.sfs` tool (DDR-767) + multi-leaf B+tree bulk load (DDR-773); ext4 read-only (ADR-019); per-process root mount (DDR-739)
- NVMe controller + block I/O (DDR-765/766); NVMe PRP2/PRP-list (DDR-772); `VBLK_MAX` 4→8 (DDR-771)

#### CPU mitigations, machine check, and kernel memory protection (2026-09)

- **SMEP (DDR-1040)** — `CR4.MEP` (bit 20), CPUID-guarded, set on the BSP and on
  every AP. Ships `kernel/fault_expect.h`, a one-shot RIP-windowed expected-fault
  latch, without which enforcement is unobservable (a ring-0 `#PF` is fatal here —
  `idt.c` has no fixup table — so "the CR4 bit is set" would be the only assertable
  claim). **The vacuity trap was measured first:** the TCG default `qemu64` reports
  `smep=false`, so a correct CPUID-guarded implementation is a permanent no-op on
  the CPU every gate runs on. `smoke-smep` therefore pins its own
  `-cpu qemu64,+smep`, and arm E re-boots on the default model to assert the
  no-op path. M2 fails arm B alone; **M3 passes every arm — the latch's RIP-window
  check is measured-uncovered, not assumed-covered.** Gate: `smoke-smep` (shard 5).
- **SMAP (DDR-1041)** — `CR4.MAP` (bit 21). `stac`/`clac` are a **runtime branch**
  on `g_smap_on`, never unconditional (both are `#UD` without SMAP). The window
  opens *after* `vmm_user_range_ok`, and in `copyinstr` wraps the **single byte**
  rather than the loop, so AC is never held across the page-boundary revalidation.
  **The enumeration was measured, not grepped:** SMAP on with no `stac` anywhere,
  and a full boot is line-for-line equivalent to baseline (416 vs 418 lines) with
  19 user-pointer-dense gates all rc=0 — `uaccess.h`'s contract HOLDS and no `stac`
  was needed outside `uaccess.c`. **NOT FIXED and named:** an interrupt taken
  between `uaccess_begin` and `uaccess_end` runs its handler with **AC still set**
  (the CPU clears IF on an interrupt gate, not AC), so SMAP is off inside that
  window. Left alone because fixing it means touching `isr_common`, load-bearing
  for DDR-981/1006/1010 and the still-open OPEN-2. Gate: `smoke-smap` (shard 0).
- **`#MC` machine check (DDR-1044)** — `idt.c` already knew vector 18 and already
  panicked with a register dump; **the real defect was upstream: with `CR4.MCE`
  clear a machine check raises no exception at all.** QEMU says so outright
  ("MCE capability is not enabled, raising triple fault") and the serial log stops
  mid-boot with no banner — on real hardware, a box lost to a memory fault with
  zero diagnostic. Ships SDM §15.8 init in order, `MCi_STATUS` cleared (a stale
  firmware VAL would make the first `#MC` report a pre-boot fault), `CR4.MCE` last
  and read **back**. `MCG_STATUS` prints *before* the register dump because RIPV is
  how a reader knows whether to trust the RIP that follows. **NOT DONE:** no
  recovery — every `#MC` is fatal, including a RIPV=1 restartable one; no CMCI or
  correctable-bank polling; the AP path is compiled but unexercised.
  Gate: `smoke-mce` (shard 4).
- **Kernel W^X through the identity alias (DDR-1046)** — the kernel image is
  mapped **twice**, and only one copy was protected. DDR-757 set NX on the 2 MiB
  identity PD entry and **left RW** ("RW kept (documented residue)"), so kernel
  text was writable through a physical address. **And the audit could not see it:**
  `vmm_protect_kernel`'s verdict loop walks only the higher-half PTEs, so it
  printed `[wx] kernel W^X OK` on a kernel with writable text — the blind spot is
  why the residue survived. The entry is now read **back** and printed
  (`PRADYOS_WX_ALIAS present=1 rw=0 nx=1`), because a measurement showing only that
  nothing crashed cannot distinguish "the alias is RO" from "the write-protect
  never applied". **M1 is literally the pre-fix tree** and now fails through the
  AUDIT, not merely the new sentinel. **NOT DONE:** the alias is made RO+NX, **not
  removed** — unmapping it rests on the page tables staying in a different 2 MiB
  page, a property of today's layout rather than a guarantee.
  Gate: `smoke-wxkernel`.
- **Spinlock contention accounting `lock_stat` (DDR-1047)** — contention counts and
  **wait time**; hold time is **deliberately dropped**, because contention and wait
  are observable entirely in the slow path (an uncontended acquire pays nothing)
  whereas hold time needs an `rdtsc` pair on every acquisition of the kernel's
  hottest primitive — and OPEN-2 is a timing-sensitive AP freeze that such a probe
  could *move* rather than measure. Side table, not fields in `spinlock_t` (that
  struct is one byte, is embedded in others, and `percpu.h`'s offsets are
  assembly-visible and static-asserted). M1 forced-proof: `g_sched_lock` dominates
  by **~450x** (1,902,380 hits vs 4,269 for the busiest runqueue lock). **NOT
  GATED, deliberately** — the dump prints only on `[apfreeze]`, which is in
  `GLOBAL_FORBIDDEN`, so any assertion on it is unreachable on a green run.
  **NOT COVERED:** `mnt_lock` (`vfs.c:34`) is not a `spinlock_t` at all but a
  sleep-mutex over a bare byte, so the one lock DDR-994 names as the unbounded
  wait on OPEN-1 route 1's path is invisible here.
- **Ring-0 console line splice (DDR-1055)** — a required gate sentinel was
  assembled from three unlocked `kputs`/`kputdec` calls and a ring-3 `write(2)`
  landed between two of them, producing every character the kernel meant to print
  in a line no gate can match. Root cause of the recurring `smoke-nethammer` red.
  **`console_line_lock()` did not cover this and `console.h` said it did** — it
  excludes only other holders of `g_line_lock`, and `kwrite`, the busiest printer
  in the system, never took it. The fix is **one `kwrite`, not one more lock**:
  a single `kwrite` holds `g_console_lock` for the whole buffer, and every printer
  takes that lock. Scope measured, not read: all 268 `EXTRA_SENTINEL` patterns
  checked, 21 sites converted; `idt.c:748` deliberately untouched (a trap printer
  that blocks turns a fault into a hang). Overflow is loud — `[kline] TRUNC` in
  `GLOBAL_FORBIDDEN`.
- **`lock_stat` made able to see a frozen CPU (DDR-1060)** — the DDR-1047
  instrument recorded its slot only **after** acquiring the lock, so a wedged AP,
  which never acquires, contributed nothing: it measured *completed* waits, the
  complement of the case it exists for. Fixed by claiming the slot and
  incrementing a live `waiters` count **before** the spin, so a frozen CPU leaves
  a permanent +1 on exactly the lock it is stuck on; and by fixing the printer's
  `if (!hits) continue;`, which would have skipped that very line. **Per-lock,
  not per-CPU, by design** — a per-CPU field needs `this_cpu()` (`%gs:0`, and
  DDR-1010 caught a broken SWAPGS discipline as one of OPEN-2's own producers) or
  `lapic_id()` (invalid pre-LAPIC, DDR-1055's reason for refusing exactly this).
  **`mnt_lock` is now visible**, closing PRE_LAUNCH_CHECKLIST §4.11's named gap,
  while keeping DDR-1047's unit boundary: spin and yield waits get different line
  shapes, never one column. M1/M0 two-sided on recorded hashes — the pre-fix tree
  under the same arm reports eleven other locks and omits the wedged one
  entirely. **No gate, deliberately** (the dump is reachable only on a run
  already failing). **NOT CLAIMED:** no defect fixed, no cause named; OPEN-1 and
  OPEN-2 untouched.

- **`smoke-sfs-btree-smp4` registered (DDR-1061)** — the OPEN-10 reproduction
  surface (DDR-824) was excluded with the note *"register it when OPEN-10 is
  fixed"*; DDR-964 fixed it, naming the mechanism and mutation-checking it, so
  the gate is now on shard 5 and the matrix is 176 gates / 6 excluded. **The rate
  campaign was stopped as null on its own design:** n=44 merely *reaches* the
  historical 6.7% failure rate, at 182 s per run, foreground-only — no reachable
  N settles it, and that was computed before the hours were spent rather than
  after (DDR-1002's shape, caught earlier). 3/3 measured on a pinned binary and
  explicitly **not** the basis for the decision. **NOT CLAIMED:** that the defect
  is proven gone; if the gate reddens, that capture is the measurement.

- **OPEN-2 bounded on the CI side (DDR-1062)** — 42 `pradyos-ci` suites since
  DDR-1049's detector landed, across 19 SHAs, with **zero** `[apfreeze]`,
  `panic_stage=` or `gs FAIL`. All four reds in that window are attributed to the
  DDR-1055/1056 console-splice class, none an AP freeze — checked by reading the
  captures, because a red left unread could be the artefact. **95% upper bound
  6.9% per suite**, and DDR-1009's 25% is refuted at p = 5.7 × 10⁻⁶. The
  discrimination set is now complete: each of the three `[apfreeze]` producers
  self-identifies, and DDR-1060's `waiters=` answers "was it a lock wait at all".
  **NOT CLAIMED:** OPEN-2 is not closed and no mechanism is named; a rate under
  6.9% is still a rate, and these are 42 suites over 19 SHAs, not 42 binaries.

- **`ptnode_in_use` underflowed on every COW fork — artefact produced, then fixed
  (DDR-1065)** — closing the `smoke-sharedpte` row unbuilt since DDR-1003.
  `ptnode_alloc` increments **once** per frame; `pmm_incref` (the kernel's **only**
  incref site) raises the refcount with no second increment — correct, no new
  frame — but `free_subtree` `ptnode_free`s the leaf page from **both** address
  spaces while `ptnode_free` decremented unconditionally and `pmm_free_pages` only
  releases on the last reference. **One `++`, two `--`, one release.** A COW page
  carries `PTE_SW_COW` (0x200), not `PTE_SW_SHARED` (0x400), so `vmm.c:371`'s skip
  does not cover it — **verified in the tree, not taken from DDR-1003's text**.
  **THE GATE HAD TO BE BUILT TO A SPECIFIC DESIGN:** DDR-1003 §5.1 warns the
  *ordinary* leak shape (fork, child **writes**, both exit) is balanced and would
  **pass**, so the probe's child **exits without writing** — one line different
  from `cow_selftest`, using the **real** fork path per DDR-1014's rule that a
  proof which paraphrases the kernel tests the paraphrase. **MEASURED:** pre-fix
  `before=0 after=18446744073709551615` — `0xFFFFFFFFFFFFFFFF`, i.e. **−1, wrapped
  from a single fork**, which also **refuted this DDR's own draft**, where I wrote
  the wrap was undemonstrated and would need ~2^64 forks. Fix is DDR-1003 §5.2's
  narrow version (`pmm_free_pages` returns released/not; `ptnode_free` decrements
  only on a real release; `void`→`int` is source-compatible so all 132 call sites
  stand), and §5.2's **wrong** fix is named and refused. M1 is the literal pre-fix
  tree. `GLOBAL_FORBIDDEN` deliberately untouched: this defect is **deterministic**
  and its own gate covers both directions, unlike DDR-981/1049's intermittents.
  **NOT CLAIMED:** no leak is fixed and none existed — frames were always released,
  the *counter* was wrong; no open issue moves.

- **The rq-3 discriminator had the race it was built to settle (DDR-1064)** —
  DDR-1030 added `idle2=` so a `resched FAIL` could separate a sampling artefact
  from a real missed kick, and its §5 table read `idle=1 idle2=1` as "a scheduler
  defect". **It does not establish that.** `idle_after` is sampled *after*
  `sched_unblock` returns and `o->idle` is live, so a CPU can **enter** idle
  between the kernel's kick loop and that sample: DDR-1030 closed DDR-1004's
  window (a CPU *leaves* idle before the call) and **opened its mirror image**.
  DDR-1030 also **contradicted itself** — its §6 says the opposite of its §5,
  having generalised from a forced mutant that ran with `ipis=1`. The §5 reading
  had been copied into three documents. **First real occurrence:** CI
  34003737145, shard 4, `smoke-smppreempt`, `e9ed2c9` — a **docs-only** commit
  with `kernel.bin: OK`, so a bit-identical binary, and `ran=1` says the property
  under test held. Consistent with a correct kernel. **FIX:** `sched_unblock`
  records what its own loop saw at the instant it ran — `kidle=` and `kkick=` —
  **on the TCB, not in a global**, because it runs from MSI-X interrupt context
  and another CPU's unblock would clobber a global between the proof's call and
  its read; both fields get explicit initialisers in `sched_create`
  (§NON-NEGOTIABLE 10). This applies DDR-1014's own rule one level down: it made
  the two loops ask the same *question*, this makes them ask it at the same
  *instant*. M1 forced-proof (`4786243a1f71a021`) printed
  `ipis=1 ran=1 idle=1 idle2=1 kidle=1 kkick=1`; revert returns
  `d19cd33755330510` bit-for-bit. **NOT CLAIMED:** no scheduler defect is named
  or fixed; the verdict is deliberately unchanged (collapsing it to SKIP would
  delete DDR-1014's coverage); no rate is measured; and the new fields are proven
  **wired**, not proven on a genuinely failing path — the DDR-1060 limit,
  recorded rather than glossed.

- **Live-state documentation consistency gated (DDR-1063)** — `CLAUDE.md`
  §CURRENT BUILD STATE stated `kernel.bin`'s **post**-quantum size beside the
  **pre**-quantum headroom: the size was updated as ML-DSA landed and the
  subtraction beside it was not, **overstating the remaining kernel budget by
  102,400 B** for four commits, in the one file every session is told to trust
  *without re-deriving*. Headroom is not decoration — it is what a session uses to
  decide whether a subsystem fits before building it, and §INV.18's real bound is
  a **boot** failure, not a compile error. **No existing gate could see it:** the
  `Makefile` size gate checks the *binary* against the ceiling and says nothing
  about what a *document* claims, and none of `hygiene_check.sh`'s six checks read
  a document at all. New `ci-docstate-check` (hygiene → **ALL SEVEN**, plus the
  `shard-check` CI job) asserts `size + headroom == ceiling`, with the ceiling read
  from `Makefile:697` rather than hardcoded. It asserts **consistency, not
  currency** — requiring the doc to match every build would redden on correct
  in-progress work and get removed (DDR-1063 §5.1). Fails on **zero** pairings
  found, so a future rewording cannot silently retire it. M1 is the literal
  pre-fix `CLAUDE.md`; M2 is the vacuity arm; M3 (ceiling drift) found a wrong
  remedy message in the checker's own first draft. **NOT CLAIMED:** this does not
  make the documented numbers *correct* — a stale but self-consistent pair still
  passes, which is exactly what the checklist's §6 was.
  **Two document sweeps for the same class then found six more stale items**
  (DDR-1063 §7b/§7c), five of them one shape: **a gate name in a planning table
  that has never existed while the real gate did** — `smoke-wx`/`smoke-wxkernel`,
  `smoke-mc`/`smoke-mce`, `smoke-lazystack`/`smoke-stack-demand`,
  `smoke-vdso-read`/`smoke-vdso`, `smoke-maximize`/**`smoke-wmmax`**. Such a row
  reads as *unbuilt work*, so the cost is building something twice; the cause is
  structural — the name is written when the work is planned, the gate is named
  when it lands, and nothing reconciles them. **A mechanical gate-inventory check
  is buildable on `ci-shard-check`'s existing machinery and is deliberately NOT
  built**, because 59 of the 116 `smoke-*` names in `CLAUDE.md` have no Makefile
  target and **that is correct** — they are backlog rows, and no grep separates
  "claimed to exist" from "named as future work". Named as the largest piece of
  the residual rather than left implicit.


#### Post-quantum cryptographic primitives (FIPS 202 / FIPS 204, 2026-09)

**Mandatory v1 scope per CLAUDE.md §PHASE 3.** The whole set shares one design
constraint: **a gate that checks an implementation against its own output proves
nothing**, because any self-consistent wrong implementation passes. Every expected
value below is generated by an implementation that is *not* this one and pinned
byte-exact.

- **Keccak-f[1600] / SHA-3 / SHAKE (DDR-1052)** — there was no SHA-3 anywhere in
  the tree, and both ML-KEM and ML-DSA are built on SHAKE, so this is a
  **prerequisite, not an extra**. Expected values come from Python `hashlib`, and
  hashlib is not merely trusted either — the empty-input digests were cross-checked
  against the **published FIPS 202 constants**, so the chain terminates in public
  knowledge. **The constants are derived and proved, not transcribed:** the 24
  round constants and rho offsets were generated from the FIPS 202 definitions and
  the permutation re-implemented in Python and checked against hashlib *before a
  line of C* — and **the first generator produced `RC[0]=0x03` instead of `0x01`**,
  exactly the silent total break that hand-transcribing 24 magic 64-bit constants
  produces. **Vector selection is the design:** 1–6 are the standard empty/`abc`
  values and catch almost nothing; 7–12 are the **absorb rate boundaries**, where
  `pad10*1` and multi-block absorb go wrong; 13–14 **squeeze past the rate**; 15 is
  a **streaming** arm (1/7/160/1-byte chunks), because a one-shot-only vector set
  never calls `update()` twice. M2 (a pad bug firing only when `len % rate ==
  rate-1`) **passes vectors 1–6 and fails 7** — which is why the boundary vectors
  exist. Gate: `smoke-shake` (shard 6).
- **ML-DSA-44 keyGen (DDR-1054)** — byte-exact against **NIST's own ACVP vectors**,
  chosen because keyGen is *deterministic*: one seed → one key pair, so a mismatch
  is unambiguous, whereas sign-then-verify passes on any self-consistent wrong
  implementation. **No magic constants:** the 256 twiddles are computed as
  `zeta^brv8(i)` and the invNTT scale as `256^(q-2) mod q` by Fermat, which
  evaluates to `8347681` — the literal the reference implementations carry
  (**checked, not assumed**). All mutable state is caller-owned (21,288 B scratch),
  which is load-bearing twice: it delivers the reentrancy `mldsa.h` claims, and
  `user/user.ld` gives a probe a single R+X `PT_LOAD`, so any writable section
  would link fine and **fault on first store**. **The KATs do not cover
  Power2Round's boundary — measured:** `r0 == 2^(D-1)` occurs in **0 of 2048**
  coefficients, so mutant M3 flipping `>` to `>=` **passed the ACVP arm outright**;
  a direct 10-case boundary arm was added, reporting a *negative* index so the two
  arms cannot be confused in a log. Gate: `smoke-mldsa` (shard 0).
- **ML-DSA-44 Sign_internal (DDR-1057)** — the predicted blocker did not exist:
  FIPS 204 signs with a random `rnd` by default and a randomized signature can only
  be checked by verifying it, **but ACVP publishes deterministic groups**
  (`rnd` = 32 zero bytes) with `signatureInterface=internal, externalMu=false` —
  Sign_internal itself, so a mismatch localises to the signing algorithm. Python
  oracle first, then the C. Vectors chosen for messages of 1 B and 273 B, because a
  set whose messages all fit one SHAKE block never exercises multi-block absorption
  in `mu = H(tr || M)`. **Two branches the KATs do not cover, both measured:**
  Decompose's `lo == GAMMA2` occurs in **0 of 28,672** calls, so mutant S4 flipping
  `>` to `>=` **passed the KAT arm** (fixed with 12 direct Alg. 36 cases); and the
  `hint_total > OMEGA` rejection is **not fixable by a unit arm** (it needs a
  crafted `sk`, which is not a KAT) — recorded uncovered, but since it is the only
  thing keeping HintBitPack inside the 2420-byte signature, the encoder now
  **re-checks the bound at the write** and returns −1. Rejection loop bounded at
  1000 (unbounded would violate S2).
- **ML-DSA-44 Verify_internal (DDR-1058)** — completes the primitive set. **The
  vector set is mostly negative and that is the point:** ACVP's group is 3
  signatures that must verify and **12 that must not**, so an implementation that
  always answers "valid" passes every positive test — sign-then-verify in disguise.
  Both verdicts are kept and the generator **refuses a one-sided set**; V1 (always
  accept) fails at vector 3, V2 (always reject) at vector 1. **Coverage measured
  per guard:** hint-encoding validation catches tcIds 107/113/119, but **the
  `||z||∞ < γ1−β` bound catches none of the twelve** — and that was measured
  *before* the test was written: forging an out-of-range `z` gives "verify WITH
  bound = False, WITHOUT = False", because altering `z` changes `w1'` and therefore
  `c~'`, so the hash comparison rejects it either way. A test asserting "rejected"
  would pass on an implementation with **no bound check at all**. Recorded
  **measured-uncovered** with the reason rather than given a decorative test.
- **NOT CLAIMED, for the whole set:** the kernel does **not** contain ML-DSA —
  `mldsa.c` is compiled into the ring-3 probe only; there is **no application**
  (the audit ledger is still SHA-256 and nothing calls these in anger); ML-DSA-44
  only; internal interface only; and **nothing about constant-time behaviour**,
  which matters most for signing — the reduction is a 64-bit `%` and the rejection
  loop's iteration count is itself secret-dependent, so this must not be used
  against an adversary who can measure it until that is addressed. See **DDR-1059**
  for why the signed ledger is assessed and deliberately not built: the blocker is
  **key custody**, not crypto — there is no TPM/PCR/secure boot, and
  `sys_vault.c:22` holds `g_owner_seed` as 32 literal bytes compiled into the
  image, so **anyone holding the ISO holds the private key**.

### Userspace / Syscalls / Shell
- Static ELF64 loader + per-process W^X AS (ADR-021); musl libc v1.2.5 (ADR-023;
  fetched from a GitHub mirror per **ADR-033** — same pinned commit, upstream
  `git.musl-libc.org` remains canonical)
- `pradyos-init` PID 1 + orphan reaper; PRISM shell w/ full-register fork (ADR-024)
- PRISM redirection `>` `>>` `<` `2>` and pipes `|` (DDR-778/780/781/784), on kernel
  `O_TRUNC`/atomic `O_APPEND` (DDR-782); diagnostics go to stderr
- CI harness exits a boot gate as soon as its required sentinels appear
  (**DDR-785**) — only where no `FORBIDDEN_SENTINEL` is declared, so no gate is
  weakened; self-tested by `make smoke-selftest`. **DDR-788** then raised the
  eligible gates' windows to 120 s, which early exit makes free on success,
  retiring the DDR-783 timeout-margin flake class
- PRISM builtins: help echo cat run ls ps kill setname touch rm uname date uptime dmesg free mode exit
  — `ls` enumerates via `SYS_GETDENTS` (DDR-742) and `ps` via `SYS_GETPROCS` (DDR-743) (was mis-tracked as planned in Section B#8 until 2026-07-24)
- Lazy per-thread FPU save/restore, user-only (DDR-740); copyin/copyout (EFAULT never panics)
- NSI **1–102** shipped; max is **102** (`SYS_POLL`, DDR-1037), next free **103**,
  table size 128. **Corrected 2026-09-03** — this line read "NSI 1–75", which was
  27 allocations stale. Verify against `kernel/syscall/syscall.h`, never against a
  prose line: CLAUDE.md §INV.14 and §CURRENT BUILD STATE have each been wrong here
  before, and allocating from a stale figure duplicates a live NSI.
  `SYS_GETDENTS` (66), `SYS_GETPROCS` (67), `SYS_POWEROFF`/`REBOOT` (69/70)
- `SYS_SYSINFO`/`TIME`/`DMESG`/`MEMINFO` (71–74), `SYS_SETNAME` (75), TCP loopback echo, kill end-to-end
- **`SYS_POLL` (NSI 102, DDR-1037)** — and it **generalised** the readiness
  predicate rather than duplicating it: `fd_ready_mask()` is now the kernel's
  single answer to "is this fd ready", shared by `epoll_wait` and `poll`, so the
  two cannot drift. **NOT** `SYS_POLL_RESULT` (32), which is the AETHER action
  poll — a name collision worth knowing. Deadline computed **once** before the
  loop (recomputing it inside would make a poll with a timeout never expire under
  load). Gate: `smoke-poll`.
- **PRISM erase (DDR-1039)** — `readline()` appended **every** non-newline byte,
  so a backspace landed *in* the command buffer and `hepl`+2 erases parsed as
  `hepl\x7f\x7flp`, matching no builtin. **Invisible to all 170 gates for one
  reason: every gate injects byte-perfect lines and none has ever typed a typo.**
  The DDR's own first arm was vacuous and it says so — it proposed asserting
  `help` output, but `smoke-shell` already feeds a plain `help` earlier in the same
  session, so that assertion passes on a shell with no erase handling at all. The
  shipped arm asserts **both directions** (`erase-ok-3m7` present, `erasX` absent),
  because presence alone cannot separate erase from strip. **Echo deliberately
  excluded:** PRISM shares COM1 with the kernel, so echoing typed input would
  inject it into the serial log every gate asserts on. Column-zero guard recorded
  **uncovered** — from outside the shell, erasing nothing and erasing at column
  zero are identical. Gate: the erase arm on `smoke-shell` (no `smoke-readline`
  exists, and none should be built).
- **Ring-3 composite sentinel splice (DDR-1056)** — the same defect as DDR-1055,
  one ring out: gates also assert via Makefile post-check greps of a **whole** line,
  and probes build those from many `wr()` calls — `actiondeltest.c:172` uses nine,
  which is **eleven** `write(2)`s because `wrdec` emits **one digit per write**.
  Three paths measured from the tree: probe lines built from many `wr()`; **musl's
  `fflush` emitting two console writes** because `__stdio_write` always passes two
  iovecs and `sys_writev` calls `fd_write_user` per iovec (so every musl program is
  affected, and stdout is fully buffered here because no `SYS_IOCTL` is
  registered); and `fd_write_user`'s 256-byte chunking, recorded not fixed. Fix:
  `user/include/uline.h` (one write per measured line) plus an `FD_CONSOLE` gather
  in `sys_writev`, sized **256 = `fd_write_user`'s own chunk size** and *not*
  musl's 1024 `BUFSIZ`, because a larger buffer would hold `g_console_lock` and
  interrupts across 4x today's maximum UART busy-wait on the hottest output path.
  **Proved deterministically** (it does not reproduce locally, so a rate campaign
  would measure nothing): the `actiondel` probe went 13 writes → 3, exactly −10.
  **NOT PROVEN:** the gather has no counter of its own and no mutation covers it —
  a mutant reverting it passes every gate, since the split is invisible unless a
  race is won.


### AETHER agent layer (kernel plumbing)
- Kernel action queue + append-only audit log (ADR-026); per-process mem cap + syscall rate limit
- 10 NSI agent calls (29–38); `CAP_SOVEREIGN`/`CAP_AGENT`; ring-3 daemon + `agent_base.c`
- Ring-3 socket NSI, 8 proxy sockets, live Ollama over HTTP (ADR-027)
- AETHER boot config from `/etc/aether/config` via SFS (DDR-732, DDR-770)
- `CAP_NET` allowlist, deny-by-default egress (DDR-734); agent CPU metrics (DDR-735/736)
- Per-agent live metrics, post-mortem stable (DDR-730); `SYS_AGENT_ROSTER` 8 named slots (DDR-707)
- 8 named agents KRYOS…SOLIN with UI panel cards + action pips (DDR-737)
- **The agent now EXECUTES its approved action (DDR-1066)** — until 2026-09-06 it
  did not. `user/agent_base.c` contained **zero** `SYS_OPEN` and **zero**
  `SYS_WRITE` in either branch; on `AE_APPROVED` it `printf`'d the path and the
  data, and the data string is `PRADYOS_AGENT_VERIFIED` — **one of
  `smoke-aether`'s four required sentinels** — so the end-to-end gate's *execute*
  arm asserted on a `.rodata` literal and could not fail for the reason it exists.
  The dead-arm class, in the **product** rather than in a gate. Nothing blocked
  it: `elf.c:320-321` gives the agent `CAP_FS_WRITE` and the FAT32 root, and
  `fat32_write` is wired; and the old path `/tmp/aether_test.txt` would have
  failed regardless, because the volume's only directory is `::/DOCS` and there is
  **no `/tmp`**. The agent now writes, closes, **reopens without `O_CREAT`** and
  reads back, printing the marker **from the read-back buffer** — so the sentinel
  the gate always required now means what the gate's comment always said, with no
  edit to that arm. **M1** (no write, print from the buffer) reddens
  `smoke-aether` with `AETHER_AGENT_EXEC_FAIL step=open_r rc=-2`; **M2** (no
  write, print the literal — the pre-fix behaviour) **passes**. They do the same
  filesystem work — none — so the only difference is where the bytes came from.
  `ACTION_SEND_IPC` is **still** unwired, and that is now the exception rather
  than the rule.

### Layer 7 UI / Sovereign desktop
- VirtIO-GPU framebuffer (ADR-028); ring-3 FB surface `SYS_FB_INFO/MAP/FLUSH` (DDR-702)
- DDR-1033: **`SYS_IPC_SEND` / `SYS_IPC_RECV` (NSI 98/99)** — the ring-3 IPC
  door, closing DDR-1017's `ACTION_SEND_IPC` gap. One endpoint per roster slot,
  addressed by slot index. Two layers: `is_ipc` (the door) and the capability
  handle (coarse — one shared `res_id`, no per-slot policy). Gated by
  `smoke-sendipc` (5 sentinels, granted **and** refused), M1/M2/M3
  mutation-checked. The action path does not yet call it.
- DDR-1032: **`execve` argv/envp marshalling** — they were previously `(void)`-cast
  away and `argc` hardcoded to 1, so an `execve` with arguments succeeded and
  delivered none. Strings are flattened into a kernel blob before the caller's
  address space is replaced. `args == NULL` takes the original path verbatim, so
  every boot probe is unchanged. Gated by `smoke-execve-argv` (7 arms, one of
  which measures RSP alignment at entry), M1/M2 mutation-checked.
- DDR-1031: **`SYS_MPROTECT` (NSI 97)** — change an existing user mapping's
  permissions, keeping its frames (`vmm_protect_range`). Preserves `PTE_SW_COW`
  and `PTE_SW_SHARED`. Refuses W+X, write-on-COW, and `PROT_NONE`, each for a
  stated reason. Gated by `smoke-mprotect` (5 arms), M1/M3 mutation-checked;
  the `invlpg` is recorded as uncovered.
- PS/2 keyboard `SYS_INPUT_POLL` (DDR-703); virtio-input pointer `SYS_MOUSE_POLL` (DDR-705)
  - DDR-1028: the compositor publishes `PRADYOS_INPUT_READY` on its first
    successful pointer poll. `PRADYOS_AMBIANCE_OK` means "loop is about to
    start" and is ~10 s earlier — every pointer-gate injector had been firing
    into a compositor that was not yet reading the pointer. NOT a fix for that
    10 s itself, which is unexplained and open.
  - DDR-1027: **Ctrl+Alt+T launches a PRISM terminal window.** `user/term.c`
    owns a surface, runs PRISM over a pipe pair (`fork`+`execve`, not
    `SYS_SPAWN_AGENT`), renders with the Inter atlas, and forwards the keys the
    compositor routes to the focused window. An **epoll** client with timeout 0,
    because this kernel has no `O_NONBLOCK` and a blocking pipe read would stop
    the window draining its key ring. No ANSI/VT parsing, no resize, no reap.
    Gated by `smoke-ctrlaltt` (5 arms), mutation-checked M1/M2/M3.
  - DDR-1026: `SYS_MOUSE_POLL` carries a **press-edge latch** — a press that
    completed since the last poll is still delivered, so a click made before the
    compositor's first input sample (or between two samples) is not lost. Bitmask,
    not a counter: repeated clicks between two polls still coalesce, and a missed
    release is still missed. Gated by `smoke-mouse` (kernel arm `mbtn>=1` first,
    then `PRADYOS_MOUSE_OK`), mutation-checked M1.
- Sovereign/Manual mode toggle (DDR-701); compositor w/ 8×8 font (DDR-704)
- Per-client surfaces `SYS_SURFACE_*` (DDR-706); z-order/focus/key routing (DDR-708)
- Sun-driven OKLab ambiances (DDR-709); window drag/close/resize/minimize/maximize (DDR-710/711/717/719)
- Per-window restore from a DOCK (DDR-1008): a strip of tiles along the bottom,
  one per minimized window, drawn over the windows and present only while
  something is minimized. Clicking a tile restores THAT window; DDR-717's `r`
  (restore-all) still works. Tiles are ordered by surface id, not z-order, so
  the dock does not reshuffle when an unrelated window is raised.
- Maximize fills the mode-aware WORK AREA, not a hardcoded 512x512 (DDR-1007):
  798x728 at a 1024x768 scanout in Sovereign, clearing the accent bar and the
  agent panel; Manual clears its menu bar and taskbar instead. Raising the cap
  required SURFACE_VA_SLOT to move with it (1 MiB -> 4 MiB) — the two were
  pinned to each other undocumented, and are now tied by _Static_assert.
- Glass blur, gradients, particle field, decorations, alt-tab, page flip, scroll, spring/ripple, Inter font
  (DDR-712/720/721/722/723/724/725/726/727/728)
- Surface destroy lifecycle safety (DDR-729); agent-card click → `SYS_SPAWN_AGENT` (DDR-713)

---

## Section A2 — AETHER host-side Python agent layer (ASI-bridge v4.0)

A **separate layer** from everything else in this document. Root `aether/`;
Python 3.13; runs on top of the OS as a service, not inside the kernel. Its
invariants are **S1–S14** in `aether/kernel/invariants/core_invariants.py` and are
**independent of Section H's S1–S8** below — the numbering collides, the meanings
do not, and they must never be merged.

Gate: `python -m pytest -W error -x -q aether/tests/` (CI job `aether-layer`).

| Item | File | Status |
|---|---|---|
| B-01 CAP_SOVEREIGN lockbox | `aether/kernel/lockbox/cap_sovereign.py` | ✅ |
| B-02 append-only audit log | `aether/kernel/audit/audit_log.py` | ✅ |
| B-03 Merkle integrity | `aether/kernel/integrity/merkle.py` | ✅ |
| B-04 prompt-injection firewall | `aether/agents/firewall/prompt_injection.py` | ✅ |
| B-05 quarantine namespace | `aether/agents/quarantine/namespace.py` | ✅ |
| B-06 red-team agent | `aether/agents/red_team/red_team_agent.py` | ✅ |
| B-07 agent runtime bus | `aether/agents/runtime/agent_bus.py` | ✅ |
| B-08 notification bus | `aether/agents/notification/notif_bus.py` | ✅ |
| B-09 Ollama model manager | `aether/ai_core/model_manager/manager.py` | ✅ |
| B-10 computer-use interface | `aether/ai_core/computer_use/interface.py` | ✅ |
| B-11 out-of-box experience | `aether/platform/oobe/oobe.py` | ✅ |
| B-12 AgentNet P2P mesh | `aether/agents/agentnet/mesh.py` | ✅ |
| B-13 soul/personality engine | `aether/agents/soul/soul_engine.py` | ✅ |
| B-14 vision pipeline | `aether/vision/pipeline.py` | ✅ |
| B-15 self-model maintenance | `aether/agents/self_model/self_model.py` | ✅ |
| B-16 introspective failure analysis | `aether/agents/introspection/failure_analysis.py` | ✅ |
| B-17 blind-spot discovery loop | `aether/agents/blindspot/discovery_loop.py` | ✅ |
| C-01 federated knowledge sync | `aether/agents/federation/knowledge_sync.py` | ✅ |
| C-02 distributed experiments | `aether/agents/federation/distributed_experiments.py` | ✅ |
| C-03 emergent tool composition | `aether/agents/tool_composer/composer.py` | ✅ |
| C-04 invention registry | `aether/agents/invention/registry.py` | ✅ |
| C-05 capability boundary mapper | `aether/agents/capability/boundary_mapper.py` | ✅ |
| C-06 counterfactual simulator | `aether/agents/counterfactual/simulator.py` | ✅ |
| C-07 NL constraint spec + enforcer | `aether/agents/constraints/nl_constraint_parser.py` | ✅ |
| C-08 offline safety review mode | `aether/agents/safety/offline_review.py` | ✅ |
| C-09 versioned agent contracts | `aether/agents/contracts/agent_contract.py` | ✅ |
| C-10 cross-domain analogy engine | `aether/agents/analogy/cross_domain_engine.py` | ✅ |
| D-01 self-improvement proposals | `aether/agents/self_improvement/proposal_engine.py` | ✅ |
| D-02 long-horizon planner | `aether/agents/planner/long_horizon_planner.py` | ✅ |
| D-03 multi-model ensemble router | `aether/ai_core/ensemble/ensemble_router.py` | ✅ |
| D-04 persistent world model | `aether/agents/world_model/world_model.py` | ✅ |
| D-05 meta-learning controller | `aether/agents/meta_learning/meta_controller.py` | ✅ |
| D-06 causal reasoning engine | `aether/agents/causal/causal_reasoning_engine.py` | ✅ |
| D-07 hypothesis generator | `aether/agents/research/hypothesis_generator.py` | ✅ |
| D-08 superalignment monitor | `aether/agents/safety/superalignment_monitor.py` | ✅ |
| D-09 recursive self-model updater | `aether/agents/self_model/recursive_self_model_updater.py` | ✅ |
| D-10 experiment outcome evaluator | `aether/agents/research/experiment_outcome_evaluator.py` | ✅ |
| D-11 knowledge consolidation | `aether/agents/memory/knowledge_consolidation.py` | ✅ |
| D-12 adaptive goal prioritizer | `aether/agents/planner/adaptive_goal_prioritizer.py` | ✅ |
| D-13 failure memory registry | `aether/agents/memory/failure_memory_registry.py` | ✅ |
| D-14 lineage knowledge loader | `aether/agents/memory/lineage_knowledge_loader.py` | ✅ |
| D-15 autonomous skill composer | `aether/agents/tool_composer/autonomous_skill_composer.py` | ✅ |
| I-09 D-08 alignment wiring | `aether/agents/safety/alignment_wiring.py` | ✅ |
| I-02 capability enforcement | `aether/capability/enforcement.py` | ✅ |
| I-10 daemon coordinator | `aether/daemon/coordinator.py` | ✅ |
| DDR-792 ollama transport bridge | `aether/ollama_bridge/transport.py` | ✅ |
| F#68 metric lockbox (S3) | `aether/kernel/lockbox/metric_lockbox.py` | ✅ |
| Privacy-mode netfilter hook (Python transport only) | `aether/platform/privacy/netfilter.py` | ✅ |
| Privacy-mode netfilter — KERNEL (DDR-802, gated by `smoke-privacy-netfilter`) | `kernel/syscall/sys_socket.c`, `kernel/aether/aether_queue.c` | ✅ |
| Per-boot probe selection via QEMU fw_cfg (DDR-804, closes OPEN-7) | `kernel/drivers/fwcfg/fwcfg.c` | ✅ |
| Shared egress rate limiter (S2) | `aether/platform/ratelimit/shared_limiter.py` | ✅ |
| Cloud bridge (built, **not enabled** — DDR-793 R1/R3) | `aether/cloud_bridge/transport.py` | ✅ |
| R3 per-destination egress audit (DDR-801) | `kernel/syscall/sys_socket.c` + `user/egressaudittest.c` | ✅ |
| R1 sovereign-egress audit (DDR-800) | `kernel/syscall/sys_socket.c` + `user/sovegresstest.c` | ✅ |
| F#68 kernel wire (DDR-795) | `kernel/aether/metric_page.c` + `aether/kernel/lockbox/metric_region.py` | ✅ |
| **I-01…I-10** integration wiring | — | ✅ **COMPLETE** |
| **J-01…J-06** Phase-4 retro audit | — | ✅ **verified, DDR-845** (audit; no code) |

Note: the spec's `ai-core/` is `aether/ai_core/` — hyphens are illegal in Python
package names, so the literal path would be a syntax error on import.

---

## Section B — Kernel/OS planned features (tracked)

> **⚠ Audit note (2026-07-26).** This table was inherited from an earlier snapshot
> and **overstates remaining work**. Items **#5 (COW fork)**, **#7 (kernel self
> W^X)** and **#8 (`ls`/`ps`)** were each marked "planned" but are in fact
> **shipped and gated** — verified against the repo, not assumed. The remaining
> entries have **not** all been individually re-verified; check the tree before
> planning any of them, exactly as B#5/#7/#8 required.

| # | Feature | Priority | Status | Requires |
|---|---|---|---|---|
| 1 | NVMe IRQ (MSI-X vector) | High | **re-scoped → DDR-774a/b/c** (blast-radius reviewed 2026-07-24) | **Vector availability was never the blocker** (DDR-771 vacated 50–53). The real cost is three coupled surfaces: (a) no generic PCI MSI-X programmer exists — the only one is virtio-coupled (`virtio_pci_msix_setup` takes `struct virtio_pci_dev*`, uses virtio's `map_bar` + `common_cfg.queue_msix_vector`), so it must be refactored out of a path serving blk/net/gpu/input; (b) `nvme.c` maps a fixed 2-page (`0x2000`) BAR0 window, but the MSI-X table offset/BIR must be read at runtime and may lie outside it or in another BAR; (c) completion moves from thread-context poll to IRQ context. Split into **774a** generic MSI-X helper (pure refactor, existing gates) — **DONE**, see `docs/ddr/DDR-774a-generic-pcie-msix.md`: `pcie_msix_find`/`pcie_msix_program`/`pcie_intx_disable` now live in `pcie.c`, `virtio_pci_msix_setup` reimplemented on them with identical signature/register order (all 3 virtio callers unchanged) — then **774b** NVMe table mapping + `IEN` — **DONE but with an OPEN ISSUE**: the table is found/mapped/programmed and `smoke-nvme` stays green (`[nvme] msix vec=50`), yet **`[nvme] irqs=0` — no interrupt is actually delivered**. Ruled out: interrupts-masked (`sti` at main.c:1478/1737 precedes `nvme_init` at :1801). Remaining suspects: MSI-X Function-Mask bit (MC bit 14) never explicitly cleared, table address math, per-entry vector control. See `docs/ddr/DDR-774b-nvme-msix-table.md`. **774c (c-1) ROOT-CAUSED AND FIXED**: the bug was in 774b — NVMe `Create I/O CQ` CDW11[31:16] is an **index into the MSI-X table**, not the x86 vector, so passing 50 aimed the controller at an unprogrammed/masked entry while only entry 0 was programmed. Passing the table index (0) instead gives `[nvme] irqs=6` and `PRADYOS_NVME_IRQ_OK`. Notably the leading suspect (MSI-X Function Mask, MC bit 14) was **NOT** the cause, so the shared 774a helper was left untouched. **774c phase c-2 STOPPED + re-scoped (stop condition invoked, no code)**: `nvme_submit()` is *already* a bounded poll, so "IRQ flag + bounded spin, no scheduler hook" degenerates into polling-with-a-hint and saves nothing; a real IRQ-driven wait needs the CPU to sleep, i.e. `sti;hlt` (rejected — mutates caller `IF`), a guarded `hlt` idle path, or a scheduler block/wake wait-queue (out of scope). Deferred to a future DDR that must first *measure* the polling cost and choose between those. **B#1 is functionally complete for correctness** (programmed + delivered + gated); the remainder is optional performance work. See `docs/ddr/DDR-774c-nvme-irq-delivery.md`. |
| 2 | mkfs.sfs multi-leaf B+tree (>14 slots) | High | **shipped (DDR-773)** — moved to Section A | Host tool mirrors kernel `sfs.c` descend/node format exactly |
| 3 | `-smp 4` percpu-sched race root-cause | **High** | **NARROWED 2026-07-30 → DDR-806. NOT explained, NOT fixed** (a proposed fix was implemented and refuted the same day) | **⚑ STATUS (DDR-806) — read before the history below. Four things are now established from specific artefacts, and one candidate survives.** ESTABLISHED: (1) the proofs do not execute — neither the OK nor the FAIL variant appears, and the DDR-777 entry marker is absent; (2) it is NOT the `!g_smp_have_aps` guard — failing run 30507516805 contains `[smp] cpus online=4/4`, `ap preempt OK`, `resched OK`, so **DDR-777 verdict (C) is refuted on evidence**; (3) it is NOT a code regression — `9f1459a`/`6c375ea`/`c9a1537`/`d8c5c95` are a byte-identical kernel with CI alternating FAIL/PASS/FAIL/PASS across two *different* gates, which also retires DDR-804 as a suspect; (4) the proofs CANNOT be hoisted above the probe block — `smpuser_proof()` polls `g_user_on_ap`, which requires live user processes that the probe block creates. SURVIVING CANDIDATE: `fs_test_thread` (`main.c:829`, **not** `kmain`) does not reach `main.c:1311` inside the window, because ~30 `user_boot_from_sfs()` calls sit in between, each doing blocking SFS I/O over contended virtio-blk. NEXT MEASUREMENT, required before any fix: stamp `g_ticks` at `main.c:1134` and `main.c:1311`. **Everything below is the four-hypothesis history; DDR-806 has itself already produced two confident explanations that the next measurement destroyed, so treat all of it as unproven.** | The DDR-771 timeout bump (90→180 s) is **not** sufficient: CI run 30151522978 failed `smoke-surfdestroy` at the full 180 s having missed the **first** sentinel (`..._CHURN_OK`), and the serial shows the boot **HUNG after `SYSFSTAT OK`** — inside the ring-3 syscall self-tests, long before any surface test runs. So it is a **hang, not slowness**. Pre-existing and unrelated to the DDR-774 work: the same gate failed in run 29726803735 (DDR-766, before 774a/b/c existed), and this gate boots with **no NVMe device**, so `nvme_init` never runs. Intermittent — it passed in runs 30141466540 and 30146543550. **NARROWED 2026-07-25 → it is the virtio-blk completion path, not percpu-sched** (see `docs/ddr/DDR-775-smp4-blk-hang.md`). Second CI hit, run 30155872016, was `smoke-blk-integrity` (`-smp 4`, **concurrent read data-verify**) also timing out at the full 180 s; two different gates, both `-smp 4` and both block-I/O, while 3/3 local `smoke-surfdestroy` runs PASS. In the surfdestroy case the stall point (`SYSFSTAT OK` → next is `SYSREAD OK`) puts it inside `sys_read` → `vfs_read` → SFS → virtio-blk. **Confirmed defect (S2 violation):** `submit()`'s `while (!done) sched_block_on(...)` is **unbounded**, so any missed completion becomes a permanent hang instead of a diagnosable error. (The lost-wakeup race itself IS correctly handled — locks-4 pattern.) **Latent defect:** `slot_waiter` is a single pointer, so >`VBLK_NREQ`(8) concurrent submitters lose a wakeup — real, but not claimed as this trigger. **CONFIRMED INTERMITTENT:** run 30158060606 passed ALL of those gates on the same commits — including `smoke-blk-integrity` and the `MSI-X-on-AP` test (a blk completion on a non-BSP CPU) — so **one green run proves nothing**. **DDR-776 shipped (diagnosis before fix):** a passive stuck-request watchdog driven from the timer path (same idiom as `net_poll_tick`) prints `[vblk] stuck dev=D slot=N lba=L age=T` **once** per request after 5 s. It changes **no** blocking behaviour, takes **no** lock (read-only from the ISR, so no new deadlock surface — S6) and is bounded at 64 scalar checks/tick (S2). Design decision made explicitly: a `g_ticks` yield-loop was **rejected** (it would spin on *every* block I/O, regressing the hot path every FS gate rides), and a scheduler timed-block — the correct eventual primitive — was **deferred** because changing the scheduler core mid-investigation would confound attribution of this very bug. **DDR-776 does NOT fix the hang**; it makes failures diagnosable. **⚠ AND IT ALREADY PAID OFF WITH A NEGATIVE RESULT (run 30163444702): the virtio-blk narrowing is REFUTED.** A third `-smp 4` gate failed — `smoke-smpuser` ("user-on-AP", **not** block-I/O), missing `[smp] user on AP OK` — and the watchdog printed **nothing**, i.e. no blk request was stuck >5 s, while the timer was demonstrably still firing (boot progressed through the fuzz test). The three failures share only `-smp 4` and miss *different* sentinels each time (`SYSREAD` path / `[smp] blk integrity OK` / `[smp] user on AP OK`). **Revised: the original percpu-scheduler/AP-race framing is better supported than the virtio-blk narrowing.** Hazards 1 (unbounded completion wait) and 2 (single-element `slot_waiter`) remain genuine S2 defects worth fixing on their own merit, but are **not proven to be this trigger**. **⚠⚠ SECOND CORRECTION — UNIFYING HYPOTHESIS: THE TIMER STALLS.** In run 30163444702 the serial printed **neither** `[smp] user on AP OK` **nor** `[smp] user on AP FAIL` (the two log hits are the sentinel echo + the "not found" message, not serial), while the preceding `ap preempt OK` / `resched OK` did print. But `smpuser_proof()` (`main.c:659`) is `while (!g_user_on_ap && g_ticks < dl)` — a deadline poll that MUST print one branch **unless `g_ticks` stops advancing**. This also **retracts** the previous inference: the watchdog is driven from the same timer path, so its silence is consistent with either "no stuck blk request" OR "the watchdog never ran" — it cannot distinguish them, and my "timer was demonstrably firing" claim was based on boot progress that happened *earlier* than the stall. **Leading hypothesis: under `-smp 4` the timer tick intermittently stops advancing `g_ticks`**, which explains all four failures at once (different sentinels = wherever the boot was), the watchdog's silence, blk waits never waking, and local passes. **Systemic S2 exposure: every `g_ticks`-bounded wait in the tree is only as bounded as the timer.** **⚠⚠⚠ THIRD CORRECTION + DDR-777 probe shipped.** Retracted: *"timed out at the full 180 s, therefore it hung"* — `boot_test.sh` **always** runs QEMU for the whole window then greps, and `terminating on signal 15 … (timeout)` appears in **passing** runs too; the only hard evidence is **sentinel absent**, not *hang*. Newly established: every SMP proof shares `if (!g_smp_have_aps) return;`, and `ap preempt OK`/`resched OK` DID print, so APs were up and `smpuser_proof()` did **not** early-return — it entered the poll and never reached its `kputs`. Three explanations survive: **(A)** timer stalls (`g_ticks < dl` never expires), **(B)** scheduler starvation (never resumes from `yield()`, system otherwise alive), **(C)** guard/ordering effect. **DDR-777 ships only a discriminator** — `[hb] t=<g_ticks>` every ~500 ticks from the existing timer call site + a `[smp] user-on-AP probe t=…` entry marker + tick on OK/FAIL. Reading: no probe line ⇒ (C); probe + heartbeat **stops** ⇒ (A), a systemic S2 exposure (every `g_ticks`-bounded wait is only as bounded as the timer); probe + heartbeat **continues** ⇒ (B). Passive: no behaviour change, no locks, no scheduler hook; sentinel safety verified (`grep -qF` substring). |
| 4 | SFS as default process root | Medium | planned | Provisioned SFS image as default `root_mnt`; retire blk2's dual role (scratch + root). Unblocked by DDR-771. |
| 5 | COW fork | — | **shipped — entry was STALE, corrected 2026-07-26; moved to Section A** | Verified built: `kernel/mm/vmm_cow.c` implements `vmm_fork_address_space_cow()`, `PTE_SW_COW` tagging in both address spaces, PMM refcounts, and `PTE_SW_SHARED` pass-through for the vDSO; `vmm_cow_fault()` is wired into the #PF handler at `idt.c:225`; gated by `smoke-cowfork` (Makefile:760, CI:228) plus a kernel self-test in `main.c`. |
| 6 | ext4 write support | Medium | planned | ADR-019 extension, journal transaction layer |
| 7 | Kernel self W^X (kernel text RX / data NX) | — | **shipped — entry was STALE, corrected 2026-07-26; moved to Section A** | Verified built: `vmm_protect_kernel()` re-stamps the kernel image after boot (text RX, data NX), gated by the DDR-757 gate `smoke-kernelwx` (Makefile:660) and CI's "kernel-self W^X test (q35; text RX + data NX PTE audit)". The old note "bootloader maps the kernel image RWX" describes only the pre-`vmm_protect_kernel` state. |
| 8 | `ls`/`ps` full implementations | — | **shipped — entry was STALE, corrected 2026-07-24; moved to Section A** | Drift found while scoping: both are implemented in `user/prism.c` — `ls` over `SYS_GETDENTS` (66, DDR-742) and `ps` over `SYS_GETPROCS` (67, DDR-743). The "requires a process-table scan syscall" precondition was already satisfied. |
| 9 | I/O APIC (q35 GSI routing) | Low | planned | Would replace 8259 for ISA devices |
| 10 | Per-CPU runqueue affinity/NUMA hints | Low | planned | CPU topology hints extension to DDR-SMP-rq-1 |
| 11 | wlroots/Wayland compatibility | Low | planned | EGL/GBM bridge to VirtIO-GPU; blocked by no-out-of-tree-libs policy |
| 12 | Pipes/redirection/job control in PRISM | Low | **partially shipped — output redirection done (DDR-778)**; `\|` and job control remain | Kernel side ALREADY ships (`SYS_DUP2 = 18` + `SYS_PIPE`, PROC-A, gates `smoke-syspipe`/CI "pipe/dup2") — `prism.c` had simply never `#define`d `SYS_DUP2`, so **DDR-778 needed no kernel change**. Output redirection `cmd … > file` now works via `dup2(1, save)` / `open` / `dup2(fd,1)` and restores at the loop's existing flush point. Two hazards handled: musl fully buffers a non-tty stdout so output is flushed **before** fd 1 is restored, and the restore sits on the single path every dispatched command ends on (audited: the only `continue` precedes the swap, `exit` returns). Gated in `smoke-shell` by a **discriminating pair** — the marker alone would pass even if redirection did nothing, so `REDIR.TXT` must also appear in `ls /`. **`\|` also DONE (DDR-780)** — ring-3, no kernel change (`SYS_PIPE`/`SYS_DUP2` already ship). Each forked half sets up its fds and **falls through** to the existing dispatch (avoiding a 120-line refactor), exiting at the loop bottom; the parent closes **both** ends (or the reader never sees EOF) and reaps both children — S2 bounded, S6 isolated. **`cat` with no arg now reads stdin**, since no builtin previously consumed fd 0 and a pipe would otherwise be unobservable. Gate uses a discriminating pair (marker present AND `pipe-marker-4k8 \| cat` absent). **`<` and `>>` also DONE (DDR-781)** — ring-3, no kernel change, though the prerequisite check changed the mechanism: the kernel has **no `O_APPEND`** (and no `O_TRUNC`), so append is `open` + `SYS_LSEEK(SEEK_END)` rather than an open flag; documented as non-atomic (one writer per command). Gates are discriminating (`>>` requires BOTH records to survive; `<` requires the marker AND forbids `cat: cannot open <`). **Kernel `O_TRUNC` + atomic `O_APPEND` now DONE (DDR-782)** — the two gaps DDR-781 recorded as real defects. Prerequisite check re-scoped it: `struct vfs_fs_ops` has **no truncate op** and no FS driver can shorten a file, so rather than invent one, `O_TRUNC` is `vfs_unlink` + `vfs_create` on an existing file (truncation-to-zero, which is all `>` needs; the honest limitation is a fresh inode/cookie, and non-zero `ftruncate` stays impossible). `O_APPEND` is fd-layer only — `sys_io.c` sets `e->off = e->file->size` once per `write()` call before the chunk loop, which IS the POSIX atomicity property. No new syscall, no new VFS op, no on-disk change, no capability change (CAP_FS_WRITE already gates create/unlink/write). PRISM's `>` now passes `O_TRUNC` and `>>` passes `O_APPEND`, dropping the `lseek` crutch. Gate is discriminating by construction: write long then short to the same file and forbid the long record's tail — that tail survives under the pre-DDR-782 behaviour, so the assertion fails before the fix. **stderr + `2>` now DONE (DDR-784)** — and the prerequisite check re-scoped it again: PRISM had **zero** writers to fd 2 (all diagnostics went `printf` → fd 1), so `2>` alone would have redirected an fd nothing writes to and no discriminating gate could exist. Landed as two halves — genuine errors moved to `fprintf(stderr, …)` (success/informational messages stay on stdout; **`rm: removed …` is gate-asserted**, so moving it would have silently broken `smoke-shell`), then `2>` added to the redirect scan with a third parking slot and DDR-782's truncating flags. No kernel change (fd 2 is already `FD_CONSOLE`; the FD_VFS write path is fd-agnostic), but the musl **subset** lacked `stderr.c`/`fprintf.c` — surfaced at link time and recorded, `tools/build_musl.sh` gained the two upstream sources. Gate discriminates by construction: stdout and stderr go to **different** files in one command, so a broken `2>` buries the error in the stdout file and the marker is absent. **Multi-stage pipelines `a\|b\|c` now DONE (DDR-786)** — DDR-780 honoured only the first `\|`, so a second one reached the builtin as a literal token. N stages did **not** force the ~120-line refactor DDR-780 deferred: the fall-through dispatch is reused and only the pipe block grows (~40 lines). The ~10-line "right child re-scans" design was rejected — it leaves an intermediate shell per boundary holding the previous read end open, which with non-blocking 4 KiB pipes silently drops output. Chosen: split all stages up front, fork each, thread one pipe between neighbours, close `prev_read`/`fds[1]` in the parent right after each fork, reap all N; malformed `\|` rejected before any fork. Recorded pre-existing limitation (not introduced): PIPE_SIZE 4096 + non-blocking `pipe_write` means a stage producing >4 KiB faster than its reader drains loses data. **Blocking pipe semantics DONE (DDR-787)** — the prerequisite check found the queued ">4 KiB truncation" was the smaller half: the read path returned 0 on a momentarily-empty ring and every reader treats that as EOF, so `a\|b` was timing-dependent and could print nothing. Neither half was fixable on a single `refcount` (a reader may block only while a writer remains, and vice versa — without those counts blocking is UNBOUNDED, an S2 violation), so `struct pipe` now counts `readers`/`writers` separately; reader blocks while empty && writers>0, writer blocks while full && readers>0 else `-EPIPE`. Gate pushes ~7.8 KiB through `cat \| cat`. **Exit status `$?` DONE (DDR-789)** — ring-3 only; `sys_wait4` already returned the raw code and PRISM was discarding it. A pipeline reports its last stage. Expansion covers a token *ending* in `$?` (widened from "exactly `$?`" when testing showed the real idiom `echo status=$?` is one token); not general `$VAR` expansion. The tree check also **reordered the queue**: SIGPIPE was next but signal defaults are a whitelist (everything without a handler except SIGKILL/SIGTERM is ignored), and SIGPIPE was ungateable until outcomes became observable — `$?` unblocks it. Remaining: `2>&1`/`2>>`, SIGPIPE (add to the default-terminate list), and job control (`&`, job table — needs signal plumbing). |
| 13 | Dynamic linker (`ld-pradyos.so`) | Low | planned | Relocatable ELF loader, GOT/PLT patching |
| 14 | 3-lane NAS scheduler | Low | planned | Lane classification field in TCB |
| 15 | PMM variable-weight/predictive allocator | Low | planned | Policy-driven allocator over current buddy |

---

## Section C — Competitor landscape (Table 1)

What to beat, and how AETHER structurally fixes it.

| Competitor | Scale | Structural weakness | AETHER fix |
|---|---|---|---|
| OpenClaw | 210k+ ★ | Abysmal skill-vetting security | `CAP_AGENT` ring-3 sandbox + 128 MiB cap + no self-escalation + `CAP_SOVEREIGN` veto + append-only audit |
| AutoGPT | 183k ★ | Stalled, hallucination-prone | SkillOpt-Sleep nightly evolution + MOSS-style rewriting + SFS CoW rollback |
| CrewAI | 60k+ ★ | No kernel isolation, logic-flow only | `ACTION_SEND_IPC` + DAG action queue at kernel level |
| LangChain/Langflow | 146k ★ | Stateless, cloud-dependent | `SYS_MEMORY_WRITE/READ` + SFS memory tree + local Ollama |
| n8n | 100k+ ★ | Static workflows, no learning | Subconscious agent rewrites plans on failure + SkillOpt validation |
| Dify | 85k+ ★ | Weak action capability | `ACTION_EXEC_CODE` + `ACTION_PARSE_DOCUMENT` |
| OpenHands/OpenDevin | 47k ★ | Privileged Docker, no self-improvement | Hard ring-3 isolation + kernel-audited syscalls + SkillOpt |
| MOSS | academic | No kernel safety boundary on self-rewriting | `ACTION_REWRITE_AGENT_CODE` gated behind `CAP_SOVEREIGN` + mandatory regression pass + SFS snapshot |
| Ouroboros/AutoResearch | Karpathy | No local fallback, no OS safety, metric-gaming risk | Local Ollama loop + `CAP_SOVEREIGN`-locked metric lockbox |
| AI Scientist/Curie | Sakana | $15–45/paper, no local run, no persistent memory | $0 local experiments + persistent hypothesis tree |
| Pentagi | 19k ★ | Security-domain only, no kernel sandbox | Same `CAP_AGENT` sandbox + rate limiting |
| RuVector | 4k ★ | Standalone DB, no agent loop | Adopt as AETHER's vector knowledge-graph memory model |

---

## Section D — ADR-026 baseline (#1–17) — **VERIFIED BUILT 2026-07-24**

Re-verified against actual repo state this session (not assumed). Evidence:
`kernel/aether/{aether.c,aether.h,aether_audit.c,aether_mem.c,aether_queue.c}`,
`kernel/cap.h`.

| # | Item | Status | Evidence |
|---|---|---|---|
| 1 | Kernel-is-arbiter trust model (`SYS_SUBMIT_ACTION` gate) | shipped | `SYS_SUBMIT_ACTION` present |
| 2 | Sovereign auto-approve mode | shipped | `sovereign` mode in `aether.c` |
| 3 | Manual mode w/ 60 s expiry (`SYS_APPROVE_ACTION`/`SYS_REJECT_ACTION`) | shipped | both syscalls present |
| 4 | 256-entry fixed action queue (no heap, DoS-resistant) | shipped | `AETHER_QUEUE_LEN 256` |
| 5 | 4096-entry append-only audit log (wrap-flagged) | shipped | `AETHER_AUDIT_LEN 4096` |
| 6 | Per-agent hard 128 MiB memory cap + OOM kill | shipped | `AETHER_MEM_DEFAULT (128ull << 20)` |
| 7 | `CAP_SOVEREIGN` bit (1<<16) | shipped | `cap.h:40` |
| 8 | `CAP_AGENT` bit (1<<17) | shipped | `cap.h:41` |
| 9 | 60 syscall/s sliding-window rate limiter | shipped | `AETHER_RATE_MAX 60`, `AETHER_RATE_WINDOW 100` ticks |
| 10 | `SYS_SPAWN_AGENT` force-PENDING even in sovereign mode | shipped | `SYS_SPAWN_AGENT` present |
| 11 | `SYS_KILL_AGENT` (caller kills own children only) | shipped | `SYS_KILL_AGENT` present |
| 12 | Ollama HTTP/1.1 ring-3 bridge (lwIP only, minimal JSON scanner) | shipped | ADR-027, Section A |
| 13 | CI test mode (deterministic, no live model) | shipped | gate suite is model-free |
| 14 | Daemon topology (PID-1 → daemon → agents) | shipped | DDR-761/770 |
| 15 | `ACTION_WRITE_FILE` (SFS W^X validated) | shipped | present |
| 16 | `ACTION_PRINT` (512-byte bounded) | shipped | `AETHER_PAYLOAD_MAX 512` |
| 17 | `ACTION_SPAWN_PROCESS` (force-PENDING always) | shipped | present |

**No drift found.** Next free capability bit is `1<<18`, matching Section E's
`CAP_MEMORY` plan. **Section D being confirmed built is the precondition for
starting any Section E/F work.**

---

## Section E — New features: kernel extensions, ring-3, post-L7 (#18–65)

All entries **proposed** until a DDR answers the architecture prerequisite
checklist for that specific item.

### 3A — New kernel syscalls (NSI appends start at **76**)
- `SYS_MEMORY_WRITE`/`SYS_MEMORY_READ` — persistent per-agent SFS CoW B+tree memory (`CAP_MEMORY` 1<<18)
- `SYS_CHECKPOINT_AGENT` / `SYS_RESUME_AGENT` — SFS snapshot serialize/restore, integrity-hashed, `CAP_SOVEREIGN`
- `spawn_depth` cap in TCB — child `mem_limit = parent/(depth+1)`, hard cap depth 3
- DAG action queue — `parent_action_id` u64 field
- `SYS_APPROVE_CODE_REWRITE` — `CAP_SOVEREIGN` gate for MOSS-style patches

### 3B — New capability bits
`CAP_MEMORY` (1<<18) · `CAP_OCR` (1<<19) · `CAP_EXEC` (1<<20) · `CAP_REWRITE`
(1<<21, **always** requires `CAP_SOVEREIGN` co-approval, never auto-granted) ·
`CAP_SCENE` (1<<22, post-L7 only) · `CAP_NET_BROWSE` (1<<23)

### 3C — New action types (#31–44)
`ACTION_READ_FILE` · `ACTION_DELETE_FILE` (force-PENDING in **every** mode,
sovereign included — DDR-842 S4; the older "in manual mode" here was wrong, and a
gate written from it would have asserted the opposite of the design) ·
`ACTION_EXEC_CODE` (sandboxed interpreter, 32 MiB cap) · `ACTION_SEND_IPC`
(agent-to-agent via NAS IPC, peer-token gated) · `ACTION_PARSE_DOCUMENT`
(OCR→Markdown, 64 MiB cap, local model) · `ACTION_BROWSE_WEB` (headless browser) ·
`ACTION_QUERY_MEMORY` (semantic/graph search, read-only, ≤16 results) ·
`ACTION_CAPTURE_FRAME` (post-L7) · `ACTION_SCAN_ENVIRONMENT` (SLAM3R, post-L7,
first-use always manual-gate) · `ACTION_QUERY_SCENE` (post-L7 NL query) ·
`ACTION_REWRITE_AGENT_CODE` (MOSS pipeline, force-PENDING, `CAP_REWRITE` +
`CAP_SOVEREIGN` co-approval, regression suite must pass first) ·
`ACTION_PROPOSE_HYPOTHESIS` (logged to SFS hypothesis tree) · `ACTION_RUN_EXPERIMENT`
(`CAP_EXEC`; metric function lives in a `CAP_SOVEREIGN`-locked SFS path — **cannot**
be modified by the experimenting agent) · `ACTION_EVOLVE_GENOME` (force-PENDING,
rewrites `genome.md` across generations, `CAP_SOVEREIGN` veto window)

**Build status — Section 3C is CLOSED: 6 of 8 shipped and gated, 2 deferred with logged reasons (DDR-1021), 0 buildable-and-unbuilt.** A 3C type is
implemented in RING 3 (DDR-1013 §2.1): the kernel is the policy engine, the
agent the executor, so "implemented" means a probe that proposes, waits for the
verdict, and acts only afterwards — plus a gate that asserts the *effect*.

| type | state | gate | DDR |
|---|---|---|---|
| `ACTION_READ_FILE` | shipped | `smoke-actionread` (asserts the content) | DDR-1015 |
| `ACTION_DELETE_FILE` | shipped, force-pending | `smoke-actiondel` (asserts PENDING + the file survives) | DDR-1016 |
| `ACTION_REWRITE_AGENT_CODE` | **shipped since DDR-842** — four capability roles, a real approver via `SYS_APPROVE_CODE_REWRITE` (NSI 86), a negative arm proving that call cannot approve a non-rewrite action, and the sov-only arm proving `CAP_REWRITE` is not decoration | `smoke-coderewrite` (shard 7, **strict**) | DDR-842 |
| `ACTION_PROPOSE_HYPOTHESIS` | shipped | `smoke-actionhypo` (auto-approves, then logs and reads back) | DDR-1020 |
| `ACTION_EVOLVE_GENOME` | shipped, force-pending | `smoke-actionhypo` (same boot: PENDING + genome untouched) | DDR-1020 |
| `RUN_EXPERIMENT` | **SHIPPED — DDR-1034**, superseding DDR-1021's deferral. A bounded integer stack machine: no `LOAD`, no `STORE`, no addressing mode, so "no memory outside its own stack" is a property of the instruction set rather than a guard that could be deleted; no `DIV` (a `#DE` in ring 0 is fatal, so the opcode is absent rather than checked); hard step cap, operand-stack bounds, code-length bound. `CAP_EXEC` is now a **checked** bit (`RES_EXEC` + `cap_authorize`), paired with `is_exec` on `struct tcb`. Results go to a **separate** kernel-owned ring whose only writer is the executor — the DDR-812 lockbox is not touched, extended or read. Deliberately **not** force-pending, with the reason recorded (DDR-1034 §4) | `smoke-runexp` (shard 8, **strict**) | DDR-1034 |
| `SEND_IPC` | **SHIPPED — DDR-1033.** `SYS_IPC_SEND`/`SYS_IPC_RECV` (NSI 98/99), addressed by roster slot; `is_ipc` + a `RES_IPC` capability. The capability is coarse by construction (one shared `res_id` = "IPC at all", not per-slot policy) and that limit is stated, not implied. **Residual:** the AETHER action path does not yet call it, so an approved `SEND_IPC` still has no automatic effect | `smoke-sendipc` (shard 7, **strict**) | DDR-1033 |
| `ACTION_QUERY_MEMORY` | shipped | `smoke-actionquery` (asserts the seeded bytes come back) | DDR-1018 |

**Two corrections to DDR-1017/1018's accounting.** (1) `ACTION_SPAWN_PROCESS` is
**not one of the eight** — `aether.h` pins it under *"pre-existing action types"*
and the DDR-842 3C block begins at `READ_FILE`. DDR-1017's gate for it is real
and useful, but it does not advance the 3C count, so "3 of 8" and "4 of 8" in
those two DDRs were both wrong. (2) `REWRITE_AGENT_CODE` was **already shipped
and gated by DDR-842**; DDR-1017 §7 and DDR-1018 §7 each listed it as remaining
without checking. Both DDRs asserted a type was unbuilt from memory instead of
grepping — the same failure that DDR-1018 §1 corrected for `QUERY_MEMORY`.
**Check the tree before declaring a type unbuilt.**


The remaining six 3C types in the list above (`EXEC_CODE`, `PARSE_DOCUMENT`,
`BROWSE_WEB`, `CAPTURE_FRAME`, `SCAN_ENVIRONMENT`, `QUERY_SCENE`) are
deliberately **absent from the enum** until their subsystem exists — see
`aether.h`, and CLAUDE.md §PRE-APPROVED EXCEPTIONS where each is logged.

### 3D — Ring-3/daemon-only (#45–65, no kernel changes)
Per-agent `skill.md` (300–2000 tokens) · SkillOpt training loop
(rollout→reflect→aggregate→select→update→evaluate; accept only on strictly positive
held-out improvement) · SkillOpt-Sleep nightly evolution
(harvest→mine→replay→consolidate; pauses active agents) · skill-update validation
gate (`CAP_SOVEREIGN` always) · multi-agent skill specialization/transfer ·
TokenJuice context compression (≤80% tokens) · JSONL trajectory run journal (from
kernel audit log) · per-call cost accounting (`token_count` + `latency_ms` in audit
entries) · `goals.md` per agent · subconscious background loop (wakes every N ticks,
goal-diff prompts, bounded by the 60 syscall/s limiter) · MOSS source-rewriting
pipeline (staging sandbox, regression gate, SFS snapshot rollback) · OCR→memory
pipeline · multi-modal context builder · privacy mode (blocks non-local Ollama via
lwIP netfilter hook) · model routing · hypothesis tree in SFS (versioned, persists
across boots) · `genome.md` per research agent (lineage archived) · vector knowledge
graph memory (RuVector-style, online learning) · dead-end registry (failure reason +
divergence score, queried before repeating) · population tournament mode ·
replayable run visualizer (optional ring-3 web UI)

---

## Section F — Visionary ASI-trajectory features (#66–76)

All **proposed**. No competitor ships these.

| # | Feature | Note |
|---|---|---|
| 66 | Self-designing agent architecture | `architect_agent` proposes topologies/syscalls/cap bits, writes ADR, scaffolds C stubs + CI smoke test; single human approval |
| 67 | Self-healing rollback cascade | `healer_agent`: checkpoint→diagnose(MOSS)→patch→regression-verify→restore, fully kernel-audited |
| **68** | **IMMUTABLE OBJECTIVE FUNCTION / metric lockbox** | `CAP_SOVEREIGN`-read-only + kernel-signed SFS path; any `CAP_AGENT` write attempt → cap-escalation-denied audit + process kill. **THE single most important safety invariant for any self-improvement loop (= Invariant S3). HIGHEST PRIORITY among proposed features — every Section E/F self-improvement feature depends on it existing first.** |
| 69 | Autonomous software inventor | `inventor_agent`: hypothesis→experiment→build→test→commit, zero human-written code; human approves final commit only |
| 70 | Divergence-aware multi-lineage research | `tournament_agent` scores parallel genome lineages, rejects premature convergence, `CAP_SOVEREIGN` per generation |
| 71 | Sub-conscious world model | `subconscious_agent` maintains compressed world model (memory+scene+trajectory), fires proactive prompts to idle agents |
| 72 | Verifier-based RL reward signals | `verifier_agent` cross-checks every experiment with a **different** model before it enters the hypothesis tree or SkillOpt reward — structurally prevents metric gaming beyond the lockbox |
| 73 | Sovereign UI w/ natural-language OS control | ring-3/post-L7 panel: goals in plain English, sovereign/manual toggle, audit log, approve/reject, hypothesis tree + world model. No CLI, no prompt templates |
| 74 | Capability discovery from internet | `research_agent` browses GitHub, generates+tests+quarantines new skill wrappers pending `CAP_SOVEREIGN` approval |
| 75 | Lineage memory / ancestral knowledge | Departure summaries on clean agent exit only; new instances read all past lineage files before their first Ollama call — knowledge compounds across restarts |
| 76 | Tamper-evident science ledger | Merkle-chained SFS ledger; every hypothesis/experiment/result/reflection/genome-edit carries `prev_hash sha256[32]`; `SYS_READ_AUDIT` verifies chain on read; append-only |

---

## Section G — Named agent roster (12 agents; extends the shipped 8-slot KRYOS…SOLIN roster)

| Agent | Capabilities | Role | Source | Status |
|---|---|---|---|---|
| `file_agent` | CAP_AGENT, CAP_MEMORY | File R/W/delete; skill evolves via SkillOpt | ADR-026 | proposed |
| `shell_agent` | CAP_AGENT, CAP_EXEC | Code execution, shell automation, skill transfer | RLM | proposed |
| `research_agent` | CAP_AGENT, CAP_MEMORY, CAP_NET_BROWSE | Web browse, doc parse, capability discovery | OpenHuman+OCR | proposed |
| `ocr_agent` | CAP_AGENT, CAP_OCR | PDF/image → structured Markdown; feeds memory pipeline | Unlimited-OCR | proposed |
| `subconscious_agent` | daemon-level | World model, proactive goal-diff, SkillOpt-Sleep orchestration | OpenHuman | proposed |
| `ai_scientist_agent` | CAP_AGENT, CAP_EXEC, CAP_MEMORY | Hypothesis gen, experiments, genome evolution, inventor loop | AI Scientist/Ouroboros | proposed |
| `healer_agent` | CAP_AGENT, CAP_REWRITE | Crash detection, MOSS diagnosis, patch proposal, rollback | MOSS+ADR-026 | proposed |
| `architect_agent` | CAP_AGENT, CAP_REWRITE | Proposes topologies/action types, writes ADRs + C stubs | Self-designing | proposed |
| `verifier_agent` | CAP_AGENT | Independently cross-checks experiment results | Verifier-RL | proposed |
| `tournament_agent` | daemon-level | Scores competing lineages, selects genome survivor | Ouroboros | proposed |
| `orchestrator_agent` | CAP_AGENT, depth-0 spawn rights | Spawns/coordinates fleets, manages DAG queue | OpenHuman/RLM | proposed |
| `vision_agent` | CAP_AGENT, CAP_SCENE (post-L7) | FB capture, SLAM3R 3D reconstruction, scene queries | SLAM3R | proposed |

---

## Section H — Security invariants (BINDING, ADR-level)

These govern **all 76 features**. Binding exactly like ADR-021's W^X contract:
changeable only by a superseding ADR, never quietly amended, and checked against
every new Section E/F feature **before** implementation.

- **S1 — No self-escalation.** An agent cannot raise its own cap bits, `mem_limit`, or approve its own actions.
- **S2 — Bounded everything.** queue 256 · audit 4096 · payload 512 B · memory 128 MiB · syscalls 60/s · skill 2 KB · IPC msg 256 B · spawn depth 3 · lineage results ≤16/query. Every bound returns an error or a clean kill — **never a panic**.
- **S3 — Immutable objective function.** `CAP_SOVEREIGN`-locked SFS path, kernel-signed at build time; agent write attempt → kill + audit. (Covers #43, #66–72.)
- **S4 — Human gate is structural.** spawn, scene, skill-update, code-rewrite, genome-evolve, architect proposals all require `CAP_SOVEREIGN` approval; force-PENDING for the highest-consequence actions.
- **S5 — Append-only audit + Merkle ledger.** No user-space erase path; science ledger Merkle-chained; wraps flagged.
- **S6 — Fault isolation.** A ring-3 fault kills the agent, never the kernel; copyin/copyout on all user pointers.
- **S7 — Metric-gaming prevention.** The verifier agent is structurally independent; the objective function is kernel-locked.
- **S8 — Skill-change veto.** Any `skill.md` or code write requires `CAP_SOVEREIGN` + optional manual approval.

---

## Architecture prerequisite checklist (answer in the DDR before coding any Section E/F item)

1. New NSI/syscalls — is the append-only NSI range still open? (through 75; next append starts at **76**)
2. TCB / roster-slot / agent-slot fields — struct extension? ABI size assumptions?
3. PMM/VMM shared mappings — needs `PTE_SW_SHARED` (DDR-729 pattern) or a new mapping class?
4. Capability checks — which `CAP_*` gates it (existing or 1<<18..1<<23); is a new bit needed?
5. AETHER queue/audit extensions — new record type in `aether_audit.c`/`aether_queue.c` (Merkle chaining for S5, DAG `parent_action_id`)?
6. Scheduler/accounting hooks — `sched_exit`, `sched_create_state`, `agent_metrics_reap` (lineage departure summary, spawn-depth enforcement)?
7. Filesystem/root-mount constraints — depends on `root_mnt` (DDR-739), SFS dirs (DDR-738), or a new `fs_prefix`/namespace concept?
8. Network policy tables — touches `net_allow`, `CAP_NET`, `CAP_NET_BROWSE`, or the socket NSI (ADR-027)?
9. Compositor/UI exposure — new agent card/roster visual (Section G agents, Section F #73 sovereign UI)?
10. New smoke gate — what proves it **deterministically**? (avoid TCG-timing flakiness — DDR-735/771 lessons)
11. **Security invariant check (MANDATORY)** — which of S1–S8 govern this feature, and does the plan violate any of them even indirectly?

---

## Standing AETHER cognitive/ASI mandate

Long-term trajectory: subconscious background reasoning (F#71), verifier-based
self-improvement (F#72), the **metric lockbox** (F#68 / S3 — highest priority once
Section D is confirmed built, which it now is), and the full 12-agent Section G
roster. This is not a single task; it is an evolving capability goal tracked here.
Decompose into bounded DDRs only when a concrete slice is chosen.

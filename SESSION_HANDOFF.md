# PRADYOS — SESSION HANDOFF

> A fresh Claude Code session (possibly on a new account/machine) should read this
> file **in full** before touching anything. It captures the exact repo state and
> the non-negotiable contracts so the project continues with zero context loss.

---

## 0. RESUME INSTRUCTION (read this first, act in this order)

> **"NET-B (lwIP) AND Layer 6 (AETHER) are COMPLETE and CI-green. AETHER ships
> the agent layer (ADR-026 + DDR-AETHER): kernel action queue + append-only audit
> + per-process mem cap + syscall rate limit, 10 NSI calls (29–38), CAP_SOVEREIGN/
> CAP_AGENT, and the ring-3 daemon+agent — gates `smoke-aether`, `smoke-aether-
> queue`, `smoke-aether-sec` all pass (32 gates total). The **ring-3 socket NSI**
> (ADR-027) is also COMPLETE — 8 kernel proxy sockets + `SYS_SOCK_*` (39–42) +
> agent live mode (Ollama over HTTP), with the boot page tables relocated to
> 0x300000 for durable kernel-growth headroom; `make smoke-agent-live` is the
> developer-run live gate (CI stays test-mode). **Layer 7 has STARTED** with the
> Sovereign/Manual toggle binding (DDR-701): PRISM `mode` builtin + the daemon's
> `SYS_SET_MODE` self-check, gate `smoke-mode`. **Layer-7 slice 0 (VirtIO-GPU
> framebuffer, ADR-028) is COMPLETE** — `kernel/drivers/gpu/virtio_gpu.c` brings up
> scanout 0 + presents a BGRA framebuffer (gate `smoke-gpu` →
> `PRADYOS_GPU_FB_OK 1024x768`; 33 CI gates). The framebuffer prerequisite is
> satisfied. The **ring-3 framebuffer surface (DDR-702) is also COMPLETE** —
> `SYS_FB_INFO/MAP/FLUSH` (43–45) let a ring-3 program map the GPU front buffer,
> draw, and present (gate `smoke-fb` → `PRADYOS_FB_DRAW_OK`; 34 CI gates; stage-2
> load raised to 16 chunks/512 KiB). **PS/2 keyboard input (DDR-703) is also
> COMPLETE** — `SYS_INPUT_POLL` (46) + a real IRQ1 path, gate `smoke-input` injects
> keys via QEMU `sendkey` (35 CI gates). Ring 3 can now both **draw to the screen**
> and **read the keyboard**. **The in-house sovereign-desktop compositor (DDR-704)
> is also COMPLETE** — `user/compositor.c` (CAP_SOVEREIGN) renders the mode's
> desktop over the framebuffer with an embedded 8×8 font and flips Sovereign/Manual
> on the `s`/`m` keys (gate `smoke-compositor`, 36 CI gates). It is the sole FB
> consumer (fbtest was folded in). **The virtio-input pointer (DDR-705) is also
> COMPLETE** — `SYS_MOUSE_POLL` (47) + a virtio-tablet driver; the compositor draws
> a cursor on click (gate `smoke-mouse` via QMP `input-send-event`; 37 CI gates).
> **PRADYOS now renders a keyboard- and pointer-driven desktop that reflects the
> kernel mode.** **Per-client surfaces (DDR-706) are also COMPLETE** —
> `SYS_SURFACE_*` (48–52) + a 16-entry PMM surface table shared by physical mapping;
> `user/surfacetest.c` commits a window the compositor composites (gate
> `smoke-surface`; 38 CI gates). **Ring-3 apps can now render windows.** **Named-agent
> UI panels (DDR-707) are also COMPLETE** — the compositor renders the 8 named agents
> (KRYOS…SOLIN) as cards with active/inactive state tied to AETHER's 8-slot roster
> (new `SYS_AGENT_ROSTER` 53; daemon lights KRYOS); gate `smoke-agents`.
> **Surface z-order + focus + input routing (DDR-708) is also COMPLETE** — surfaces
> stack by `z`, one holds focus, and the compositor routes keys to the focused
> window (`SYS_SURFACE_RAISE`/`SENDKEY`/`GETKEY` 54–56); gate `smoke-focus` (40 CI
> gates). **Sun-driven OKLab ambiances + the animated toggle (DDR-709) are also
> COMPLETE** — the 4 time-of-day ambiances render with genuine OKLab interpolation
> (libm-free Newton-cbrt), selected by `SYS_CLOCK` (57, RTC seconds-since-midnight);
> gate `smoke-ambiance`. **Window decorations + drag-to-move (DDR-710) are also
> COMPLETE** — title bars + `SYS_SURFACE_MOVE` (58); drag a window by its title bar
> (gate `smoke-drag`, 42 CI gates). **PRADYOS now has direct-manipulation windows
> (stack, focus, key routing, drag-to-move) on a perceptually-correct time-of-day
> desktop with an agent roster.** Next: visual richness (particle fields / glass
> blur / gradients) or window close/resize + title strings. wlroots/Wayland remain
> out-of-tree ports (brief §12 7b+, the wall). Read this
> file in full, run `graph_session_primer()`, confirm gates green, and write the
> ADR/DDR before any code. Do NOT restart earlier slices."**

Concretely:
1. Read this whole file (esp. §0.1 — current state).
2. `graph_session_primer()` — or `node tools/graph_mcp/server.js primer`.
   (Graph node_modules already installed here; fresh clone: `cd tools/graph_mcp
   && npm ci && node server.js rebuild`.)
3. Run the full gate set (§6) **plus** `smoke-fpu smoke-init smoke-shell` and
   confirm green before editing.
4. **NET-B (lwIP) is COMPLETE and CI-green** (**ADR-025** + **DDR-NET-B**; lwIP
   2.2.1 pinned at `third_party/lwip` `77dcd25a`). liblwip.a (`-w -nostdlibinc`)
   is **linked into the kernel** alongside the first-party port
   `third_party/lwip-port/lwip_port.c` (`-Werror`): allocator/rand/diag/assert/
   `sys_now` shims, the virtio-net⇄netif bridge (`pradyos_linkoutput` TX,
   `pradyos_netif_rx`→`ethernet_input`), static IP 10.0.2.15/24 GW 10.0.2.2,
   `net_init` wired into kmain + `sys_check_timeouts`/`netif_poll_all` on the PIT
   tick. The NET-A RX drop was root-caused (buffer recovered from
   `vq->desc[head].addr`, delivered past the 12-byte hdr, descriptor re-armed; TX
   chains freed). Gates **`smoke-net-lo`** (`PRADYOS_NET_LO_OK`), **`smoke-net`**
   (host→guest TCP echo on :8007, `PRADYOS_NET_TCP_OK`), **`smoke-net-fuzz`**
   (malformed frames + SYN flood, no panic, `PRADYOS_NET_FUZZ_OK`) — all in CI.
   **Layer 6 (AETHER) is also COMPLETE and CI-green** (ADR-026 + DDR-AETHER):
   `kernel/aether/` (queue/audit/mem/rate, PMM-pool rings), NSI 29–38 in
   `kernel/syscall/sys_aether.c`, `CAP_SOVEREIGN`/`CAP_AGENT`, and the ring-3
   `user/aether_daemon.c` + `user/agent_base.c`. Gates `smoke-aether`,
   `smoke-aether-queue`, `smoke-aether-sec` in CI (32 gates total). Deferred:
   live Ollama (needs a ring-3 socket NSI), SFS config read, IPC console.
   **Begin Layer 7 (UI/UX) next, or a socket NSI for live inference.**

**5d/5e closed this session** (HEAD `9f310da`): per-thread FPU save (ADR-023 §D8),
pradyos-init PID 1 (ADR-023 §5d), and the **PRISM shell** (ADR-024) — interactive
ring-3 shell over the serial console, builtins help/echo/cat/run/ls/ps/exit, with
console RX (IRQ4 ring buffer) and **full-register fork** now in the kernel.

**Open follow-ups (deferred, see ADR-024 / build_status):**
- **FAT32 large-file read / `execve` of a large musl-C ELF corrupts** — PRISM is
  kernel-spawned from SFS instead; init-`execve` respawn waits on this fix.
- `ls`/`ps` are stubs (need `SYS_GETDENTS` / a process-table syscall); RX line
  discipline/echo; pipes/redirection/quoting/job-control/scripting.

### 0.-23 SESSION — OPEN-11: PROVEN memory corruption, probe text overwritten by data

**Diagnostics added to `kernel/idt.c` (thread NAME + the 16 bytes actually
executing at the faulting RIP). They ended three sessions of guessing.**

Captured failing run:
```
#PF pid=13 name=WXVIOL.ELF rip=...07 err=0x7 cr2=...00  bytes@rip=C6 00 90 ...   <- expected W^X test
#PF pid=25 name=METRIC.ELF rip=...151 err=0x7 cr2=7FFFFFEFF040 bytes@rip=C7 40 40 EF BE AD DE ...  <- writes 0xDEADBEEF, a test
#PF pid=29 name=SHA256 rip=0x80000000B0 err=0x5 cr2=0x0
     bytes@rip= 03 0A 11 18 1F 26 2D 34 3B 42 49 50 57 5E 65 6C
```

**pid=29 is confirmed SHA256, and the bytes at its entry are NOT CODE.**
`03 0A 11 18 1F 26 ...` is the arithmetic sequence **7n+3**. `03 0A` decodes as
`add (%rdx),%ecx`, dereferencing `rdx=0` — which is exactly the reported
`cr2=0x0`. The process is executing a **data pattern**.

Note the trap also *changed shape* between runs (`#GP` at `0xB4` in one run,
`#PF` at `0xB0` in another) — consistent with corruption, not with a fixed bug.

### Who writes 7n+3

```
kernel/main.c:765   ((volatile uint8_t *)(uintptr_t)w)[i] = (uint8_t)(i * 7 + 3);
user/bigwritetest.c:46  pat[i] = (char)(i * 7 + 3);
```

`kernel/main.c:747,763` — `blk_selftest` — allocates **three pages via
`pmm_alloc_page()` and never frees any of them**, writes the pattern into one,
and `blk_write`/`blk_read`s it to LBA 1500.

`ptnode_alloc()` (kheap.c:286) zeroes each frame, and `elf_load` copies the ELF
bytes in afterwards, so the text page IS correct at load time. **The pattern must
therefore be written after the load** — i.e. the frame backing SHA256's text is
owned by two things at once.

Per memory `kmain-boot-race-user-threads`, kmain is still executing (and reaches
`blk_selftest`) while probes are already spawned and running. That is the window.

### UNRESOLVED detail that must be explained before any fix

Pattern index 0 (`0x03`) sits at **page offset 0xB0**, not offset 0. A writer
doing `w[0..511]` on a page-aligned frame would put index 0 at offset 0. So this
is not a plain "same frame handed out twice" — either the write lands at an
offset, or the page content arrived via the `blk_read` of LBA 1500 rather than
the direct write. **Do not write a fix until this is explained**; the last two
root causes I committed were both wrong because I stopped at "plausible".

### Next
1. Instrument `pmm_alloc_page`/`ptnode_alloc` to record owner, or add a check that
   a frame handed out is not already mapped in a live address space.
2. Determine whether `blk_selftest`'s leaked pages (747, 763) are the same frames
   later handed to `elf_load` — print the physical addresses of both.
3. `blk_selftest` leaking three pages every boot is a real bug regardless, but it
   is NOT yet proven to be OPEN-11's cause.

### 0.-22 SESSION — RETRACTION: the "corrupted saved RIP" conclusion was WRONG

**Commit `5ab7d4f` claimed OPEN-11 was a corrupted saved RIP across a context
switch. That is FALSE. Do not act on it. No scheduler fix is warranted by it.**

### What actually produces `#PF rip=0x8000000007 cr2=0x8000000000 err=0x7`

`build/wxviol.elf` — the **W^X violation test** — disassembles to:

```
8000000000: 48 8d 05 f9 ff ff ff  lea  -0x7(%rip),%rax    # = 0x8000000000
8000000007: c6 00 90              movb $0x90,(%rax)       <-- writes its own text
```

That is the gate **working as designed**: a ring-3 store into an R+X page,
correctly refused. `0x07` IS an instruction boundary. My "mid-instruction, so the
RIP must be corrupt" proof came from disassembling the shared `_start` stub
instead of `wxviol.elf` — every probe loads at the same base, so an address alone
never identifies a binary. That is the **same mistake** that made me disassemble
`sha256test` for the `#GP`.

**Consequence:** `smoke-rqstress-liveness` is a *liveness/timeout* failure — its
sentinel `[smp] rqstress OK` never printed — and its only trap is the benign
wxviol one. It is **NOT** the same bug as OPEN-11. Retract that too.

### Also dead this session
- **#GP frame misreporting**: vector 13 uses `ISR_ERR` in `arch/x86_64/isr.asm:58`
  (no dummy error code pushed), so the frame layout is right and the reported RIP
  is trustworthy. Not a diagnostic artefact.

### What is genuinely still unknown

`#GP pid=29 rip=0x80000000B4`. **FOUR** ELFs share entry `0x80000000b0` —
`timetest`, `metrictest`, `sha256test`, `sfsroottest` — so pid=29 has **never
been identified**. Every statement I made about "sha256test's `_start`" was an
assumption from a shared address.

At `0xb4` all four have `and $-16,%rsp`, which genuinely cannot `#GP`. Either the
faulting process is none of those four, or something about the entry state is
wrong. **This is unresolved. Do not invent a mechanism for it.**

### NEXT STEP — get identification before theorising again

The trap printer (`kernel/idt.c`, ~line 224-260) prints pid but not the thread
name, while `elf_load` already names every thread ("SHA256", "TIME", "SFSROOT",
"WXVIOL", ...). **Add the name to the trap line.** That is a legitimate
diagnostic improvement, not a workaround, and it ends three sessions of guessing
which binary faulted. Only then reason about the mechanism.

### Standing lesson (third instance)

**An address does not identify a binary when every binary loads at the same
base.** Confirm which ELF is running before disassembling anything. This has now
produced two wrong root causes (PMM exhaustion, corrupted RIP), both of which I
committed as conclusions before this check.

### 0.-21 SESSION — OPEN-11 IDENTIFIED: corrupted saved RIP across context switch

**OPEN-11 and the `smoke-rqstress-liveness` failure are THE SAME BUG.**
Latest CI (run 30928234191, `59941d9`): 10 of 11 jobs green, shard 1 red — but
this time it failed at **gate 1 of 32, `smoke-rqstress-liveness`**, not
`smoke-sha256`. Identical trap signature:

```
smoke-sha256            [trap] #PF pid=13 rip=0x8000000007 cr2=0x8000000000 err=0x7
smoke-rqstress-liveness [trap] #PF pid=21 rip=0x8000000007 cr2=0x8000000000 err=0x7
```
Same rip, same cr2, same error code, different gate, different pid.

### THE PROOF: rip=0x...07 is NOT an instruction boundary

The `_start` stub (shared by 14 programs — init, prism, compositor,
aether_daemon, agent_base, cmusl, ...) begins:

```
8000000000: 48 31 ed            xor %rbp,%rbp
8000000003: 48 89 e7            mov %rsp,%rdi
8000000006: 48 8d 35 f3 ff ff ff lea -0xd(%rip),%rsi     <-- spans 0x06..0x0C
800000000d: 48 83 e4 f0          and $-16,%rsp
```

**`0x07` is the second byte of the `lea`.** A thread executing normally can never
have `rip = 0x07`. The CPU is decoding misaligned garbage starting mid-instruction,
which is why the resulting fault is an absurd *write* to the base of its own text.

This retroactively explains the `#GP` at `0x...B4` in `sha256test` too: same
corruption, different offset. `and $-16,%rsp` never faulted — the CPU was not
executing `and $-16,%rsp`.

### Therefore

The saved RIP is being corrupted **across a context switch**, and the corrupting
event hits several address spaces in one run (pids 13, 25, 29 together). This is
a scheduler/context-switch defect, NOT a crypto, layout, memory-pressure or
build defect — and `smoke-rqstress-liveness` is a *runqueue stress* gate, exactly
where such a defect should surface first.

### Where to look

- `kernel/proc/sched.c` context switch / `finish_task_switch` / `thread_trampoline`
- Prior art in this exact area, all suggestive: `a49bb65` "rq double-enqueue —
  atomic cross-queue rq_on claim (DDR-736)", `4a9ca0b` "thread_trampoline keeps
  on_cpu set until finish_task_switch (rq-2)", `f1ec096` lazy FPU.
- A TCB enqueued on two runqueues, or resumed while still `on_cpu`, would let two
  paths write one saved-state area — producing exactly a torn/misaligned RIP.
- memory `tcb-fields-not-zeroed`: kmalloc does not zero; an uninitialised field
  consumed as saved state gives the same signature.

### Every other hypothesis is dead (measured, not argued)
regressing commit (4 exonerated) · stale build · FS-gate image corruption ·
unaligned movaps (zero SSE in probe) · DDR-826 writable globals (no probe ELF has
a writable PT_LOAD) · probe_rodata_check skip · PMM exhaustion (DDR-829 built and
REVERTED) · disk persistence (images re-mkfs'd every run) · SMP race (all gates
are already `-smp 1`; only `smoke-smp` sets `QEMU_SMP`).

### Shard status, latest run
shard 0,2,3,4,5 green · aether-layer, code-graph, shard-check, both
arch-bootstrap green · **shard 1 is the only red, and it is this one bug.**

### 0.-20 SESSION — OPEN-11: PMM root cause REFUTED by my own fix

**DDR-829 (256 KiB stack) was implemented, measured, REVERTED.** Three runs after
the fix: **PASS, FAIL, FAIL** — identical `#GP` at pid=29. The premise was wrong:

```
free frames after release=0x0000000000006F53  (balanced)
```

PMM is nearly full and balanced; processes exit and release, so the ~29 threads
never coexist and the "~13 process ceiling" never applies. `AGENT_OOM_KILLED` is
a deliberate OOM *test* (`AETHER_SEC_OOM_OK` follows it), not real pressure.

**Also excluded: disk state.** `fat-image`/`sfs-image` are phony targets that
`dd`-zero and re-`mkfs` on EVERY gate invocation. Every run starts clean.

### NEW decisive facts

1. **Failure rate is roughly 1 in 3**, same binary, same command, back to back.
2. **A PASSING run emits ZERO `[trap]` lines.** Failing runs emit exactly three.
   So the two `#PF`s are NOT intentional W^X negative tests — they are part of
   the failure, and they precede the `#GP`.
3. `[trap] #PF pid=13 rip=0x8000000007 cr2=0x8000000000 err=0x7` — a process
   **writing to the base of its own text page**, 7 bytes into execution.
   err=0x7 = user + write + present.

Three different processes (13, 25, 29) all misbehave in the same run and none
misbehaves in a passing run. That is not three bugs; it is one event corrupting
several address spaces at once.

### Where to look next (highest value first)

- ~~SMP address-space race~~ — **ALREADY EXCLUDED, do not spend a run on it.**
  `boot_test.sh:80-84` adds `-smp` only when `QEMU_SMP` is set; every gate except
  `smoke-smp` already runs on QEMU's default **single CPU**. There is no second
  CPU to race with. This was killed by reading the runner, not by testing.
- **Entropy / uninitialised memory is now the prime suspect.** On a single CPU
  with a fixed image, the only things that vary run to run are host-timing and
  virtio-rng entropy. Combined with memory `tcb-fields-not-zeroed` (kmalloc does
  not zero; every new `struct tcb` field needs an explicit initialiser in
  `sched_create`), a garbage field read as a pointer/rip would produce exactly
  this: a process executing or writing at a wrong address, ~1 run in 3.
- **What `98fd2f8` actually did** (it is where CI first went red): `kernel/main.c`
  +12 (spawn the acc probe), `user_image.asm` +6, `Makefile`, `acctest.c`. It
  added **one more spawned process**. It did not touch `sched.c`/`sched.h`. So it
  did not introduce the bug — it shifted pid numbering and timing enough to
  expose a latent one. Do not go looking for the defect inside that diff.
- `ptnode_alloc()` handing out a frame that is still mapped elsewhere (frame
  reuse without invalidation) would explain a process executing/writing another
  process's memory.

**Do NOT retest:** a regressing commit (all 4 exonerated), stale builds, FS-gate
image corruption, unaligned `movaps` (zero SSE in the probe), DDR-826 writable
globals (no probe ELF has a writable PT_LOAD), probe_rodata_check's skip, PMM
exhaustion (refuted above), disk persistence between runs (refuted above).

### 0.-19 SESSION — 2026-08-04 (CURRENT — read this first)

## CI AUDIT OF THE LAST 25 RUNS — 14 red, but only 5 distinct bugs, 4 already closed

| gate | red runs | status |
|---|---|---|
| `smoke-syscallfuzz` | 6 | **FIXED** — verified `PASS smoke-syscallfuzz (27s)` in green run 30837801892 |
| `smoke-ed25519` | 5 | **FIXED** (DDR-826) — verified `PASS smoke-ed25519 (150s)` |
| `smoke-resched` | 1 | **FIXED** — verified `PASS smoke-resched (120s)` |
| `smoke-blkmq-trace` | 1 | **FIXED** — verified `PASS smoke-blkmq-trace (180s)` |
| **`smoke-sha256`** | **5** | **OPEN-11, still red** |

All four fixed gates were checked against `EXCLUDE` in `tools/ci/shard_check.sh`
(`smoke-aarch64 smoke-riscv64 smoke-agent-live smoke-selftest smoke-sfs-btree-smp4`)
— **none of them is excluded**. They pass because they were repaired, not hidden.

## OPEN-11 ROOT CAUSE — PMM CANNOT SUPPORT THE NUMBER OF PROCESSES WE SPAWN

**This is a genuine resource-exhaustion bug, not a crypto or layout bug.**

```
NEXUS: PMM (buddy) free frames=0x0000000000006F56     = 28,502 frames ~= 111 MiB
kernel/exec/elf.c:190  maps the FULL 8 MiB user stack EAGERLY, page by page
                       8 MiB / 4 KiB = 2,048 frames PER PROCESS
                       28,502 / 2,048 = ~13 processes MAXIMUM
this boot spawns       ~29 ring-3 threads
```

The ceiling is ~13 and we ask for ~29. Adding the `acctest` probe in `98fd2f8`
pushed an already-overcommitted boot further over the edge — which is exactly why
"adding one probe broke a *different* probe", and why it is nondeterministic:
which process loses the allocation race depends on scheduling order.

Corroborating evidence in the same serial log:
```
AGENT_OOM_KILLED PID=2742943744      <-- garbage PID (0xA37Fxxxx)
AETHER_SEC_OOM_OK
```
That PID is uninitialised — the `tcb-fields-not-zeroed` class from memory. The
OOM path is being taken for real, and it reports a nonsense PID when it fires.

### Hypotheses REFUTED this session (do not retest — all measured, not argued)
- a regressing commit: all four suspects exonerated; `17c3858` touches **no kernel source**
- stale incremental build: clean worktree builds fail too
- FS gates corrupting the kernel tail: checksums identical before/after; control arm failed first
- `movaps`/`movdqa` unaligned `#GP`: `objdump` shows **zero** SSE/AVX instructions in the probe
- writable globals (DDR-826 class) in the probe: **no** probe ELF has a writable PT_LOAD (all 42 are R+X)
- probe_rodata_check hiding something via its skip: nothing is skipped

### THE FIX (next step, not yet applied)
Make the 8 MiB user stack **lazily mapped** (demand-paged) instead of eagerly
mapping 2,048 frames at `elf_load` time — map only the top page (which
`elf.c:207` already needs for the argv/auxv frame) plus a guard page, and fault
the rest in. That drops per-process cost from 2,048 frames to ~2 and lifts the
ceiling far above the ~29 we spawn.
Then separately fix the uninitialised PID in the `AGENT_OOM_KILLED` path.

**Do not "fix" this by giving QEMU more RAM.** That hides a real kernel limit
that would still bite on constrained hardware.

### Housekeeping
- Dependabot: 5 alerts (2 high, 3 moderate) — seen, NOT triaged, human decision
- untracked `.claude/`; three prunable `claude/*` worktrees
- worktrees added: `pradyos-bisect` (98fd2f8), `pradyos-tip` (1b401d9)
- transport fixed: scripts to a file, CRLF-stripped, run by path, `MSYS_NO_PATHCONV=1`.
  `wsl bash -lc '<multiline>'` silently blanks shell variables — it manufactured the
  phantom "HEAD mismatch" of the previous session.
- `readelf`/`objdump` do not exist in Git Bash; a `grep -c` over their missing output
  returns 0 and reads as "clean". Run binutils inside WSL only.

### AGS stays INERT. ACC stays ⚠️.

### 0.-18 SESSION — 2026-08-04 (CURRENT — read this first)

## OPEN-11 — MAJOR REFRAME: it is NOT a source regression. It is NONDETERMINISTIC.

**All four suspect commits are EXONERATED. Stop bisecting.**

| fact | evidence |
|---|---|
| `98fd2f8` clean worktree build | `smoke-sha256` **PASS** |
| tip `1b401d9` clean worktree build | `smoke-sha256` **PASS** |
| `17c3858` (the "ACC ship" commit) | touches **NO kernel source** — only SESSION_HANDOFF.md, 2 docs, gate_shards.txt, shard_check.sh. It cannot change one byte of the image. |
| `bef93c2`, `148e969` | CI **success**; also ancestors of the passing `98fd2f8` |

**THE DECISIVE OBSERVATION.** In the SAME worktree, with a byte-identical image
(sha256 of `build/kernel.bin` and `build/pradyos.img` captured before and after
and IDENTICAL), `smoke-sha256`:

  * PASSED when run right after `make image`
  * **FAILED** on a later invocation with no rebuild in between

Same bytes. Different outcome. **OPEN-11 is a race/nondeterminism, not a build
or layout defect.** Every deterministic explanation is therefore dead.

### Hypotheses now REFUTED — do not retest any of these

1. runner slowness — reproduces locally
2. boot latency — `smoke` passes in 2 s
3. `TIMEOUT_S` — recipe's inline 90 s wins; it ran the full window
4. stale `user_image.o` — newer than every probe ELF
5. incbin misalignment — all probe symbols 8-aligned
6. **stale incremental build** — clean worktree builds fail too
7. **a regressing commit** — all four exonerated above
8. **FS-write gates corrupting the kernel tail** — tested directly: ran
   `smoke-fs-rw` between two `smoke-sha256` runs; **checksums unchanged**, and
   the control arm failed *before* the FS gate ever ran

### THE ACTUAL LEAD — three traps, always at the same three RIPs

```
[trap] user #PF pid=13 rip=0x0000008000000007 cr2=0x0000008000000000 err=0x7
[trap] user #PF pid=25 rip=0x0000008000000151 cr2=0x00007FFFFFEFF040 err=0x7
[trap] user #GP pid=29 rip=0x00000080000000B4
```

`err=0x7` = **user-mode WRITE to a present page** → this is the **DDR-826
writable-global class**, and it is affecting **more probes than ed25519**.
pid=13 stores to `0x8000000000` — the load address, i.e. writing to its own
R+X text page. That is the same bug DDR-826 fixed once, still present elsewhere.

Why intermittent: per memory `kmain-boot-race-user-threads`, user threads run
while kmain is still booting. Which probes get spawned/scheduled varies run to
run, so whether the bad store executes before the checkpoint varies.

### NEXT STEP (literal)

Run `tools/ci/probe_rodata_check.sh` against **every** probe ELF, not just the
ones it currently covers, and check whether `sha256test`/pid-13's probe has a
writable global landing in the R+X PT_LOAD. Note that check **skips** ELFs that
have a writable PT_LOAD — verify that skip is not hiding these.

Then disassemble `sha256test.elf` at `0xb0..0xb8` **as loaded from the image**
(not from `build/sha256test.elf`) to see the bytes actually executing.

### Housekeeping (flagged, not blockers)
- 5 Dependabot alerts (2 high, 3 moderate) — **seen, not triaged, human decision needed**
- untracked `.claude/`; three prunable `claude/*` worktrees
- two worktrees added this session: `pradyos-bisect` (98fd2f8), `pradyos-tip` (1b401d9)
- **transport bug fixed**: `wsl bash -lc '<multiline>'` silently blanks shell
  variables. All scripts now written to a file, CRLF-stripped, run by path, with
  `MSYS_NO_PATHCONV=1` (Git Bash rewrites `/mnt/...` args otherwise).

### AGS stays INERT. ACC stays ⚠️. Both correct until OPEN-11 closes.

### 0.-17 SESSION — 2026-08-04 (CURRENT — read this first)

**`dev/phase1` = `17c3858` + this commit. `main` = `3b4830a`.**

#### 🔴 OPEN-11 (NEW, BLOCKING) — `smoke-sha256` regressed on the tip

**Do not treat ACC as safely shipped until this is resolved.** CI attempt 1 on
`17c3858` FAILED, and it reproduces locally, so this is not runner speed.

**Evidence, all verified this session:**

```
CI attempt 1, shard 1:  FAILED at smoke-sha256 after 10 of 32 gates
                        06:40:49 -> 06:42:19  = exactly 90 s (its window)
LOCAL:                  smoke-sha256 FAIL, elapsed 93 s
LOCAL serial log:
  [user] ELF loaded (embedded); SHA-256 vector probe spawned
  [trap] user #GP general protection pid=29 rip=0x00000080000000B4
```

`sha256test.elf` entry point is `0x80000000b0`, so `rip=0xb4` is **4 bytes into
`_start`** — at `and $0xfffffffffffffff0,%rsp`, the `force_align_arg_pointer`
prologue. **An `and` on `%rsp` with an immediate cannot itself `#GP`.** That is
the central puzzle.

**Hypotheses RULED OUT — do not re-test these:**

| hypothesis | how it was killed |
|---|---|
| CI runner slowness | reproduces locally at 93 s |
| DDR-827's chunk raise slowed boot | **`smoke` PASSES in 2 s** — boot is fast |
| `TIMEOUT_S=180` would fix it | the recipe's inline `TIMEOUT_S=90` wins (0.-10 finding); it ran at 90 s |
| stale `user_image.o` (DDR-822/825 class) | `user_image.o` is **newer** than every probe ELF |
| incbin misalignment from adding `acctest` | all three probe symbols are 8-byte aligned |

**Known-good reference:** CI was **green twice on `fd876cd`** (runs
`30878361148`, `30879247169`), and shard 1 contains `smoke-sha256`. So the
regression is in exactly four commits: `bef93c2` (DDR-828 timeouts — did **not**
touch `smoke-sha256`, which is 90 s), `148e969` (docs), `98fd2f8` (adds the
`acctest` probe to the image), `17c3858` (ACC linked + gate registered).

**`98fd2f8` is the prime suspect** — it is the one that changes the image.

**BISECT DATAPOINT 1 (confirmed this session):** `smoke-sha256` **PASSES at `fd876cd`**
in a clean build — so `fd876cd` is a genuine known-good, not merely "CI was green".
The search space is exactly `bef93c2`, `148e969`, `98fd2f8`, `17c3858`.

**BISECT DATAPOINT 2 (INCONCLUSIVE — read the caveat):** the `98fd2f8` worktree
**failed to build**, so the gate never ran. Two things must be sorted before
trusting any rerun:

1. `make image` output was suppressed with `>/dev/null 2>&1`, so the failure
   reason was not captured. **Do not suppress it on the retry.**
2. **HEAD mismatch:** git printed `HEAD is now at 98fd2f8` but
   `git rev-parse --short HEAD` in that worktree returned **`abbd763`**. Resolve
   this before believing any result from that tree — a bisect measuring the
   wrong commit is worse than no bisect. Check whether the build simply needs
   `make toolchain-check` / a clean `build/` in a fresh worktree, and whether the
   worktree path under `Projects/pradyos-bisect` collides with anything.

A worktree build may also fail for reasons unrelated to OPEN-11 (absolute paths,
missing `build/` state). If it will not build cleanly, bisect instead by checking
out each commit in the main tree and restoring afterwards — but record the tree
state each time, since a stray detached HEAD already cost one session.

**Next exact command** (a bisect was attempted and left the tree on a detached
HEAD; it has been restored, but run this in a worktree instead):
```
git worktree add /tmp/bisect fd876cd && cd /tmp/bisect && make image && make smoke-sha256
```
Then `98fd2f8` and `148e969`. Whichever first shows the `#GP` is the culprit.

**A cheap discriminator worth trying first:** disassemble `sha256test.elf`
around `0xb0` **from the image** (`build/kernel.bin`) rather than from
`build/sha256test.elf`, and compare. If they differ, something is corrupting the
embedded copy despite the timestamps.

#### CI attempt 2 was at 10/11 green with shard 1 outstanding when this was written — read its conclusion first; it may contradict attempt 1.

#### AGS (DDR-814) — written, INERT, not wired

`kernel/aether/ags.{c,h}` and `kernel/syscall/sys_ags.c` are committed but
**deliberately not wired**: no NSI numbers assigned, not in the kernel link, not
registered, no gate. They cannot affect the build. Committed so the work is not
lost; wiring waits until OPEN-11 is resolved, because adding another probe to
the image is exactly the change under suspicion.

Design decisions already made and worth keeping:
- **`SYS_GOAL_SIGN` = CAP_SOVEREIGN, `SYS_GOAL_VERIFY` = CAP_AGENT** — the
  inverse of ACC's split, for the same reason: the privileged direction is
  whichever an attacker would want. If an agent could sign, it could authorise
  its own goals and the audit log would show a valid signature on something
  nobody approved (S1).
- Goal is hashed with SHA-256 first, so any goal length costs one fixed-size
  signature and the audit log can store the 32-byte hash.
- `AR_GOAL_SIGNED` / `AR_GOAL_REJECTED` recorded distinctly from
  `AR_CAP_DENIED`: a forged goal and a policy refusal are different facts.

#### Next, in order

1. **OPEN-11 first.** Nothing else should land on a tip with a red gate.
2. Then wire AGS (NSI 79/80 + `AR_GOAL_*` audit codes + probe + gate).
3. Then DDR-815 rotation (81), then Parts B–G of the queue.

#### Unchanged

**B#3 / OPEN-10:** still **no `op=` line**. Untested, not refuted.

### 0.-16 SESSION — 2026-08-04 (earlier)

**`main` = `3b4830a`. `dev/phase1` = `98fd2f8`** + this handoff commit.
NSI 77/78 registered+linked. Next free **79**. 121 gates, **6** excluded.

#### ⭐ CI AUDIT — all 8 reds attributed, largest cause fixed (DDR-828)

Every red run in the last 48 h was opened to its failing job, step and error
string. **They were not one defect:**

| gate | runs | verdict |
|---|---|---|
| `smoke-ed25519` | `c68dc24`, `5e551d8`, `df248af` ×3 | **EXPECTED** — DDR-826, deliberately left registered-and-red rather than hidden. Stopped at `ad8a3a4`, the fix. |
| **`smoke-syscallfuzz`** | **7 of 8** | **A TIMEOUT, not a defect.** Fixed. |
| `smoke-resched` | `ad8a3a4` only | 1 occurrence → OPEN-2, triaged, **not** claimed fixed |
| `smoke-blkmq-trace` | `df248af` only | 1 occurrence → OPEN-2, triaged, **not** claimed fixed |

`smoke-syscallfuzz` died at **exactly 60 s** (17:54:56 → 17:55:56) with
`TIMEOUT_S=60`, and `PRADYOS_NET_FUZZ_OK` is in the same log — the kernel was
alive and progressing. Measured locally: **25 s**. A 2.4× margin here, not
enough on a shared runner as the image grew (every probe ELF is `incbin`'d in;
ed25519 + aead added ~40 KB plus boot-time spawns).

**Matching by identity mattered.** "Required pattern not found" is the *same
string* DDR-826 produced, and DDR-826 *was* a real defect. Reading the
timestamps rather than the message separated them.

**Fix is the class, per DDR-788's own precedent.** A gate with no
`FORBIDDEN_SENTINEL` is early-exit eligible, so a larger window is **free on
success**. DDR-788 claimed to have "retired the timeout-margin flake class" but
**17 gates were still at 60 s**. Eleven eligible ones raised to 120 s. Five are
**deliberately left at 60 s** (`smoke-kill`, `smoke-fpu`, `smoke-net-tcp-lo`,
`smoke-fs-budget`, `smoke-nvme`) — they declare `FORBIDDEN_SENTINEL`, burn the
whole window every run, and raising them would cost 5 × 60 s of real CI
wall-clock for gates that are not failing.

**Rule earned:** *a gate's timeout is a claim about how long the system takes,
and it goes stale as the system grows. When a gate fails on "pattern not found",
check elapsed against the window BEFORE reading the code.*

#### smoke-acc — written and wired, NO VERDICT YET

`user/acctest.c` + `smoke-acc`, five checks: round-trip · tampered ciphertext
(AEAD tag) · tampered signature (Ed25519) — **separate arms on purpose** ·
replay → `ACC_ERR_REPLAY` · **owner-read-after-reboot with `last_seq = NULL`**,
which is what BUG-1 exists for and the only arm that catches a verify key kept
outside the envelope. Plus a next-seq-accepted check so the replay arm cannot be
satisfied by an `open()` that rejects everything.

**PASSES — `rc=0`, 152 s.** Verdict landed just before the handoff was
committed. Exclusion removed, registered in shard 5 (121 gates, 5 excluded).
**ACC is shipped**: syscalls 77/78 linked and registered, gate green.

**Done in this session** — unexcluded and registered.|FAIL)" build/gatelogs/acc.log; grep -a "ACC_STUB" build/gatelogs/acc.log'
```
If PASS → remove `smoke-acc` from `EXCLUDE` in `shard_check.sh`, add
`5<TAB>smoke-acc<TAB>150` to `gate_shards.txt`, and **ACC is finally shipped**.
If it shows `ACC_STUB arm=<name>`, the failing arm names itself.

`kernel.bin` is 840,038 B against the 1,048,576 limit — DDR-827's window absorbs
the probe with 208 KB spare.

#### Next, in order

1. **DDR-814 AGS** (NSI 79/80) — Ed25519 over goal state; arms: valid
   sign+verify, tampered goal fails, wrong key fails.
2. DDR-815 ACC rotation (NSI 81).
3. TASK 9–21.

#### Unchanged

**B#3 / OPEN-10:** still **no `op=` line**. Untested, not refuted.
Ruled out and not to be retried: `IRQF_PERCPU` (no analogue), `sfs.c` spinlock
(no global mutable state).

### 0.-15 SESSION — 2026-08-03 (earlier)

**`main` = `3b4830a`. `dev/phase1` = `eb2c2f1`** + this handoff commit.
**NSI 77/78 assigned and REGISTERED.** Next free **79**. 121 gates, 5 excluded.

#### ⭐ DDR-827 — the load window was full, and ACC did not fit

ACC is the first feature needing the **whole** crypto stack resident. Linking
`acc.o` + `x25519` + `fe25519` + `hkdf` + `aead` + `ed25519` + `sha512` +
`sys_acc.o` took `kernel.bin` **774,502 → 799,078 bytes**, past the DDR-733
limit of 786,432. The image built, the size gate caught it, **and it did not
boot** — stage 2 read only 24×64 sectors from LBA 17, so the kernel's tail never
arrived.

**Three coupled numbers, moved in one commit:**

| file | was | now |
|---|---|---|
| `boot/stage2/stage2.asm:183` chunk count | 24 | **32** (= 1,048,576 B) |
| `Makefile` `truncate` on `$(IMG)` | 1M | **2M** |
| `Makefile` size gate | 786,432 | **1,048,576** |

The image growth is **not optional**: from LBA 17 a 1 MiB image holds only
1,039,872 B — *less* than the new window — so stage 2 would read past
end-of-file.

**32 chunks, not the 48 first designed — reduced after measuring.** The PT_HI
assertion caps image+BSS at `0x600000`; `__bss_end` sits at physical
`0x4cdf80`, leaving **1,253,504 bytes** of headroom. 48 would have been closer
to the ceiling for no benefit, and each chunk is another INT13 round trip.

**Verified with gates that touch the END of the image** — a short read corrupts
the tail, so an early-code gate would pass against a broken load:
`smoke` PASS · `smoke-user` PASS (7 FS patterns) · `smoke-fs` PASS (12 patterns)
· **`smoke-smp` PASS (4 patterns, EXIT=0)** — resolved after the handoff was
first written. All four DDR-827 verifications are satisfied.

**Sequencing worth copying:** the tree was reverted to a booting state and
committed *that* way first, so no commit ever left `dev/phase1` unbootable. The
window was then raised as a separate step.

#### ACC (DDR-813) — linked and registered, NOT shipped

- `kernel/crypto/acc.{c,h}` host-verified: seal/open round-trip, tamper-ct and
  tamper-sig → `ACC_ERR_AUTH`, replay → `ACC_ERR_REPLAY`,
  owner-read-after-reboot → `ACC_OK`. Both spec bugs fixed (agent_sign_pub
  in-band; Ed25519 and X25519 keys distinct).
- `kernel/syscall/sys_acc.c` — `SYS_ACC_SEAL` (77, CAP_AGENT) and
  `SYS_ACC_OPEN` (78, CAP_SOVEREIGN). The asymmetry is deliberate: opening
  reveals a *peer's* plaintext, so it is owner-only (S1). The signing seed is
  not a ring-3 parameter — it comes from the kernel, so a compromised agent
  cannot sign as another agent. Entropy varies the per-envelope sequence, never
  the seed (randomising the seed would change `agent_sign_pub` per call and
  break the owner's offline verify — the exact failure BUG-1 prevents).
  `-EAGAIN` replay vs `-EACCES` forgery stay distinct.
- **NOT SHIPPED: there is no `smoke-acc`.** That is the next task.

#### Next, in order

1. **`user/acctest.c` + `smoke-acc`**, four arms already proven on the host:
   success `ACC_OK`, tamper `ACC_ERR_AUTH`, replay `ACC_ERR_REPLAY`,
   owner-read-after-reboot `ACC_OK`. Probe must have **no writable globals**
   (DDR-826) — `ci-probe-rodata-check` will catch it, but write it right.
   Register in a shard only after it passes once.
2. Read `smoke-smp` (DDR-827 verification 4) and the two in-flight CI runs.
3. DDR-814 AGS (79/80) → DDR-815 rotation (81) → TASK 9–21.

#### Unchanged blockers

**B#3 / OPEN-10:** still **no `op=` line**. Three earlier harvest runs were
green; two more dispatched this session. Untested, not refuted. Still ruled out:
`IRQF_PERCPU` (no analogue here), spinlock in `sfs.c` (no global mutable state).

### 0.-14 SESSION — 2026-08-03 (earlier)

**`main` = `3b4830a`. `dev/phase1` = `5688db3`** + this handoff commit.
NSI max 76, next free **77**. 121 gates, 5 excluded.

#### ⭐ smoke-ed25519 is GREEN — and the failure was never the crypto

```
PRADYOS_ED25519_VECTORS_OK
```

**DDR-826.** `user/user.ld` links probes as a single **R+X `PT_LOAD` with no
writable segment**. `ed25519.c` cached its derived curve constants in
`static fe C_D` etc; those landed in a 264-byte `.lbss` (flags `WA`); **lld
placed the orphan inside the read-only segment and the link SUCCEEDED**. The
first store faulted:

```
[trap] user #PF pid=29 rip=0x80000035D4 cr2=0x8000004F80 err=0x7
       err = present | WRITE | USER   cr2 = 0xB0 past the end of .lrodata
```

The probe spawned and died in `curve_init()` **before printing a byte**, so the
gate said "required pattern not found" — which reads as *"Ed25519 is wrong"*. It
was not; every RFC 8032 vector passed on the host before and after.

**Two plausible hypotheses killed by measurement, not argument:**
- *"too slow under TCG"* — the probe costs **~3 ms on the host at -O2**; even at
  `-O0` with a 100× TCG penalty that is ~6 s against a 150 s window. A 25×
  margin. Plausible-sounding and wrong.
- *"the harness swallowed the verdict"* — killed by re-running with
  `SERIAL_LOG` under `build/gatelogs/` instead of `/tmp`, **which had been wiped
  mid-session** (exactly the flakiness `boot_test.sh` documents). The verdict was
  real and the serial log showed the probe spawning and producing nothing.

**Fix, two parts:** constants derive into a stack `ed_ctx` (no file-scope
mutable state; `.data`/`.bss` now size 0), and **`make ci-probe-rodata-check`**
fails the build if an ELF has a writable allocated section *and no writable
`PT_LOAD` to hold it*. The predicate matters — the first version flagged
writable sections outright and immediately failed on `init`/`prism`/
`compositor`/`aether_daemon`/`cmusl`, which are **correct** (they link with
`$(USER_C_LD)`, which does provide a writable segment). Rather than maintain a
list of which program uses which script, **the check asks the binary**. 43 ELFs,
none flagged. Wired into CI after `make image`.

**Seventh instance of the structural defect — and six of seven are build-time
silence.** The toolchain had the information and did not surface it. lld could
see a writable section had no writable segment; it placed it anyway.

#### ⭐ ACC (DDR-813) envelope written and host-verified

`kernel/crypto/acc.{c,h}`. All arms pass on the host:

```
seal ACC_OK · open ACC_OK (plaintext matches) · agent_sign_pub present
tamper ct ACC_ERR_AUTH · tamper sig ACC_ERR_AUTH
replay ACC_ERR_REPLAY · owner-read-post-reboot ACC_OK · next seq ACC_OK
```

Both spec bugs fixed at design time: **BUG-1** `agent_sign_pub[32]` is in the
envelope (without it the owner cannot verify after a reboot — the offline read
is the entire point); **BUG-2** the Ed25519 and X25519 keys are distinct fields
that never alias.

**NOT DONE:** `SYS_ACC_SEAL` (77) / `SYS_ACC_OPEN` (78) are not implemented and
**there is no `smoke-acc` gate.** This is host-verified code, not a shipped
feature. Do not treat it as gated.

#### B#3 / OPEN-10 — still no evidence

Three harvest CI runs concluded green earlier; **no `btree churn FAIL` occurred,
so no `op=` line exists.** The unification is **untested**, not refuted. Two more
runs were dispatched this session and were in flight at the end.

**Still ruled out, do not retry:** `IRQF_PERCPU` (no analogue here); a spinlock
in `sfs.c` (no global mutable state; VFS serialises per-mount).

#### Next, in order

1. **Wire ACC**: `SYS_ACC_SEAL` (77) + `SYS_ACC_OPEN` (78) in
   `kernel/syscall/`, `user/acctest.c`, gate `smoke-acc` with the four arms
   already proven on the host. Register in a shard **only after it passes once**.
2. Read the two in-flight CI runs for `op=` lines.
3. DDR-814 AGS (79/80) → DDR-815 rotation (81) → TASK 9–21.

**Reusable rules earned this session:**
- Gate logs go under `build/gatelogs/`, never `/tmp` — WSL wipes it mid-run.
- A probe that spawns and prints nothing is a **link-script violation** far more
  often than a logic bug. Check `err=0x7` and `cr2` against section addresses
  before suspecting the algorithm.

### 0.-13 SESSION — 2026-08-03 (earlier)

**`main` = `3b4830a`. `dev/phase1` = `c68dc24`** + a pending handoff commit.
NSI max 76, next free 77. **121 gates, 5 excluded.**

#### ⭐ Ed25519 implemented — all RFC 8032 host vectors pass, first try

`kernel/crypto/ed25519.{c,h}` over the fe25519 field layer and SHA-512.
Host results on the first clean compile:

```
T1 pubkey/sig/verify (EMPTY message)   OK
T2 pubkey/sig/verify (1 byte)          OK
T2 tampered signature rejected         OK
wrong pubkey rejected                  OK
round-trip depending on no constant    OK
```

**Why it worked first try, and this is the transferable part: the curve
constants are DERIVED, not recalled.**

```
d        = -121665 * inv(121666)      via fe_invert at init
By       = 4 * inv(5)
Bx       = even sqrt((y^2-1)/(d y^2+1))
sqrt(-1) = 2^((p-1)/4)
```

DDR-819 and DDR-820 each shipped a **wrong recalled constant** that a vector
caught (§2.3.2-vs-§2.4.2 nonce; 121665-vs-121666). Deriving removes that failure
mode instead of testing for it. The one unavoidable recalled constant is the
group order **L**, which does not follow from the curve equation — isolated in
`L_BYTES`, and every checked signature exercises it.

Other decisions worth keeping:
- `sc_reduce` is **binary long division** (512 shift-and-subtract), not ref10's
  21-bit-limb routine. Slower, obviously correct, no hand-transcribed magic.
- `verify()` **rejects non-canonical S (S ≥ L)**. Without it the scheme is
  malleable — a second valid signature exists for the same message. The probe
  asserts it.
- `*r = acc` on a 160-byte struct compiles to a **memcpy call**, which does not
  exist in the freestanding user model. Replaced with explicit `fe_copy`.

**GATE STATUS — FAILED. Resolved at session end, recorded rather than left open.**

```
[smoke] FAIL — required pattern 'PRADYOS_ED25519_VECTORS_OK' not found.
EXIT=2
```

**Read what this is and is NOT.** There is **no** `PRADYOS_ED25519_STUB` line and
**no** trap. The probe did not report a wrong vector and did not fault — the
sentinel simply never appeared inside the 150 s window. So this is NOT evidence
that the implementation is wrong, and the host vectors (all of RFC 8032 §7.1,
plus tamper/wrong-key/round-trip) stand.

**Leading hypothesis, untested: the probe is too slow under TCG.** It performs
**8 scalar multiplications** — 3 pubkey derivations, 3 signs (each of which does
two scalarmults internally), plus verifies — at 256 ladder steps each with an
unconditional `ge_add` and `ge_dbl`, i.e. roughly 40,000 `fe_mul` calls, plus a
1023-byte hash. Every other crypto gate does a few hundred field operations.
150 s was chosen by analogy, not measured.

**Next actions, cheapest first:**
1. Read the full serial log for `Ed25519 vector probe spawned` — that separates
   "never started" from "started and did not finish".
2. Raise `TIMEOUT_S` to 300 **in the recipe** (the shell-assignment prefix beats
   the environment — see the 0.-10 note) and re-run.
3. If still nothing, cut the probe to T1 + tamper only and re-measure; the cost
   is dominated by scalarmult count, not by which vectors are checked.
4. Only after those: suspect the implementation.

**The gate stays REGISTERED in shard 5.** It is a real assertion about a real
primitive and it is currently failing; hiding it in EXCLUDE would be the DDR-817
mistake. **CI will be red on this until it is fixed — that is correct and
intended, and it must be fixed before ACC (DDR-813) starts.**

**Next exact command:**
```
wsl -d Ubuntu-24.04 -e bash -c 'cd /mnt/c/Users/prady/Documents/Claude/Projects/Prady4OS
grep -aE "^\[smoke\]|EXIT=" /tmp/ed_gate.log | tail -4'
```
If that shows PASS + `EXIT=0`, Ed25519 is done and **ACC (DDR-813) is
unblocked** — AEAD, X25519 and Ed25519 would all then be genuinely green.

#### Also this session

- **`smoke-aead` green.** First failure was the probe pairing RFC 8439 §2.3.2's
  nonce with §2.4.2's ciphertext. Proved by running both candidates against the
  same unmodified `aead.c`. Registered in shard 4.
- **DDR-821 step 1**: `fe25519.{c,h}` extracted; `smoke-x25519` passes on a
  genuinely rebuilt artefact.
- **DDR-825**: `kernel/crypto/*.c` and `Makefile` were not prerequisites of the
  image, so `make image` reported success while the link never ran. Sixth
  instance of the silent-drop family. **Now working as intended** — Makefile
  edits correctly trigger full rebuilds, which is why gate wall-clock rose.

#### B#3 / OPEN-10 — harvest returned NOTHING

Three of four CI harvest runs concluded **green**. **No `btree churn FAIL`
occurred, so there is no `op=` line to read.** The unification hypothesis
(OPEN-10 ≡ B#3) is **still untested** — this is absence of evidence, not
evidence of absence. Combined with 5/5 clean local `-smp 4` runs, B#3 is
constrained but not fixed and not reproduced.

**Still ruled out, do not retry:** `IRQF_PERCPU` (no analogue in this kernel);
a spinlock in `sfs.c` (no global mutable state; VFS serialises per-mount).

#### Next, in order

1. Read `/tmp/ed_gate.log` for the Ed25519 verdict.
2. If green: **ACC (DDR-813)** — NSI 77/78, envelope exactly as specified in the
   queue (X25519 ephemeral → HKDF `ACC-session-v1`/`ACC-owner-v1` →
   ChaCha20-Poly1305 with `nonce=eph_pub[0:12]` → Ed25519 over
   `eph_pub||ct||tag`, `agent_pubkey[32]` in the envelope, Ed25519 and X25519
   keys distinct). Arms: `ACC_OK`, `ACC_ERR_AUTH`, `ACC_ERR_REPLAY`,
   owner-read-after-reboot.
3. Then AGS (79/80) → rotation (81) → TASK 9–21.

### 0.-12 SESSION — 2026-08-03 (earlier)

**`main` = `3b4830a` — PROMOTED.** `dev/phase1` = `d5901ee`, clean, pushed.
NSI max 76, next free 77. 120 gates, 5 excluded.

#### ⭐ PROMOTION DONE — three consecutive greens on ONE tip

`30804476970`, `30811210244`, `30811221820` — all success, all `3b4830a`.
`git merge --ff-only` carried DDR-817 (CI sharding, 2 h 08 m → ~25 min),
**X25519**, SHA-512, DDR-822, DDR-823, and the `_start` alignment fix across 23
probes. **Ed25519 is unblocked under rule 7.**

#### ⭐ smoke-aead PASSES — the wrong constant was the probe's

Root-caused with positive evidence, not assumed. **RFC 8439 carries two
different nonces a few pages apart:**

```
§2.3.2 (block function test)  00 00 00 09 | 00 00 00 4a | 00 00 00 00
§2.4.2 (encryption test)      00 00 00 00 | 00 00 00 4a | 00 00 00 00
```

The probe paired §2.3.2's nonce with §2.4.2's expected ciphertext — which is
exactly why the failure was `first_bad_byte=0`, a gross mismatch rather than
anything resembling a carry bug. **Proved** by running both candidate nonces
against the same unmodified `aead.c` on the host: §2.4.2 matches, §2.3.2 does
not. `aead.c` was correct throughout.

Gate now passes in QEMU, exclusion removed, registered in shard 4. The comment
on `N244` records the trap. **This vindicates committing it excluded** — had it
gone straight into the matrix, CI would have been red on a probe bug.

#### ⭐ DDR-821 step 1 done — field layer extracted, regression-clean

`kernel/crypto/fe25519.{c,h}` extracted from `x25519.c` so `ed25519.c` can share
it. Verified **twice**: all 7 RFC 7748 checks still pass on the host, and
`smoke-x25519` PASSES in QEMU on a genuinely rebuilt artefact.

**Next step for Ed25519** is the group law itself — Edwards extended
coordinates, plus scalar arithmetic mod
`L = 2^252 + 27742317777372353535851937790883648493`. Design is complete in
`docs/ddr/DDR-821-ed25519.md`, including the 7 vectors and the A/B/C arms.

#### 🔴 DDR-825 — a build that reported success and never ran

**DDR-822 fixed `user/` and stopped there.** The probe ELFs also link
`kernel/crypto/*.c`, and the **Makefile itself** decides what gets linked.
Neither was a prerequisite of `$(KERNEL_BIN)`.

So `make image` reported no errors, `x25519test.elf` appeared to change size,
and **`build/fe25519_user.o` did not exist** — the link never ran. Without the
rule-4 freshness check, the next `smoke-x25519` would have exercised the OLD
`x25519.c` with the field layer still inlined, and its PASS would have said
nothing about the refactor it was meant to regression-test.

Fixed by globbing `kernel/crypto/*` and listing `Makefile`. **Sixth instance**
of the same structural defect. Rule extended: *when replacing a hand-maintained
list, ask what else belongs in it before declaring it fixed.*

#### B#3 — evidence harvest dispatched, not yet read

Per the queue's instruction, stopped local repros and dispatched **four CI runs**
(`30817443737`, `30817543924`, `30817552272`, `30817560375`) on `dev/phase1` to
harvest `op=` lines from `smoke-smp`/`smoke-rqstress`, where both OPEN-10 hits
actually occurred. **All still in flight — read them first next session.**

`op=write` supports the OPEN-10 ≡ B#3 unification; `op=create`/`op=unlink`
refutes it. **Still ruled out, do not retry:** `IRQF_PERCPU` (no analogue in
this kernel) and a spinlock in `sfs.c` (no global mutable state; VFS already
serialises per-mount).

#### Next, in order

1. Read the four B#3 harvest runs for `op=` lines.
2. **Ed25519 group law** — `kernel/crypto/ed25519.{c,h}`, RFC 8032 vectors,
   `smoke-ed25519` arms A/B/C. Field layer is ready and proven.
3. ACC (DDR-813, NSI 77/78) — **now legitimately unblocked**: AEAD is genuinely
   green, not merely excluded.
4. AGS (79/80) → rotation (81) → TASK 9–21.

### 0.-11 SESSION — 2026-08-03 (earlier)

**`dev/phase1` tip when this was written: `3b4830a` + this commit.
`main` = `b823bb5`.** NSI max 76, next free 77. 119 gates, 6 excluded.

#### ⭐ smoke-x25519 is CI-PROVEN

**CI run `30804476970` on tip `3b4830a`: SUCCESS, all jobs.** That was the
gate's first-ever execution on a CI runner, and it passed. Combined with 4/4
clean-host local passes, X25519 is settled.

**Root cause of the two-session mystery, now closed:** it was never one defect.
A stale probe ELF (DDR-822 — the Makefile named 17 of 31 user sources, so the
`_start` alignment fix was never in a tested binary) **plus** leaked QEMU
holding the image write-lock (DDR-823 — reported as "kernel sentinel not
found"). The primitive was correct throughout.

#### Promotion — two runs dispatched on ONE tip, both in flight

The rule is 3 consecutive greens on the **same** commit. Prior greens were on
four *different* tips and promoted nothing.

| run | tip | state at handoff |
|---|---|---|
| `30804476970` | `3b4830a` | **success** (green #1) |
| `30811210244` | `3b4830a` | in flight (green #2?) |
| `30811221820` | `3b4830a` | in flight (green #3?) |

Both dispatches were verified to target `3b4830a` before I continued.
**If both are green: `main` fast-forwards to `3b4830a`, and Ed25519 (TASK 4)
unblocks under rule 7.** Later commits on `dev/phase1` do not affect this — a
dispatched run pins its head SHA, so promote `3b4830a` specifically.

```
git checkout main && git merge --ff-only 3b4830a && git push origin main
```

#### smoke-aead written — COMMITTED BUT EXCLUDED, NOT YET RUN

`user/aeadtest.c` + `smoke-aead` + build wiring + kernel spawn are complete and
lint-clean (`ci-shard-check`, `ci-start-align-check` both pass). This closes the
gap flagged since DDR-819: `aead.c` was host-verified only, with no gate and in
no build, and DDR-813 must not consume an ungated primitive.

**IT WAS RUN ONCE AT SESSION END AND IT FAILED:**
```
PRADYOS_AEAD_STUB case=chacha20_2_4_2 first_bad_byte=0
```
**This vindicates excluding it.** Had it been registered, CI would be red right
now and blocking every promotion.

**Diagnosis, stated as far as the evidence goes and no further.** Byte 0 of the
ChaCha20 keystream check differs — a gross mismatch, so it is the key/nonce/
counter setup or a wrong recalled constant, NOT a subtle carry bug in the round
function. DDR-819 recorded RFC 8439 §2.4.2 passing on the HOST against this same
`aead.c`, which points at **the probe**, not the primitive. **That is a
hypothesis, not a finding — it has not been checked.**

First thing to do: re-derive the §2.4.2 inputs rather than trusting the recalled
ones. Key = 00..1f, nonce = 00:00:00:09 00:00:00:4a 00:00:00:00, **counter = 1**,
the "sunscreen" plaintext. Verify `chacha20_stream`'s counter argument means
what the probe assumes. Then re-run, and only then unexclude.

The gate stays **excluded with that reason recorded** in `shard_check.sh`.

**Next action for it:** `make smoke-aead`, then delete the `smoke-aead` line
from `EXCLUDE` and add `4<TAB>smoke-aead<TAB>90` to `gate_shards.txt`.

Five checks, chosen by code path: §2.4.2 keystream (isolates the cipher),
§2.5.2 tag with a **34-byte** message (2-byte final block — the only path that
catches the short-block defect DDR-819 actually shipped), a seal/open round-trip
depending on no published constant, and **two distinct rejection arms**
(tampered ciphertext, tampered tag — the second catches an `open()` that
recomputes the tag but never compares it to the one supplied).

#### B#3 — no evidence yet, and the queue's fix does not apply

**The suggested `IRQF_PERCPU` fix is Linux terminology with no analogue in this
kernel.** There is no such flag to set. Not applied.

**Code review found no open race.** `submit()` holds `compl_lock` with IRQs off
across `virtq_publish` → `notify` → `while (!done) sched_block_on()`, and
`complete()` takes the same lock. The completion cannot land between publish and
the BLOCKED publication, because the submitter holds the lock until
`sched_block_on` releases it — the DDR-locks-4 pattern is intact. That is
consistent with B#3 having been "narrowed but not fixed": the obvious races are
already closed.

**B#3 REPRODUCTION RESULT: 5/5 PASSED, `stuck_lines=0` on every run.**
5 × `smoke-blkmq` at `-smp 4`, `TIMEOUT_S=180`, serial, clean host. The DDR-776
watchdog **never fired once** — no request was stuck at all, and no
`churn FAIL op=` appeared.

That is real negative evidence, and it constrains B#3 without fixing it: the
defect is **rarer than 1-in-5 locally at `-smp 4`, or host-dependent**. It does
NOT show B#3 is fixed — nothing was changed. Next attempt should either run a
much larger budget or, cheaper, harvest it from CI where both OPEN-10 hits
actually occurred.

**The unification hypothesis is unchanged and untested:** OPEN-10 may be B#3
seen through the SFS churn probe. `op=write` supports it; `op=create`/`op=unlink`
refutes it. DDR-824's context dump is what will show it.

#### Next, in order

1. Read `30811210244` and `30811221820`. Two greens ⇒ promote `3b4830a` to
   `main` with the ff-only command above.
2. `make smoke-aead` once; if green, unexclude and register in shard 4.
3. Finish the B#3 reproduction (5 × `smoke-blkmq`) and read the `op=` line.
4. **TASK 4 Ed25519** — only after x25519 is green in `main` (rule 7). Design is
   already written in `docs/ddr/DDR-821-ed25519.md`; SHA-512 is done and gated.
   First step there is extracting the shared field layer from `x25519.c` into
   `fe25519.{c,h}`, with `smoke-x25519` as the regression test for the move.
5. Then ACC (77/78) → AGS (79/80) → rotation (81), then TASK 9–21.

### 0.-10 SESSION — 2026-08-03 (earlier)

**Branch `dev/phase1` = `6a0c571`. `main` = `b823bb5` (not promoted).**
NSI max 76, next free 77. `MAX_SYSCALLS` 128. 118 gates, 6 excluded.

#### ⭐ smoke-x25519 PASSED on a clean host — the crypto chain may be unblocked

```
[smoke] PASS — saw 'NEXUS KERNEL OK' + 1 FS pattern(s).   EXIT=0
```

**Why this pass is trustworthy where last session's was not.** Last session a
PASS was observed and correctly discounted, because two QEMUs were racing the
same image. This one has all four preconditions verified *before* the run:

1. `pgrep qemu-system-x86_64` returned **0** (rule 1).
2. `build/x25519test.elf` newer than `user/x25519test.c` (rule 4, DDR-822).
3. DDR-823's HOST-ENV detector is live — lock contention would exit **3**, not
   pass. It exited **0**.
4. Nothing else was running.

**VERIFIED vs ASSUMED, stated precisely (updated post-handoff):**
- **Verified: THREE clean-host PASSes, all rc=0** — the initial run plus
  confirmation runs 1 and 2, each on a verified-empty host. A fourth was still
  running at session end.
- **DONE:** all three confirmations passed — **4/4 clean-host passes, zero
  failures**. The exclusion has been REMOVED and `smoke-x25519` is registered in
  shard 3 (119 gates, 5 excluded). The 5/5 bar was arbitrary; 4/4 consecutive on
  a verified-clean host with the HOST-ENV detector active is sufficient, and
  leaving a working gate excluded indefinitely is precisely the DDR-817 failure
  mode. **Watch its first CI run** — it has never executed on a CI runner.

**CI greens that landed after the handoff was first written:**

| run | tip | verdict |
|---|---|---|
| `30773609553` | `1fa8495` | **success** |
| `30773828417` | `e4bb576` (DDR-824) | **success** |
| `30774291748` | `1e40464` (SHA-512 gate) | **success** |
| `30803907180` | `6a0c571` (tracker sync) | in flight at session end |

**`smoke-sha512` is therefore CI-green**, not merely locally A/B-verified.

**Promotion note:** those are four greens on **four different tips**. The rule
is three consecutive greens on **one** tip, so none of this promotes anything
yet — pick the final tip and dispatch two more runs on it.

**Next exact command:**
```
wsl -d Ubuntu-24.04 -e bash -c 'cd /mnt/c/Users/prady/Documents/Claude/Projects/Prady4OS
for i in 1 2 3 4 5; do make smoke-x25519 >/dev/null 2>&1; echo "run $i rc=$?"; done'
```
5/5 with rc=0 ⇒ remove `smoke-x25519` from `EXCLUDE` in
`tools/ci/shard_check.sh`, add `3<TAB>smoke-x25519<TAB>90` to
`tools/ci/gate_shards.txt`, run `make ci-shard-check`, push, and **Ed25519
(TASK 4) is unblocked** once it is green in `main` (rule 7).

#### NEW FINDING — `TIMEOUT_S=<n> make smoke-*` is silently ignored

The recipe sets `TIMEOUT_S=90` as a **shell assignment prefix**, which always
beats the environment. So `TIMEOUT_S=300 make smoke-x25519` runs at **90 s** and
says so in its own log — I caught it only by reading
`[smoke] booting ... (timeout 90s ...)`.

**Every past "still fails at TIMEOUT_S=300 via make" claim is void.** The one
genuine 300 s data point came from invoking `tools/qemu_runner/boot_test.sh`
directly, which does honour the environment. To override a gate's timeout you
must either call the script directly or use `make TIMEOUT_S=300 smoke-x25519`
(a make-level variable) — and even that loses to the recipe's inline assignment.
**Verify the timeout in the harness's own banner line, never assume it took.**

#### Tracker reconciled — five stale load-bearing entries corrected (`6a0c571`)

| stale | verified |
|---|---|
| "X25519 not started, no source" | `kernel/crypto/x25519.{c,h}`, 10,468 B, all RFC 7748 host vectors pass |
| "SHA-512 not present" | `kernel/crypto/sha512.{c,h}`, gate `smoke-sha512` A/B-verified, shard 3 |
| "aarch64/riscv64 zero source files" | `boot.S` + `start.c` + `kernel.ld` each; **green in CI's `arch-bootstrap` every run**. Scope is **boot-only** per ADR-034 — recorded that way so it is not over-read |
| "ISO = 4 from-scratch ports, 3–5 sessions" | **packaging**, not porting — 2 of 4 targets already boot |
| tracker tip `77e690c` | matched nothing; now `1e40464`→`6a0c571` |

Phase 0 36%→55%, Phase 6 44%→61%, total 74%→76%. **Bookkeeping, not new code** —
those features existed and were invisible in the tables.

`BUILD_TRACKER.md` §6 now enumerates the **entire** plan with per-item status:
TASK 0–21, Section E (NSI 82–87), 3B capability bits, 3C action types #31–44,
3D daemon features 1–21, F#66–F#76, Section G 12 agents, J-01…J-06, Section B
remainder, ISO ×4, `prad`, invariant gates, v1.0.0. Nothing planned is hidden.

**NSI collision flagged:** TASK 18 specified `prad` at 87–89, but **87 is
`SYS_READ_AUDIT`** (F#76). `prad` renumbered **88–90**.

#### OPEN-10 — diagnosis advanced, root cause still open

- CI run `30773609553` (workflow_dispatch on `1fa8495`) — **GREEN, all jobs.**
- **DDR-824**: the harness printed only lines matching the forbidden pattern, so
  `[sfs] churn FAIL op=<create|write|unlink> iter=<N>` — the line that names the
  defect — **never reached CI output**. That is why OPEN-10 was seen twice and
  stayed undiagnosable. Now prints 40 lines of context.
- **The queue's spinlock fix has no target**: `kernel/fs/sfs/sfs.c` has zero
  global mutable state; the VFS already serialises per-mount (`vfs.c:25`).
- **Live hypothesis:** OPEN-10 is **B#3 seen through the SFS probe**. Both hits
  were `-smp 4`; the probe does 40 × (create + 64 KiB write + unlink); a lost
  virtio-blk completion makes `vfs_write` return ≠ 65536 ⇒ `op=write`.
  **`op=write` supports it; `op=create`/`op=unlink` refutes it.**
- `smoke-sfs-btree-smp4` added as an on-demand repro, **excluded** from the
  matrix. It **PASSED** at `TIMEOUT_S=180`, confirming last session's 16/20
  "failures" were the 90 s window (see the TIMEOUT_S finding above).

#### Remaining blockers

| blocker | state |
|---|---|
| `smoke-x25519` repeatability | 1 clean PASS; confirmations pending |
| OPEN-10 root cause | gates promotion; **do B#3 first** |
| B#3 `-smp 4` virtio-blk | untouched this session |
| `smoke-aead` | DDR-819 still has no gate and is in no build |
| Ed25519 → ACC → AGS → rotation | rule 7 chain behind x25519 |

#### Next, in order

1. Run the 5× confirmation above; if 5/5, unexclude and register `smoke-x25519`.
2. Read CI runs `30773828417`, `30774291748`, `30803907180` (all 10/11 green
   with one shard outstanding at handoff).
3. **TASK 8 (B#3) before more OPEN-10 work.**
4. TASK 4 Ed25519 once x25519 is green **in `main`**.

### 0.-9 SESSION — 2026-08-03 (earlier)

**`dev/phase1` = `e4bb576` + uncommitted SHA-512 gate. `main` = `b823bb5`.**
NSI max 76, next free 77. MAX_SYSCALLS 128. 118 gates, 6 excluded.

#### Corrections to the queue's premises — verified against the tree

The verification table supplied with the queue is wrong on load-bearing points.
Checked, not assumed:

| table claim | reality |
|---|---|
| "X25519 ❌ Not started, no source found" | `kernel/crypto/x25519.c` — 10,468 B, all RFC 7748 vectors pass |
| "`arch/aarch64/`, `arch/riscv64/` zero source files" | `kernel/arch/{aarch64,riscv64}/{boot.S,start.c}` exist; **CI's `arch-bootstrap` jobs build AND boot both, green every run** |
| "SHA-512 not present" | `kernel/crypto/sha512.c` — 4 FIPS 180-4 vectors pass |
| "`tools/boot_test.sh`" | actual path `tools/qemu_runner/boot_test.sh` |
| "109 CI gates" | 118 assigned, 6 excluded |

**This materially shrinks TASK 17.** aarch64 and riscv64 already reach their
boot banner in CI. The ISO work is packaging, not a from-scratch port.

#### TASK 1 — the queue's prescribed OPEN-10 fix has no target

It said: find B+tree ops touching shared state without a lock under SMP, add a
spinlock. **There is no such target.** `kernel/fs/sfs/sfs.c` has **zero** global
mutable state, and the VFS already serialises every op per-mount with an atomic
sleep-mutex (`kernel/fs/vfs/vfs.c:25`, DDR-locks-3). Adding a spinlock would be
patchwork against a hypothesis the code contradicts.

**Better-supported hypothesis, recorded not asserted:** the churn probe does
40 × (create + 64 KiB write + unlink) of heavy block I/O, and **both OPEN-10
hits were `-smp 4` gates** — which is exactly B#3/DDR-806, the known `-smp 4`
virtio-blk completion stall. A lost completion makes `vfs_write` return
≠ 65536, i.e. `op=write`. **If so, OPEN-10 and B#3 are one defect seen through
two probes, and fixing B#3 fixes both.** Do B#3 (TASK 8) before more OPEN-10
work.

**DDR-824 — the harness was discarding the proof.** `check_global_forbidden`
printed only lines matching the forbidden pattern. Probes print summary-last:
`[sfs] churn FAIL op=create iter=17` (names the defect) then
`[sfs] btree churn FAIL` (what the list matches). The `op=` line contains none
of the forbidden string, so **it never reached CI output** — which is why
OPEN-10 was seen twice and stayed undiagnosable. Now prints 40 lines of leading
context. **The next OPEN-10 occurrence will name its failing operation.**

`smoke-sfs-btree-smp4` added as a reproduction surface, **excluded** from the
shard matrix with that reason (registering it now would make CI red on a
known-open defect and block unrelated promotions). It **PASSED** locally at
`TIMEOUT_S=180`, which also confirms last session's 16/20 "failures" were the
90 s window, not a defect — 3 extra vCPUs multiply TCG work without adding host
parallelism.

#### TASK 3 — SHA-512 gate, A/B verified

`user/sha512test.c` + `smoke-sha512`, registered in shard 3, added to
`GLOBAL_FORBIDDEN` (append-only, nothing removed).

| arm | kernel SHA | verdict |
|---|---|---|
| B — one byte flipped in the empty-message vector | `92d528310a7c` | **FAIL**, and names `case=empty first_bad_byte=7` — exactly the flipped byte |
| C — correct | `4fe5534e6caa` | **PASS** |

Distinct SHAs (DDR-791). Artefact freshness confirmed per rule 4 before running.
Arm B also demonstrates DDR-824's context dump working end to end.

#### TASK 2 — NOT DONE

`smoke-x25519` was not re-verified. Rule 1 (serial QEMU) was binding all
session: the smp4 reproduction and the SHA-512 A/B arms occupied the single
QEMU slot. **This is the next thing to do**, and the host is currently clean
(`pgrep qemu-system-x86_64` returned 0 before the last run).

#### Next, in order

1. `pgrep qemu-system-x86_64` (must be empty) → `make smoke-x25519 TIMEOUT_S=300`.
2. Commit the SHA-512 gate if not already in (check `git status`).
3. **TASK 8 (B#3) before more OPEN-10 work** — see the unification hypothesis.
4. Read run `30773609553` (dispatched this session on `1fa8495`).
5. Ed25519 stays blocked until `smoke-x25519` is green in `main` (rule 7).

#### Scope reality, unchanged

TASKs 9–21 are ~250 features including 4 ISO targets, 12 agents, F#66–F#76 and
a package manager. This session completed TASK 0, most of TASK 1, and TASK 3,
and found two more harness defects on the way. That rate does not reach Aug 31
for the full set. The sequencing is sound; the scope is the decision.

### 0.-8 SESSION — 2026-08-03 (earlier)

**`dev/phase1` = `7a7385e`. `main` = `b823bb5`** (still not promoted).

#### CORRECTION — DDR-820's "cause unknown" was wrong, and so was the last handoff

`$(KERNEL_BIN)`'s prerequisites named **17 of the 31 files in `user/`**. Editing
any of the other 14 did not rebuild the image. `make image` said success and the
gate tested the previous binary.

**The `_start` alignment fix was correct the first time and was never in a
binary that ran.** The `#GP`, the "no output after the fix", and the "still
fails at `TIMEOUT_S=300`, so it is stuck not slow" were three correct
measurements of one stale 16:19 artefact while the source said 18:18. Fixed in
DDR-822 with `$(wildcard user/*.c)`, verified by touch-then-relink.

**This is the second instance of one structural bug this session.** DDR-817
found `ci.yml` had drifted from the Makefile (eight gates never ran); DDR-822 is
the Makefile drifting from `user/`. Both are hand-maintained lists mirroring a
directory where drift silently produces a green result. **Treat any remaining
hand-maintained list that mirrors a directory as a latent third instance.**

#### smoke-x25519 — STILL NOT PASSING, still excluded

Do not believe any claim that it passes. It was observed PASSing once, but under
host conditions I had contaminated (see below), so that observation carries no
weight. It remains in `EXCLUDE` in `shard_check.sh`. **DDR-813 must not consume
x25519 until this gate is green on a clean host.**

Host vectors still pass (RFC 7748 + commutativity + small-order rejection).

#### STRONG OPEN-9 CANDIDATE — leaked QEMU holds the image write-lock

Five consecutive `smoke-x25519` failures reported `kernel sentinel not found`
while plain `make smoke` passed seconds earlier on the same image. Cause:

```
qemu-system-x86_64: Failed to get "write" lock
Is another process using the image [build/pradyos.img]?
```

Two orphaned `qemu-system-x86_64` processes were live. QEMU exits before
printing anything, so the harness sees an empty serial log and **blames the
kernel**. Every OPEN-9 symptom matches: fails locally / passes CI (fresh runner
per job), identical binary opposite verdicts, "recovered overnight" (orphan
reaped), twice caused a change to be wrongly blamed.

**Recorded as a hypothesis, not a closed defect** — it has not yet been caught
red-handed on a `smoke-shell` failure. Full analysis in DDR-822 §A second
host-side defect.

**Highest-value next slice:** make `boot_test.sh` detect the lock error and
report *"host has a stale QEMU holding the image"* instead of *"kernel sentinel
not found"*. A harness that misattributes a host problem to the kernel is how
two sessions were spent blaming the tree. Add `pkill -f qemu-system-x86_64` (or
a lock precheck) to the gate preamble.

**Practical rule learned the hard way this session: never run two QEMU gates
concurrently on this host.** Several of my own measurements were invalidated by
overlapping background runs sharing `build/pradyos.img` and the serial log.

#### CI

- run `30756063513` (DDR-817, tip `2158778`) — **GREEN, 25m20s**, down from
  2h08m. Sharding works; `shard-check` green.
- run `30756989017` (tip `0e23bf3`) — GREEN.
- run `30757030329` (tip `7e5d522`, docs-only) — **RED**, then rerun still in
  flight at session end.

**NEW UNTRACKED DEFECT — `OPEN-10`.** That red was `smoke-smp` tripping
`GLOBAL_FORBIDDEN` on **`'btree churn FAIL'`** — the DDR-763 SFS B+tree probe
failing during an unrelated gate's boot, caught by DDR-791's global list working
exactly as designed. Docs-only commit, so it is intermittent, not a regression.
It is a *different* signature from OPEN-1 (`smoke-surfdestroy`) and was not
previously tracked. **Read the rerun verdict before promoting anything.**

#### ITEM 2 done

`force_align_arg_pointer` applied to all 23 `user/` `_start` functions. Baseline
`make smoke` still passes with the change.

#### Next, in order

1. Read run `30757030329`'s rerun verdict. Do not promote on a red tip.
2. Harness lock-detection slice (above) — it unblocks trustworthy local testing,
   which everything else depends on.
3. Re-verify `smoke-x25519` on a clean host; if green, re-register it.
4. Then ITEM 3 (`smoke-aead`), ITEM 4 (DDR-821 Ed25519), ITEM 5 (DDR-813 ACC).

**Scope note on the 13-item queue:** items 3–13 are ~250 tracked features. At the
current verified rate — and with two structural build/CI defects found in one
session — that is many sessions of work, not one. The sequencing in the queue is
sound; the "done when" criteria should be read as a destination, not a session
goal.

### 0.-7 SESSION — 2026-08-02 late

**`dev/phase1` = `2158778`. `main` = `b823bb5`** (not promoted this session).
NSI max **76**; next syscall is **77**.

Commits: `b343b0f` docs sync · `a5d9dac` DDR-817 CI sharding ·
`2158778` DDR-820 X25519.

#### READ THIS FIRST — eight gates were never running in CI

`ci.yml` hand-listed 111 gate steps and **nothing compared that list to the
Makefile.** Eight gates existed and had never run in a single CI run:

```
smoke-sha256  smoke-hkdf  smoke-lockbox  smoke-rng
smoke-sigpipe  smoke-privacy-netfilter  smoke-blkmq  smoke-rqstress
```

That is the gate for **every crypto primitive promoted to `main`** — DDR-811,
DDR-812, DDR-816, DDR-818 — plus DDR-805 and DDR-802. Those promotions each had
two CI greens on the exact tip. **The greens were real and carried no
information about those features.** What they actually rest on is their local
3-arm A/B verification, which is genuine evidence but is not what "two CI
greens" was taken to mean. `smoke-blkmq`/`smoke-rqstress` were additionally
masked by their own `-trace`/`-liveness` variants, so grepping the base name
matched and the absence looked like presence.

All eight are now in the shard matrix. **`make ci-shard-check` fails if any
`smoke-*` target is unassigned, assigned twice, or names a target the Makefile
does not define.** Exclusions are explicit, each with its reason on the line.

#### DDR-817 — CI sharding (in flight)

Measured before assuming: `build-and-boot` was 2 h 08 m and every other CI job
finished in under 30 s, so one serial job was the entire critical path. Setup is
~49 s against ~7 600 s of boots. Now a **6-way matrix**, packed
longest-processing-time-first by measured duration
(`tools/ci/gate_shards.txt`). Predicted longest shard ~24 min.

**Run 30756063513 was still in flight at session end (23m+, all six shards
running, `shard-check` green). Read its verdict first.** If green, it needs a
second green before `main` moves.

Rejected deliberately: relaxing DDR-785's forbidden-sentinel rule with a grace
period to reclaim the eleven 180 s windows. That trades a real guarantee for
speed the sharding already provides. **No gate's semantics, timeout, or
sentinels changed; no gate was removed.**

#### DDR-820 — X25519: host vectors pass, GATE DOES NOT

`kernel/crypto/x25519.{c,h}` + `user/x25519test.c`. All RFC 7748 vectors pass
under `gcc` on the host, including the commutativity check that depends on no
published constant. **The in-QEMU `smoke-x25519` gate does not reach its
sentinel, and the cause is NOT known.** Recorded as unknown rather than guessed.

It is not slow: the same source runs all seven checks in **3 ms at -O0 on the
host**, so even a 1000× TCG penalty is seconds — and a re-run at `TIMEOUT_S=300`
(3x the gate window) still produced no sentinel and no trap, which eliminates
"slow" by measurement rather than by argument. After the alignment fix below it
neither traps nor prints: it is stuck or dead, not late. Candidates not ruled out, in DDR-820: the
large-code-model `.ltext`/`.lrodata` orphan sections (`user.ld` names neither —
lld's orphan placement is what currently puts them in the PT_LOAD), stack depth
in `fe_invert`'s addition chain, or an uninitialised `tcb` field for this
thread shape (see memory `tcb-fields-not-zeroed`).

`smoke-x25519` is therefore **excluded from the shard matrix, explicitly, with
that reason in `shard_check.sh`.** Re-register it the moment it passes.
**DDR-813 must not consume this primitive until the gate is green.**

**Two defects worth carrying forward:**

1. **Ladder constant.** `a24 = 121665` paired with the `BB` form, which needs
   `(A+2)/4 = 121666`. Both constants are correct *for different forms of the
   same expression*, so it reads like a typo and is not. Every
   constant-comparing vector failed while **commutativity passed** — diagnostic,
   because a consistent group law with a wrong parameter still commutes and
   broken field arithmetic does not. That narrowed 250 lines to one line.

2. **`_start` stack alignment — affects EVERY freestanding probe in `user/`.**
   SysV enters a process with RSP 16-byte aligned; a compiler treats `_start` as
   a called function and assumes RSP ≡ 8 (mod 16). The frame is off by 8,
   callees inherit it, and `movaps` to a stack slot raises **#GP** (not #AC).
   **The kernel is correct — `elf_build_image` aligns properly. Do not "fix" the
   entry RSP.** Fixed in this probe with `force_align_arg_pointer`. The other
   probes survive only because their generated code never emitted an aligned SSE
   stack access; nothing about them is safe by design. Worth its own slice.

#### Next, in order

1. Read run 30756063513's verdict. If green, `workflow_dispatch` a second run on
   the same tip, then promote.
2. Root-cause `smoke-x25519` (the `.ltext` orphan-section hypothesis is the
   cheapest to test: name `*(.ltext*)` and `*(.lrodata*)` explicitly in
   `user.ld` — but note that changes every user ELF, so check the blast radius).
3. Then DDR-821 (Ed25519), then DDR-813 (ACC).
4. Unrelated and still owed: DDR-819's `aead.{c,h}` has **no gate and is in no
   build** — host vectors only.

### 0.-6 SESSION — 2026-08-02 (earlier)

**`main` = `dev/phase1` = `b823bb5`.** Two consecutive CI greens on the exact tip
(run 30733620093, attempts 1 and 2). Tree clean, nothing unpushed.

That promotion carried **DDR-816** (entropy), **DDR-818** (HMAC+HKDF) and
**DDR-819** (ChaCha20-Poly1305) in one cycle. Six features reached `main` across
the session: DDR-811, DDR-812, ADR-035, DDR-816, DDR-818, DDR-819.

**NSI max is 76** (`SYS_METRIC_READ`). The next new syscall is **77**.

**Two corrections to the standing work queue — verified against the tree, not
assumed.** A session brief listed both of these as unbuilt:
- `SYS_GETDENTS` is **already shipped at NSI 66**, and PRISM's `ls` enumerates
  through it (DDR-742). It is not a stub.
- `ps` is **already shipped** via `SYS_GETPROCS` at NSI 67 (DDR-743).
Both were mis-tracked as "stubs" in Section 0's older prose and in Section B#8
until 2026-07-24; `docs/AETHER_MASTER_FEATURES.md` records the correction.
Building them again would duplicate shipped syscalls and burn two NSI numbers.
The claim "current NSI max = 79" in that brief is wrong by three.

**Outstanding on DDR-819:** `kernel/crypto/aead.{c,h}` is verified against RFC
8439 vectors **on the host under gcc only**. There is no `smoke-aead` gate and
the object is in no build. In-QEMU verification of this primitive is still owed —
DDR-813 is scheduled to be its first caller and should wire the probe.

**Owner decisions now on the record (do not re-ask):**
- **D-1** — the X25519/Ed25519 constant-time gap is **accepted**. Build DDR-820
  and DDR-821 to the best achievable standard (pure C, constant-time by
  construction, all RFC vectors passing) and document that QEMU cannot verify
  timing-channel resistance and that production use needs side-channel review.
- **D-2** — sequencing: DDR-817 (boot acceleration) first, then 820 → 821 → 813
  (ACC) → 814 (AGS) → 815 (update propagation).
- **D-3** — local `smoke-shell` reds are informational; CI is the arbiter.
- **D-4** — the `-smp 4` virtio-blk completion stall (B#3/DDR-806) must be
  root-caused, not worked around.
- **D-5** — the Rust requirement for init/compositor/service-manager is waived;
  continue in C.

**Next:** DDR-817 §Design — profile the boot and CI wall-clock, find the actual
bottleneck before assuming one, and cut it. Target ≤60 min per CI cycle against
the current ~4h15m per two-green promotion.

### 0.-5 SESSION — 2026-08-02

**`main` = `a4d1569`** (promoted; DDR-811 SHA-256 + DDR-812 lockbox + ADR-035 +
designs, two greens on the exact tip).

**DDR-816 (entropy) re-applied and A/B-verified** — kernel `4a6b5e680038`:
A no-device FAIL · B fixed-buffer `b2b2b57ece36` FAIL · C correct PASS.
Arms A and C share a SHA **correctly**: arm A varies the QEMU invocation, not the
source. Nine gates green locally. Awaiting two CI greens.

**OPEN-9 UPDATE — the host recovered.** `smoke-shell` now PASSES locally on
functionally identical DDR-816 code that failed 5/5 the previous day. Same code,
opposite verdict, no change in between. This confirms OPEN-9 is host-state
dependent and that reverting the DDR-816 attribution was correct. **Keep treating
local `smoke-shell` reds as informational until the host-side cause is found** —
it has now produced two false attributions (OPEN-8 and DDR-816).

**Historical red never diagnosed:** run 30640007581 on `258e439` failed
`smoke-surfdestroy` (missing `PRADYOS_SURFDESTROY_CHURN_OK`). That is the OPEN-1
signature from run 30151522978. Docs-only commit; every later commit passed. Left
recorded rather than closed — OPEN-1 is still live in CI, not only locally.

**Next:** DDR-813 (ACC) unblocks on DDR-816's greens. Its two spec bugs are
already documented and MUST be fixed at design time: the owner CC box needs
`agent_pubkey[32]` in the envelope (otherwise the owner's later offline read —
the whole point — fails after any reboot), and `OWNER_PUBKEY[32]` must split into
separate Ed25519 (sign) and X25519 (box) constants.

### 0.-4 SESSION CLOSE — 2026-08-01 late (READ FIRST)

**HEAD `8677d6a` (DDR-812). `main` = `1d7637a`. Tree is CLEAN — DDR-816 was
implemented and fully reverted.**

**OPEN-9 HAS ESCALATED ON THIS WORKSTATION — local reds are no longer evidence
about the tree.**
`smoke-shell` now fails ~5/5 locally on kernel `f36ce889348e`, which is the exact
binary that PASSED it earlier the same day. Same binary, opposite result. It also
failed identically on the DDR-816 kernel and on the unconditional-drain kernel,
so it tracks the host, not the code.

**Consequence, and it is the important line in this file:** until OPEN-9's
host-side cause is found, **judge changes in CI, not locally**. `smoke-shell`
has passed in CI on every recent commit. I twice attributed a local red to the
change in flight (OPEN-8 previously, DDR-816 today) and both survived a full
revert. The REVERT VERIFICATION RULE caught both.

**DDR-816 status: designed, implemented once, reverted, NOT pushed.**
- `smoke-rng` **PASSED** — virtio-rng works, the `0x1040 + type` device ID was
  right, fail-closed and the two-draw self-test all function.
- Its `smoke-shell` 5/5 failure is NOT attributable to it: the baseline fails
  5/5 too, so the comparison carries no information.
- Re-apply from DDR-816 (fully described there) and judge it **in CI**.
- Do NOT re-derive the "boot output aggravates RX loss" story — it was tested;
  making the RX drain unconditional did not change the outcome (the failing
  assertion merely moved from DDR-789 to DDR-782).

**Correction to a claim I made earlier:** DDR-809 was reported as closing the
input-loss window. It drains RX **only inside the THRE spin**, so on a fast UART
the loop body rarely runs and the drain rarely happens. That is narrower than it
was described. Whether it matters is unproven — the unconditional version did not
help.

**Still true and unaffected:** DDR-811 (SHA-256) and DDR-812 (metric lockbox) are
sound, A/B-verified with distinct SHAs, and DDR-812's CI run 1 was in flight at
session end (run 30710422416 on `8677d6a` — read its verdict first).

### 0.-3 SESSION — 2026-08-01 (read this first)

**Shipped:** DDR-811 (SHA-256, two greens, promoted — `main` = `1d7637a`) and
DDR-812 (metric lockbox, gate-verified locally on kernel `f36ce889348e`).

**DDR-812 notes that matter for anyone touching it:**
- Record lives in the DDR-795 `metric_page` frame, **not** SFS. The VFS gates
  writes on `CAP_FS_WRITE` alone, which every `CAP_SOVEREIGN` process holds, so
  an SFS lockbox is writable by exactly the processes it guards against.
- **Arm D was already built.** `user/metrictest.c` + `smoke-metric` store to
  `METRIC_USER_VA` and pin the fault to `cr2=...040` — offset 64, which is where
  the lockbox record now begins. That gate protects the record by construction,
  so `smoke-lockbox` covers only read+verify. **`smoke-metric` is therefore the
  regression that matters most if the record layout is ever changed.**
- Write-once is `static lockbox_commit()` (linker-enforced) + two phase wrappers
  + a runtime phase guard. A compile-time assertion constraining call sites is
  **not expressible in C**; do not re-add that claim.
- Hash input order is BINDING, stated in `metric_page.h` and DDR-812. The probe
  serialises independently — two implementations of one contract, so divergence
  is detectable.
- `sha256.o` joined the kernel link here (DDR-811 left it out for lack of a
  caller; the build failed with `undefined symbol: sha256` until this slice).

**§S dependency order changed by design review — do not follow the old brief:**
- DDR-813 (ACC) is **blocked** on DDR-816 (entropy). There is NO entropy source
  in this kernel; predictable X25519 keys and reused ChaCha20 nonces would make
  a system that presents as encrypted and is not.
- DDR-816 fails closed — no jitter fallback. A source that silently degrades is
  worse than one obviously absent.
- Two ACC spec bugs recorded in DDR-813: the owner CC box loses access at every
  reboot without `agent_pubkey` in the envelope, and one `OWNER_PUBKEY[32]`
  cannot be both an Ed25519 and an X25519 key.

**Open:** OPEN-1, OPEN-2 (passive, await natural reds), OPEN-9 (one unattributed
`smoke-shell` red; many passes since).

**New:** `ETAMPER` 133, `SYS_METRIC_READ` 76, `smoke-lockbox`, `smoke-sha256`.
`GLOBAL_FORBIDDEN` gained `PRADYOS_SHA256_STUB`, `SHA256 FAIL`,
`PRADYOS_LOCKBOX_STUB`, `LOCKBOX FAIL` (append-only).

### 0.-2 SESSION CLOSE — 2026-07-31 (read this first; it is the only source of truth)

**HEAD / branches**
- `main` = `06d8fa0` — **promoted 2026-07-31**, two verified greens.
- `dev/phase1` = `06d8fa0` + the docs commit below.
- Last two greens: run **30623530245 attempt 1 and attempt 2**, both on
  `06d8fa0`. Second attempt obtained via `gh run rerun` (the fine-grained PAT has
  admin; the old classic PAT did not, which is why this rule was previously
  unsatisfiable).

**CI facts worth not rediscovering**
- Expected duration **~2h08m**. Two-green = ~4h15m per promotion.
- The run object's `updatedAt` is **unreliable** — it freezes seconds after
  creation while the job runs for hours. Use the `.../jobs` endpoint's step
  timestamps for real progress.
- `workflow_dispatch` resolves from the **default branch**; it only started
  working once `main` carried it. `gh run rerun` is the better tool anyway — it
  re-executes the identical SHA by construction.
- Doc-only commits: 1 green is sufficient (protocol exception).

**DDR state**
- **DDR-805 SIGPIPE — CLOSED.** Three edits; three-arm A/B with distinct
  kernels (A `30e6f27da9b2` FAIL, B `4a8f44823ce5` FAIL, C `d3404eef47a7` PASS);
  gate `smoke-sigpipe` via `QEMU_PROBES=sigpipe`; two CI greens.
  The gate asserts **survival, not `$? == 13`** — see the DDR for why a status
  assertion needs a 4th edit that changes SIGKILL/SIGTERM.
- **ADR-035 — WRITTEN, not yet CI-confirmed.** Bounded W^X carve-out; **E-05
  code must not be written until this is accepted** (ADR-021 is binding).
- **DDR-810 (§S5 metric lockbox) — BLOCKED ON THREE DECISIONS, no code.**
  Do not start §S5 until these are answered:
  1. Authoritative record in `metric_page` (page-table enforced) **not** SFS.
     As specified, `/metric/lockbox` is writable by any `CAP_FS_WRITE` holder —
     which every `CAP_SOVEREIGN` process is — so the lockbox would be writable
     by exactly the processes it guards against. DDR-795's header already argued
     this.
  2. **No hash primitive exists in this kernel** (`grep blake3|sha256` over
     `kernel/`+`tools/` → nothing). §S5 and §J-03 both need one. Prerequisite
     slice, own DDR, gate must check **published test vectors**.
  3. **SHA-256, not BLAKE3** (recommended) — `metric_page.h` already declares
     SHA-256 and the shipped Python side produces it. Both are 32 bytes, so the
     mismatch is invisible at the type level.

**Open**
- **OPEN-9 (new):** one unattributed `smoke-shell` red on the DDR-805 arm-C
  kernel; 6 local passes + 2 CI greens since. Not attributed to DDR-805 — a ~14%
  rate shows zero failures in 3 runs ~64% of the time. Needs a real artefact.
- **OPEN-1 / OPEN-2:** passive. Stamps at `main.c:1134`/`:1311` and
  `PIPE_TRACE=1` self-report on the next natural red. Do not force runs.

**Throughput note:** the binding constraint is now wall-clock, not work — ~4h15m
of CI per promotion against a ~75-item queue. The lever is DDR-803's unclaimed
observation: cut boot work so the twelve 90 s windows can come down.

### 0.-1 TASK TRACKER (authoritative; update EVERY loop — master-prompt §3)

- **CANONICAL FEATURE STATE:** `docs/AETHER_MASTER_FEATURES.md` (Sections A–H) is
  the single source of truth for feature status — created 2026-07-24. Never let a
  second feature list exist. Mirror it here + in `docs/build_status.md` in the
  same commit as any code touching agents/UI/sockets/storage/namespaces/telemetry/
  scheduling/capabilities.
- **GROUND STATE (2026-07-30, later):** DDR-804 + the DDR-802 gate landed.
  Last confirmed green: run **30483750211 on `90634b6`** (which contains
  `6a0ec7c`). `main` still **72 commits behind** — nothing promoted since
  `3485085`. aether baseline unchanged: **764 collected**.
- **CURRENT_ACTIVE_TASK:** UNATTRIBUTED RED — `smoke-shell` fails locally at
  `9f1459a`, truncating the DDR-786/787 200-line pipeline at line 197 of 200.
  **Resolve this before resuming the work queue.** Read CI run
  **30504947387** on `9f1459a` first: green ⇒ local timing artefact (the gate is
  driven by fixed `sleep`s against a FIFO, unlike the `boot_test.sh` gates);
  red ⇒ DDR-804 is the first suspect. Items 1–4 are DONE.
- **DDR-805 (SIGPIPE) IMPLEMENTED + A/B VERIFIED 2026-07-31.** Three edits only:
  `SIGPIPE 13`, added to the default-terminate set, raised on the `-EPIPE`
  branch only. Three-arm A/B, distinct kernels: A no-sigpipe `30e6f27da9b2`
  FAIL · B raised-not-terminal `4a8f44823ce5` FAIL · C correct `d3404eef47a7`
  PASS. Gate `smoke-sigpipe`, opt-in via `QEMU_PROBES=sigpipe`.
  **Gate asserts SURVIVAL, not `$? == 13`** — the kernel sets `exit_status = -1`
  for every default-terminate signal and records no signal number, so a status
  assertion would need a 4th edit adding `128+signum`, changing SIGKILL/SIGTERM
  too. That remains available as a future DDR; do not fold it in silently.
  Blast radius clean: `smoke-syspipe`/`smoke`/`smoke-user`/`smoke-fs` PASS.
  **CI state: NOT yet confirmed — needs 2 greens on the pushed SHA.**
- **OPEN-9 (NEW, unattributed):** `smoke-shell` failed once on the DDR-805 arm-C
  kernel, then passed 6 consecutive runs on that identical kernel. NOT claimed as
  caused by DDR-805 — a ~14% rate shows zero failures in 3 runs ~64% of the time,
  so the 3/3 baseline does not establish the rate is new. Do not "fix" this
  without a named mechanism from a real failing artefact.
- **LICENSE-MIT renamed to LICENSE** — content was already the correct proprietary
  text; `README.md` and `docs/decisions/ADR-002-licensing.md` referenced the old
  filename and were updated, so a bare `git mv` would have left the licensing ADR
  pointing at a missing file.
- **OPEN-8 IS CLOSED (DDR-809). OPEN-8 was never a gate-timing problem — it was
  a kernel input-integrity defect.** `kputs`/`kwrite` hold the console lock with
  interrupts off while `kputc` spun unboundedly on THRE, so IRQ4 could not drain
  COM1's 16-byte RX FIFO and kernel output silently destroyed console input.
  Fix: bound the spin (`CONSOLE_THRE_MAX = 10000`) and drain RX inline.
  A/B, distinct kernels: baseline `4923c1831f2a` FAIL 4/4 with 1 RX loss per run;
  fixed `4a1dc378c13e` PASS 3/3 with **0 losses**, and both `$?` assertions
  (`st-ok=0`, `st-fail=127`) now correct. `smoke`/`smoke-user`/`smoke-fs`/
  `smoke-syspipe` all PASS. **DDR-807 is closed by the same change.**
  Invariant changed on purpose: the RX ring is now multi-producer under
  `g_rx_lock` (consumer unchanged). A BSP-only drain was rejected as unwritable —
  `kputc` prints before percpu exists, so `this_cpu()` would fault on the first
  character the kernel ever prints.
  **Three earlier "established" OPEN-8 facts were each refuted by measurement:**
  the 4096-VM-exit mechanism (actual `inb_count=968`; 4096 was my own clamp
  arithmetic propagated through handoffs as observed data), the `fwcfg_init()`
  cause (FAIL 3/3 both arms), and my `ae2fdbf` stamps (FAIL 3/3, and `90634b6`
  too). Do not reintroduce any of them.
- **OPEN-1 — SETTLING MEASUREMENT RUN (2026-07-30). Six hypotheses refuted, one
  open. The stamps are now PERMANENT INSTRUMENTATION in the tree — the next red
  run answers the question by itself.**
  Measured on the exact failing artefact (`BSP_LIVENESS=1`, `-smp 4`,
  `TIMEOUT_S=180`, kernel `32c84784cf9d`): stamp B (`main.c:1311`) is reached at
  **6.8–10.8 s of a 180 s window** over 5 runs. The "boot runs out of window"
  family — including DDR-803's prediction — is **closed**.
  **READ THIS ON THE NEXT RED RUN:** grep the serial for `[boot-stamp] B`.
  B present ⇒ the stall is AFTER B ⇒ prime suspect is **DDR-807** (`kputc`
  unbounded THRE spin with IRQs off). B absent ⇒ stall is BEFORE B ⇒ DDR-807 is
  wrong too and hypothesis 8 is needed. Do not fix before reading this.
  Local repro rate is under 1-in-5 and each attempt costs a full 180 s (these
  gates cannot early-exit per DDR-785), so do NOT fish for a local repro — let CI
  produce the artefact.
- **DDR-807 (NEW) — real S2 violation, found in passing, fix DEFERRED with no
  code.** `kernel/console.c:63` spins on UART THRE with no bound, called with
  interrupts disabled. Dormant today. The fix is not a two-liner: bounding it
  forces a choice between a lossy console (every gate asserts on serial), an
  error return (`kputc` is `void`, called from panic/ISR), or serial and `dmesg`
  disagreeing. Needs a gate that genuinely back-pressures the UART.
- **(superseded) OPEN-1 — NARROWED, NOT EXPLAINED (DDR-806). A proposed fix was implemented
  and REFUTED the same day; it is reverted, no DDR-806 code is in the tree.**
  Four things are ESTABLISHED, each from a named artefact:
  (1) the proofs never execute — neither OK nor FAIL variant, DDR-777 entry
  marker absent; (2) **DDR-777 verdict (C) is REFUTED** — failing run
  30507516805 shows `[smp] cpus online=4/4`, `ap preempt OK`, `resched OK`, so
  APs were up and the `!g_smp_have_aps` guard passed; (3) not a code regression —
  `9f1459a`/`6c375ea`/`c9a1537`/`d8c5c95` are a byte-identical kernel with CI
  FAIL/PASS/FAIL/PASS across two different gates; (4) the proofs **cannot** be
  hoisted above the probe block — `smpuser_proof()` polls `g_user_on_ap`, which
  needs live user processes the probe block creates.
  SURVIVING CANDIDATE: `fs_test_thread` (`main.c:829` — NOT `kmain`, which is at
  `main.c:1805`) does not reach `main.c:1311` in the window; ~30
  `user_boot_from_sfs()` calls sit between, each blocking on SFS I/O over
  contended virtio-blk.
  **DO THIS FIRST, before any fix: stamp `g_ticks` at `main.c:1134` and
  `main.c:1311`.** No second stamp in a failing run confirms it; both stamps
  early refutes it. DDR-806 has already produced two confident explanations that
  the next measurement destroyed — do not add a third without the stamps.
- **(superseded, kept as a record of a wrong turn) OPEN-1 "explained":** Open since
  DDR-775 with four refuted hypotheses, all hunting a defect in the SMP path.
  There is none. `smpuser_proof()` / `blkmq_proof()` / `smp_blk_integrity()` /
  `rqstress_proof()` sat ~180 lines AFTER the user-probe spawn block in `kmain`;
  those probes are `sched_unblock`ed and compete with `kmain` for CPU, so on a
  slow runner `kmain` never reaches the proofs inside the window. The tell:
  **neither the OK nor the FAIL variant of those sentinels appears in the failing
  serials** — they never executed. Proof it is not a regression: `9f1459a`,
  `6c375ea`, `c9a1537`, `d8c5c95` are a byte-identical kernel and CI alternates
  FAIL/PASS/FAIL/PASS across two different gates. Fix: proofs moved ahead of the
  probe storm. **Not proven fixed** — ~50% base rate means one green run is not
  evidence; needs several consecutive greens.
- **OPEN-8 IS RETIRED as a DDR-804 regression.** CI shows `smoke-shell` PASSING
  on `9f1459a`; the deterministic local red is a property of THIS workstation
  (fwcfg_init shifting boot timing against a gate driven by fixed `sleep`s on a
  FIFO), not of the tree. Do not revert DDR-804 for it. Superseded detail below:
- **(superseded) OPEN-8 as originally written:** `9f1459a` (DDR-804) broke
  `smoke-shell`. Local bisect: `90634b6` PASS, HEAD FAIL 2/2, and
  `6c375ea` is docs-only. Failing assertion is
  **`[shell] FAIL: $? did not expand to 0 after a successful command (DDR-789)`**
  — NOT the 200-line pipe test, which passes.
  Mechanism not yet named. Discriminate by reverting these INDEPENDENTLY:
  (1) the `privacynettest.elf` incbin entry in `arch/x86_64/user_image.asm`
      (~6.4 KB of image growth on every boot, gated or not — the tree has a
      low-mem image cap); (2) the unconditional `fwcfg_init()` call in `kmain`.
  Do NOT revert DDR-804 wholesale — it closes OPEN-7 and has a verified
  three-arm A/B. The defect is in how it was wired in, not the mechanism.
  Two of my intermediate readings were WRONG and are corrected in
  `docs/ddr/DDR-805-sigpipe.md`: the "truncation at line 197" was interleaved
  console output, not data loss; and I then analysed the PASSING run's
  `shell_serial.log`, not the failing one.
- **DDR-805 (SIGPIPE) is DESIGNED, IMPLEMENTED, and REVERTED — not blocked by
  its own defect.** I first blamed it for the `smoke-shell` red, reverted it, and
  the gate failed identically with the code gone. The design is sound and the
  three edits are fully described in the DDR; re-apply them once OPEN-8 is
  resolved. **Lesson recorded: a revert is not verified until the gate is re-run
  — a failure that survives a revert was never yours.**
- **DDR-802 IS COMPLETE.** Three-arm A/B, all four kernel SHAs distinct so every
  arm genuinely rebuilt (the DDR-791 trap): A no-privacy-check `1b6dddc3f139`
  FAIL · B no-audit-record `23daf7ef7146` FAIL · C wrong-result-code
  `c58be04cf099` FAIL · restored `f9d03ce220da` PASS.
- **OPEN-7 IS CLOSED** by DDR-804 (per-boot probe selection via QEMU fw_cfg,
  `kernel/drivers/fwcfg/`). Any future gate over GLOBAL kernel state must use
  `probe_enabled("<name>")` + `QEMU_PROBES=<name>` rather than spawning
  unconditionally — that is now the standing pattern, not a one-off.
- **PREVIOUS GROUND STATE (2026-07-30):** HEAD `6a0ec7c` on `dev/phase1`. `main` is
  **72 commits behind** `dev/phase1` — nothing has been promoted since
  `3485085`. Latest CI: run **30472148480 on `1cbe6f6` = SUCCESS** (workflow
  `pradyos-ci`; ignore "Dependabot Updates", it is not a CI verdict). `6a0ec7c`
  is pushed but **its CI verdict is not yet read** — read it before anything
  else. aether test baseline: **764 collected** (763 pass + 1 permitted skip,
  `test_quarantine.py:69`).
- **CURRENT_ACTIVE_TASK:** DDR-802, step "gate". The mechanism is committed and
  green-adjacent; the gate is NOT written. The next action is designing a
  kernel-visible per-boot opt-in (see the new OPEN-7), then wiring
  `user/privacynettest.c` (already written, not built), then the three-arm A/B.
- **LAST_COMPLETED_TASK (newest):** DDR-802 mechanism — `sys_sock_connect`
  refuses egress under privacy mode ahead of the CAP_NET check, the allowlist,
  and (deliberately) the DDR-800 sovereign bypass, audited as
  `AR_PRIVACY_BLOCKED`. State is a separate `g_privacy_mode` on
  `AETHER_MODE_PRIVACY_ON/OFF` (2/3) through the existing `SYS_SET_MODE`. A
  bitmask was rejected after enumerating the blast radius: `aether_get_mode()`
  goes verbatim to ring 3 and userspace compares it to literal 0/1
  (`compositor.c:837`), so a privacy bit would have broken the UI. **The gate is
  deliberately absent** — see OPEN-7. Defaults off; `make image` warning-free,
  `smoke`/`smoke-fs` pass.
- **PREVIOUS:** DDR-803 — twelve gates were inheriting
  `boot_test.sh`'s unstated 30 s default (`TIMEOUT_S="${TIMEOUT_S:-30}"`, line 19)
  while declaring `FORBIDDEN_SENTINEL`, which disables DDR-785 early exit. Their
  sentinels land late in boot, so on a slow shared runner the window expires
  before the sentinel prints — `PRADYOS_BIGWRITE_OK` appears **nowhere** in run
  30447042919's serial, and the FAIL is stamped exactly 30 s after the gate
  starts. All twelve now set `TIMEOUT_S=90` explicitly. Costs ~+12 min/CI run;
  the trade-off and why this is a correction rather than papering are argued in
  the DDR. **Before this, I hypothesised my DDR-800/801 probes had slowed the
  boot and measured it: 8.3 s with vs 8.4 s without — hypothesis refuted, and I
  nearly fixed something that was not broken.**
- **OPEN-7 (NEW, 2026-07-30) — probes cannot be scoped to one gate.** Every
  `kmain` probe is spawned with `sched_unblock` and runs concurrently, and
  `user_boot_from_sfs` writes-then-loads unconditionally, so there is no way to
  make a probe exist in only one gate. Any probe that mutates GLOBAL kernel
  state therefore perturbs its neighbours. This blocks the DDR-802 gate
  (privacy-ON would refuse the concurrent connects in `capnettest`,
  `sovegresstest`, `egressaudittest`) and will block every future
  global-state gate. The nearest precedent, `QEMU_SFSROOT`, works by having the
  kernel detect an extra block device — it is not a general mechanism, so this
  needs designing rather than copying. Write it as its own DDR.
- **OPEN-1 update (2026-07-30):** `smoke-smpuser` PASSED in run 30472148480, so
  it is intermittent and no discriminator output was produced this session. The
  rule is unchanged: wait for a natural red run, read the DDR-777 discriminator,
  name the mechanism, THEN fix. Do not force a failure.
- **STILL OPEN — do NOT fold into DDR-803:** `smoke-smpuser` failed in run
  30448425988 (`bf5b4c4`) missing `[smp] user on AP OK`. That gate sets
  `TIMEOUT_S=180` explicitly, so the DDR-803 mechanism does not apply. It is
  OPEN-1 class: on the next red run matching this signature, read the
  discriminator output and name the mechanism BEFORE writing any fix.
- **PREVIOUS:** DDR-773 mkfs.sfs multi-leaf B+tree (Master doc
  Section B item 2). Slots now collected flat (bounded `MKFS_MAX_SLOTS = 512`,
  clean error = S2) and bulk-loaded: ≤14 slots → single leaf at block 1
  (byte-identical to before), else N leaves + one internal root with separators =
  each next leaf's first key (mirrors kernel `bt_search_root` descend).
  `sfs_readback` descends too. Gates: `smoke-mkfs-sfs` +20-file/41-slot case
  verifying first/middle/last; `smoke-sfs-persist` now feeds the KERNEL a
  multi-leaf image (`21 slots, root=23`) — both PASS locally. Host-tool only,
  zero kernel files. **106 gates. CI pending on dev/phase1.**
- **SECTION D VERIFIED (2026-07-24):** ADR-026 baseline #1–17 confirmed genuinely
  BUILT in `kernel/aether/` (QUEUE_LEN 256, AUDIT_LEN 4096, PAYLOAD_MAX 512,
  MEM_DEFAULT 128 MiB, RATE_MAX 60/window 100, CAP_SOVEREIGN 1<<16, CAP_AGENT
  1<<17, SYS_SUBMIT/APPROVE/REJECT/SPAWN_AGENT/KILL_AGENT, ACTION_WRITE_FILE/
  PRINT/SPAWN_PROCESS). No drift. Next free cap bit `1<<18` = CAP_MEMORY as
  planned. This is the precondition for starting any Section E/F work.
- **PRIOR:** DDR-772 NVMe PRP2 + PRP list. `nvme_io` now
  issues multi-page single commands instead of one-per-≤page-chunk: `nvme_submit`
  gained a `prp2` arg; a scratch PRP-list page (`n->prp_list`) holds ≤512 page
  addrs. Per command PRP1 + (0 | second-page | PRP-list), capped ~2 MiB, loop for
  larger. `smoke-nvme` extended: 16 KiB round-trip as ONE command → gate PASS
  (5 patterns incl. `PRADYOS_NVME_PRP_OK`). **106 gates. CI pending.** NVMe IRQ
  still deferred (needs an MSI-X vector; 50–63 window full).
- **PRIOR:** ADR-032 FS write budget lifetime-cap →
  token-bucket rate limit. `fs_write_budget` is now a bucket refilling
  256 KiB/tick (25 MiB/s) up to a 1 MiB burst cap; added `tcb.fs_budget_tick`;
  `vfs_write` lazily refills from elapsed ticks (only tops up below-cap; never
  reduces → preserves the `~0ull` kernel bypass). Removes the 1 MiB LIFETIME
  ceiling; bounds RATE (anti-flood); disk-space is a separate control. New gate
  `smoke-fs-budget` — deterministic (rewind `fs_budget_tick` to simulate elapsed
  ticks): drained bucket refills+writes (old cap couldn't) AND rejects with no
  elapsed time. churn (smoke-sfs-btree) + budget bypass regressions green. **105
  gates. CI pending on dev/phase1.** LESSONS: (1) `make image` doesn't always
  rebuild `main.o` on source change — `rm build/main.o` before local test builds
  (wasted cycles on stale kernels); (2) a single 64 KiB FAT `vfs_write` hung in
  this context — used 4 KiB; (3) grep gate sentinels when changing kernel prints.
- **PRIOR:** DDR-771 `VBLK_MAX` 4→8 (MSI-X vector remap) —
  virtio-blk block MSI-X window moved 50–53 → 56–63 (clear of net@54/input@55).
  Required extending shared MSI-X infra: `isr.asm` stubs + `isr_stub_table`→64,
  `idt.c` gate loop→64 + `MSIX_VEC_COUNT` 6→14, plus `virtio_blk.c` VBLK_MAX 8 +
  base 56 + handlers 4–7. Root-caused a #GP (err 0x1C2 = IDT vec 56 ungated) a
  driver-only change first hit. `smoke-aether-sfsroot` now boots 5 virtio-blk
  disks (dropped `QEMU_NO_EXT4`, added ext4-image): asserts `blk4 ready` + daemon
  roots at provisioned image at blk4 alongside ext4. Local PASS. **104 gates. CI
  pending on dev/phase1.**
- **PRIOR:** DDR-770 persistent root from a host mkfs.sfs
  image — the AETHER `/etc/aether/config` now ships in `build/sfsroot.img`
  (mkfs.sfs, `QEMU_SFSROOT` knob) instead of being kernel-provisioned. The
  DDR-768 peek records the provisioned disk's mount (`prov_mnt`) when its
  `/etc/aether/config` starts with `mode=`; the persistent-root block roots the
  AETHER daemon there and skips the kernel `vfs_create/write` of config. Default
  boots unchanged (blk2 fallback intact). DDR-769's nested marker moved to
  `/etc/test/config` to disambiguate. New gate `smoke-aether-sfsroot`: `AETHER
  daemon rooted at provisioned mkfs image` + `PRADYOS_AETHER_CFG_OK
  mode=sovereign`. Local PASS (+ smoke-sfs-persist regression green). **103
  gates. CI pending on dev/phase1.**
- **PRIOR:** DDR-769 nested-directory provisioning in host
  `mkfs.sfs` — `add_file(path,…)` walks `/`-split components (intermediates =
  dir inodes `SFS_INO_DIR`, no extents, dedup shared prefixes; last = file),
  slots keyed `(parent<<32)|FNV1a32(name)` in the single root leaf; `sfs_readback`
  walks the same. `smoke-sfs-persist` extended: mkfs provisions
  `/etc/aether/config`, kernel reads it → `PRADYOS_SFS_NESTED_OK`. Verified
  locally (host round-trip + kernel boot). **102 gates. CI pending on dev/phase1.**
  NEXT after this lands: DDR-770 (migrate persistent-root `/etc/aether/config`
  off kernel provisioning onto a shipped mkfs image — touches boot/root flow).
- **PRIOR:** DDR-768 cross-reboot SFS persistence proof —
  the kernel boots on a host `mkfs.sfs` image and reads `/PERSIST.TXT` back.
  Guarded self-test in `fs_test_thread` (`main.c`): peek highest blk index's
  block 0 for `SFS_MAGIC` (`blk_read`), mount, `vfs_open`/`vfs_read`
  `/PERSIST.TXT` → `PRADYOS_SFS_PERSIST_OK`. `boot_test.sh` gains `QEMU_SFS2`
  (attach mkfs last) + `QEMU_NO_EXT4`. Root-caused a gotcha: virtio-blk caps at
  `VBLK_MAX=4` (MSI-X vec 50–53 packed vs net@54/input@55 → can't naively raise);
  with ext4 present the mkfs disk was the dropped 5th → suppress ext4 for this
  gate instead. `VFS_MAX_MOUNTS` 4→6. New gate `smoke-sfs-persist`, local PASS.
  **102 gates. CI pending on dev/phase1.**
- **PRIOR:** DDR-767 host `mkfs.sfs` — `tools/mkfs_sfs/{mkfs_sfs,sfs_readback}.c`
  #include kernel `sfs.h` (byte-exact). Host gate `smoke-mkfs-sfs`. Landed with
  the 16 `-smp 4` timeout bumps (90→180/60→120) that cleared the surfdestroy
  flake. **CI-green on `main` at `4ebcdc4`.** 101 gates.
- **PRIOR:** DDR-765+766 NVMe (controller bring-up + block I/O + `blk_register`)
  — `nvme0` registered, `PRADYOS_NVME_RW_OK`. Both **CI-green on `main` at
  `4ebcdc4`** (the 767 run validated 765+766+767 cumulatively after the -smp4
  timeout bump cleared the surfdestroy flake).
- **DDR-773 IS CI-GREEN ON `main` @ `3485085`** (run 30132610321, 106 gates).
- **DDR-774 (scoping only, no code):** blast-radius review of B#1 NVMe IRQ found
  it is NOT bounded — split into 774a/b/c. Vector availability was never the
  blocker. (a) the only MSI-X programmer is virtio-coupled
  (`virtio_pci_msix_setup` takes `struct virtio_pci_dev*`, uses virtio `map_bar` +
  `common_cfg.queue_msix_vector`) so it must be refactored out of a path serving
  blk/net/gpu/input — the same shared surface that produced DDR-771's #GP and
  stale-sentinel CI break; (b) `nvme.c` maps a fixed 2-page BAR0 window but the
  MSI-X table offset/BIR is runtime-determined and may be outside it or in another
  BAR; (c) completion moves thread→IRQ context for every I/O incl. the DDR-772 PRP
  loop. Per policy, invasive work was NOT started.
- **DRIFT CORRECTED:** master-doc Section B#8 (`ls`/`ps`) was STALE — both are
  already implemented in `user/prism.c` (`ls`→SYS_GETDENTS/DDR-742,
  `ps`→SYS_GETPROCS/DDR-743). Moved to Section A.
- **CI INFRA FLAKE (2026-07-25):** run 30139119085 (docs-only commit a02e790)
  FAILED at the `Install toolchain` step — NOT a regression: rustup reported
  `checksum failed for channel-rust-nightly.toml` → `toolchain 'nightly-...' is
  not installable`. Upstream static.rust-lang.org publish/CDN issue. `main` was
  therefore NOT promoted to a02e790. The DDR-774a push re-attempts the install; if
  this recurs, consider pinning rust to a dated nightly or stable in the workflow
  (the rust dep exists only for the `toolchain-check` no_std interop gate).
- **LAST_COMPLETED_TASK (newest):** DDR-774c **phase c-1** — NVMe MSI-X delivery
  ROOT-CAUSED AND FIXED (Master doc Section B#1 sub-slice c). The 774b symptom
  `[nvme] irqs=0` was **my bug in 774b**: NVMe `Create I/O CQ` CDW11[31:16] is an
  **index into the MSI-X table**, NOT the x86 interrupt vector. It was passed 50,
  aiming the controller at an unprogrammed/masked entry while only **entry 0** was
  programmed (the x86 vector rides in that entry's message-data field). Fix =
  pass the table index: `NVME_MSIX_ENTRY 0`. Result: **`[nvme] irqs=6`**.
  IMPORTANT: the leading suspect (MSI-X **Function Mask**, MC bit 14, never
  explicitly cleared) was **NOT** the cause — so `pcie_msix_program()` (the SHARED
  774a helper) was left untouched and the four virtio consumers cannot regress.
  Neither was the table address math / BIR decode. `smoke-nvme` PASS with 7
  patterns, adding `PRADYOS_NVME_IRQ_OK` (forbidding `..._IRQ_FAIL`), made
  deterministic by a BOUNDED settle spin (S2) instead of a timing-dependent count;
  the exact count is printed but NOT asserted (brittle if the self-test changes).
  **106 gates. CI pending.**
- **PRIOR:** DDR-774b NVMe MSI-X table mapping + IEN
  (Master doc Section B#1 sub-slice b) — plumbing only, completion STILL POLLED.
  `nvme_msix_setup()` uses `pcie_msix_find()` for the runtime BIR/offset, maps 2
  uncached pages at `NVME_MSIX_VBASE` (separate from the BAR0 register window),
  programs entry 0 for **vector 50**, registers an inert counting ISR, disables
  INTx, and creates the I/O CQ with IEN + vector in cdw11. `smoke-nvme` PASS
  (6 patterns incl. the deterministic `[nvme] msix vec=50`); RW_OK/PRP_OK green so
  the polled path is unaffected. **106 gates. CI pending.**
  ⚠ **OPEN ISSUE — MSI-X delivery does NOT work: boot prints `[nvme] irqs=0`.**
  Ruled out: interrupts masked (`sti` at main.c:1478/:1737 precedes `nvme_init` at
  :1801, so IF is set). Suspects for 774c, in order: (1) MSI-X **Function Mask**
  (message-control bit 14) is never explicitly cleared by `pcie_msix_program` —
  it is 0 after reset so virtio works, but NVMe's state is unverified;
  (2) the table address math in `nvme_msix_setup` (no #PF occurred, so the write
  hit mapped memory — but not provably the controller's table); (3) per-entry
  vector control read-back. The count is printed but deliberately NOT gated
  (timing-dependent while polling). **774c MUST root-cause delivery before
  touching the completion path.**
- **PRIOR:** DDR-774a generic PCI MSI-X programmer (Master
  doc Section B#1, sub-slice a) — pure refactor. `pcie_msix_find()`,
  `pcie_msix_program()`, `pcie_intx_disable()` added to `kernel/drivers/pcie/pcie.c`
  (+ decls in `pcie.h`); `virtio_pci_msix_setup()` reimplemented on them with
  identical signature, register writes and ORDER (INTx-disable stays at the call
  site so it still runs after the per-queue routing). All three virtio callers
  unchanged → the DDR-774 stop condition ("if it forces driver changes, stop") did
  NOT trigger. Unblocks 774b. Gates: smoke-fs/net/input/gpu.
- **DDR-774b IS CI-GREEN ON `main` @ `2034a53`** (run 30146543550); DDR-774a landed
  earlier at `347c422` (run 30141466540, which also carried the a02e790 docs
  commit). The `Install toolchain` rustup failure was confirmed transient — no CI
  pinning change was needed.
- ✅ **`main` IS AT `2083c09`** (run 30158060606 GREEN) — that promotion carried
  DDR-774c c-1 + both docs commits out of limbo. Section B#1 (NVMe MSI-X) is
  functionally complete for correctness and fully on `main`.
- **LAST_COMPLETED_TASK (newest):** DDR-776 virtio-blk stuck-request watchdog
  (Master doc Section B#3) — **diagnosis, NOT a fix; do not describe it as one.**
  `struct vreq` gains `t0`/`lba`/`warned`; `virtio_blk_watchdog()` is called from
  the timer path in `idt.c` every ~1 s (same idiom as `net_poll_tick`) and prints
  `[vblk] stuck dev=D slot=N lba=L age=T` once per request stuck >5 s. Works while
  a submitter is blocked forever because only that thread is stuck. No blocking
  behaviour changed; NO lock taken (read-only from the ISR — deliberately avoids
  adding a deadlock surface to the subsystem under investigation, S6); bounded
  64 checks/tick + one print per request (S2). **Design decision recorded:**
  yield-loop REJECTED (would spin on every block I/O, regressing the hot path all
  FS gates ride); scheduler timed-block DEFERRED (correct eventual primitive, but
  changing the scheduler core mid-investigation would confound attribution).
- **(historical) CI RED episode — main was NOT promoted.** Run 30151522978 (commit `715040e`, DDR-774c
  c-1) FAILED at step 101 `smoke-surfdestroy` (q35 `-smp 4`) — the Section B#3
  race. NOT a regression from 774c: that gate boots with **no NVMe device** (so
  `nvme_init` never runs), `smoke-nvme` passed earlier in the same run, and the
  same gate failed back in run 29726803735 (DDR-766, before any 774 work).
  **New signature:** timed out at the FULL 180 s having missed the FIRST sentinel,
  serial shows the boot **HUNG after `SYSFSTAT OK`** (ring-3 syscall self-tests,
  before any surface test) → a **hang, not slowness**; the DDR-771 timeout bump is
  not a fix. `main` stays at `2034a53`; `715040e` is unpromoted on dev/phase1.
- **DDR-774c c-2 STOPPED + re-scoped (no code)** — stop condition invoked; see
  DDR-774c. `nvme_submit()` is already a bounded poll, so the specified
  no-scheduler-hook design is polling-with-a-hint; a genuine IRQ wait needs the CPU
  to sleep (`sti;hlt` rejected — mutates caller `IF`; guarded `hlt` idle; or a
  scheduler block/wake wait-queue = out of scope). Deferred to a future DDR that
  must MEASURE polling cost first. Section B#1 is functionally complete for
  correctness (programmed + delivered + gated).
- **CURRENT_ACTIVE_TASK:** none — `715040e` awaits a green run.
- **DDR-775 (findings, no code): B#3 NARROWED to the virtio-blk completion wait.**
  Second CI hit — run 30155872016 failed `smoke-blk-integrity` (`-smp 4`,
  concurrent read data-verify) at the full 180 s. Local repro attempted: 3/3
  `smoke-surfdestroy` PASS → CI-only timing window. Two different `-smp 4` gates,
  both block-I/O; surfdestroy's stall point (`SYSFSTAT OK`, next `SYSREAD OK`) is
  inside `sys_read`→`vfs_read`→SFS→virtio-blk. **CONFIRMED DEFECT + S2 VIOLATION:**
  `submit()`'s `while (!v->req[s].done) sched_block_on(&v->compl_lock);` is
  UNBOUNDED → a missed completion hangs forever instead of erroring. The
  lost-wakeup race is NOT the defect (locks-4 pattern correctly closes it).
  **Latent, not this trigger:** `slot_waiter` is a single `struct tcb *`, so
  >VBLK_NREQ(8) concurrent submitters lose a wakeup. NO fix shipped — it does not
  reproduce locally, so a concurrency change to the shared block path would be
  pushed unvalidated and judged only by a 2.5 h CI run.
- ⚠ **CI RED on `c61a149` (run 30163444702) — DDR-776 unpromoted; `main` stays at
  `2083c09`.** Failing gate: step 51 `smoke-smpuser` "user-on-AP" (`-smp 4`,
  ring-3 thread on a non-BSP CPU), 180 s timeout, missed `[smp] user on AP OK`.
- 🔬 **DDR-776's FIRST RESULT IS A NEGATIVE ONE THAT REFUTES THE DDR-775
  NARROWING.** The watchdog printed **nothing** in that failure — no virtio-blk
  request stuck >5 s — while the timer was demonstrably still firing (boot
  progressed through the fuzz test, a ring-3 thread exited). So this failure is
  NOT a stuck block request. The three B#3 failures now share only `-smp 4` and
  miss DIFFERENT sentinels: surfdestroy (hung after `SYSFSTAT OK`),
  blk-integrity (`[smp] blk integrity OK`), smpuser (`[smp] user on AP OK`).
  **Revised: the ORIGINAL percpu-scheduler/AP-race framing is better supported
  than the virtio-blk narrowing** (DDR-775's narrowing section is marked
  superseded). Hazard 1 (unbounded completion wait) and Hazard 2 (single-element
  `slot_waiter`) are still real S2 defects worth fixing on merit — but are NOT
  proven to be this trigger. Note this vindicates diagnosis-first: a blind bound
  on the blk wait would have fixed nothing and masked the real cause.
- ⚠⚠ **B#3 SECOND CORRECTION — LEADING HYPOTHESIS IS A STALLED TIMER.** In run
  30163444702 the serial printed **neither** `[smp] user on AP OK` **nor**
  `[smp] user on AP FAIL` (the 2 log hits are the sentinel echo + the "not found"
  message, NOT serial), yet the preceding `ap preempt OK` / `resched OK` DID
  print. `smpuser_proof()` (main.c:659) is `while (!g_user_on_ap && g_ticks < dl)`
  — a deadline poll that MUST print one branch **unless `g_ticks` stops
  advancing**. This RETRACTS my earlier inference: the DDR-776 watchdog runs on
  the SAME timer path, so its silence cannot distinguish "no stuck blk request"
  from "the watchdog never ran"; my "timer was demonstrably firing" claim rested
  on boot progress that happened EARLIER than the stall point.
  **Hypothesis: under `-smp 4` the timer tick intermittently stops advancing
  `g_ticks`.** Explains all four failures at once (each missing whichever
  sentinel the boot had reached), the watchdog silence, blk waits never waking,
  and consistent local passes. **Systemic S2 exposure: EVERY `g_ticks`-bounded
  wait in the tree is only as bounded as the timer.**
- ⚠⚠⚠ **THIRD CORRECTION — "timeout ⇒ hang" WAS WRONG.** `boot_test.sh` ALWAYS
  runs QEMU for the full `TIMEOUT_S` window and THEN greps; `terminating on
  signal 15 … (timeout)` appears in PASSING runs too (verified in a local
  smoke-input PASS). So none of the B#3 failures are proven to be hangs — the
  only hard evidence is **sentinel absent**. Do not describe them as hangs.
- **NEWLY ESTABLISHED:** every SMP proof shares `if (!g_smp_have_aps) return;`
  (silent early return). In the failing run `ap preempt OK` + `resched OK` DID
  print ⇒ `g_smp_have_aps == 1` ⇒ `smpuser_proof()` did NOT early-return; it
  entered the poll and never reached its `kputs`. Three explanations survive:
  **(A)** timer stalls so `g_ticks < dl` never expires; **(B)** scheduler
  starvation — never resumes from `yield()` while the system stays alive;
  **(C)** guard/ordering effect.
- **LAST_COMPLETED_TASK (newest):** DDR-777 three-way discriminator probe
  (Section B#3) — **passive instrumentation, NOT a fix.** `[hb] t=<g_ticks>` every
  ~500 ticks from the existing timer call site in `idt.c`, plus
  `[smp] user-on-AP probe t=…` at `smpuser_proof` entry and a tick appended to the
  OK/FAIL line. Sentinel safety verified: `boot_test.sh` uses `grep -qF`
  (substring), so EXTRA `[smp] user on AP OK` and FORBIDDEN `user on AP FAIL`
  both still match with the ` t=<n>` suffix. No behaviour change, no locks, no
  scheduler hook (S2/S6).
  **HOW TO READ THE NEXT FAILING RUN (this is the whole point):**
  no `probe t=` line ⇒ **(C)** APs never came up (silent guard);
  `probe t=` present + `[hb]` STOPS ⇒ **(A)** timer stalled → B#3 moves to the
  LAPIC/timer path, and *every* `g_ticks`-bounded wait in the tree is exposed
  (systemic S2);
  `probe t=` present + `[hb]` CONTINUES + no OK/FAIL ⇒ **(B)** scheduler
  starvation → a runqueue/AP-claim bug.
- ✅ **`main` IS AT `968169c`** — all of DDR-774c c-1, DDR-776 (blk watchdog),
  DDR-777 (discriminator probe) and every B#3 correction doc are promoted.
  `dev/phase1` and `main` are level.
- **B#3 = INSTRUMENTED, AWAITING A NATURAL FAILURE.** Four consecutive green runs
  (30158060606, 30165570464, 30167716462, 30170362044) after the cluster of four
  failures; never reproduces locally (3/3). The probe is on `main` and will
  explain the next failure by itself. **Do NOT force a fix** — that would be a
  fourth blind attempt (already refuted: DDR-771 timeout bump, the virtio-blk
  theory, the "timeout ⇒ hang" reading). On ANY future red run, grep for
  `user-on-AP probe t=`, `[hb] t=`, and `vblk] stuck` and read the 3-way verdict
  in DDR-777.
- 🔎 **SECTION B AUDIT (2026-07-26) — the roadmap overstated remaining work.**
  Verified against the tree (not assumed): **B#5 COW fork is SHIPPED**
  (`kernel/mm/vmm_cow.c`, `vmm_cow_fault` at `idt.c:225`, gate `smoke-cowfork`
  Makefile:760 / CI:228), **B#7 kernel self W^X is SHIPPED**
  (`vmm_protect_kernel()`, DDR-757 gate Makefile:660 + CI PTE audit), and B#8
  (`ls`/`ps`) was corrected earlier. All three moved to Section A. **The other
  Section B entries have NOT all been re-verified — check the tree before
  planning any of them.**
- **LAST_COMPLETED_TASK (newest):** DDR-778 PRISM output redirection
  (Master doc Section B#12, first bounded slice) — **ring-3 only, zero kernel
  change.** Scoping confirmed `SYS_DUP2 = 18` and `SYS_PIPE` already ship
  (PROC-A, gates `smoke-syspipe` + CI "pipe/dup2"); `prism.c` had merely never
  `#define`d `SYS_DUP2`. `cmd … > file` now works: scan `argv[1..argc)` for a bare
  `>` (from index 1 so a leading `> file` can't leave `argv[0]` undefined),
  truncate `argc`, then `dup2(1,REDIR_SAVE_FD)` / `open(O_CREAT|O_WRONLY)` /
  `dup2(fd,1)` / `close`, restoring at the loop's existing flush point.
  TWO HAZARDS HANDLED — (1) musl FULLY BUFFERS a non-tty stdout, so the flush must
  happen BEFORE fd 1 is restored or the output lands on the console and the
  redirect silently half-works; (2) a skipped restore would send ALL later shell
  output into the file, so control flow was audited (only `continue` precedes the
  swap; `ls`'s `break` is an inner loop; `exit` returns from main). Gate:
  `smoke-shell` + a DISCRIMINATING PAIR — the marker alone would pass even with
  redirection broken (plain `echo` prints it), so `REDIR.TXT` must ALSO appear in
  `ls /`. **106 gates. CI pending.**
- 🚨 **CI IS BLOCKED UPSTREAM — `git.musl-libc.org` IS DOWN (DDR-779).** Run
  30178367399 (`21e4a51`, DDR-778) failed after 4m39s at **step 2
  `actions/checkout@v5`**, before any project code was fetched: the
  `third_party/musl` submodule clone timed out twice. Verified independently —
  `git ls-remote https://git.musl-libc.org/git/musl` is unreachable from here too.
  **NOT a regression, and pushing more commits cannot help** — every run will die
  at checkout until musl returns. This is the SECOND external-dependency outage
  this session (the first was `static.rust-lang.org`'s nightly checksum, run
  30139119085, which self-resolved). `.gitmodules` pins musl to a single upstream
  host while lwip already uses GitHub — that asymmetry is the single point of
  failure. **Proposed fix (NOT applied, needs maintainer sign-off — it is a
  supply-chain change): point musl at a GitHub mirror keeping the same pinned SHA
  `0784374d…`.** Content integrity is SHA-guaranteed, but the mirror must first be
  confirmed to contain that exact commit via a real fetch (`git ls-remote` only
  lists ref tips and returned inconclusive, not negative). See DDR-779 for the
  verification recipe and alternatives (vendor / actions/cache / retried step).
  **Operational rule while down: check step 2 before diagnosing any CI failure,
  and do not push just to retrigger.** Local work is unaffected (musl is already
  cloned; `make image` and all local gates run normally).
- ⚠ **LOCAL-ONLY GATE LIMITATION (not a regression): `make smoke-shell` fails
  here with `no PRISM_READY`.** The gate writes its serial log to `build/` on
  **DrvFs (`/mnt/c`)**, which is slow enough in this WSL setup that the boot
  misses the gate's 60 s window. Evidence it is environmental: a direct boot
  prints `PRISM_READY` fine, and `smoke-shell` **passes in CI** (step 26, green in
  run 30170362044). Do NOT "fix" this by raising the gate's timeout. To validate
  shell changes locally, use the same FIFO flow but put the serial log on `/tmp`
  (ext4) — see the DDR-778 proof script pattern.
- 🧪 **LOCAL-VALIDATION GOTCHA (cost a wasted check):** WSL's `/tmp` does **not**
  persist between separate `wsl -d Ubuntu-24.04 bash -lc ...` invocations, so a
  serial log written by one call is GONE in the next — post-hoc inspection is
  impossible. Put the boot AND every assertion in ONE scratchpad script. Also note
  an unexpanded `$VAR` inside nested `wsl -c` quoting makes `grep` error, and an
  `||` fallback then prints a FALSE "PASS" — use literal paths and `if/else`.
- ✅ **CI UNBLOCKED (2026-07-26): `git.musl-libc.org` is REACHABLE again.** The
  DDR-779 outage is over; runs get past `actions/checkout@v5` once more. The
  backlog (`21e4a51` DDR-778 `>`, `d8d78e4` DDR-779 finding, `6561a74` DDR-780
  `|`, plus DDR-781) can finally be CI-validated — promote `main` to the first
  green sha covering them. DDR-779's mirror proposal still needs sign-off; do NOT
  apply it autonomously.
- ✅ **PROMOTED (2026-07-26): `main` fast-forwarded to `ac459d7`** on CI run
  **30193738689** (`success`, 112/112). Promotes DDR-782 (kernel `O_TRUNC` +
  atomic `O_APPEND`), ADR-033/DDR-779 (musl mirror) and DDR-783. That run also
  confirms both fixes individually: **step 2 checkout passed** after three musl
  outages, and **step 10 smoke-fs passed** after the measured timeout bump.
## AETHER host-side Python layer (ASI-bridge v4.0) — build tracker

Root: `aether/`. Host-side Python 3.13 agent stack that runs ON the OS as a
service; NOT code the bare-metal target executes. Its invariants are **S1–S14**
(`aether/kernel/invariants/core_invariants.py`) and are **independent of the C
kernel's S1–S8** — the numbering collides, the meanings do not, never merge them.

**Gate command:** `python -m pytest -W error -x -q aether/tests/`
**CI:** the `aether-layer` job runs it on every push (added 2026-07-28 — before
that, "CI green" on an `aether/` commit proved only that the C kernel still built).

| Section | Items | Status |
|---|---|---|
| **B** (foundation) | B-01…B-17 | ✅ **COMPLETE** — 187 passed, 1 skipped, 0 warnings |
| **C** (gap features) | C-01…C-10 | ✅ **COMPLETE** — 335 passed, 1 skipped, 0 warnings |
| **D** (ASI bridge) | D-01…D-15 | ✅ **COMPLETE** — D-01…D-15 (614 passed, 1 skipped, 0 warnings) |
| **I** (integration) | I-01…I-10 | ✅ **COMPLETE** — 763 passed, 1 skipped, 0 warnings (DDR-792/793 bridges, F#68 lockbox+kernel wire, netfilter, limiter) |
| **J** (Phase 4 audit) | J-01…J-06 | ⬜ not started |

B-series detail: B-01 lockbox · B-02 audit log · B-03 merkle · B-04 firewall ·
B-05 quarantine · B-06 red team · B-07 agent bus · B-08 notification bus ·
B-09 model manager · B-10 computer use · B-11 OOBE · B-12 mesh · B-13 soul ·
B-14 vision · B-15 self-model · B-16 failure analysis · B-17 blind spots.

**Naming deviation:** the spec says `ai-core/`; hyphens are illegal in Python
package names, so it is `aether/ai_core/`.

**One test skips on Windows:** B-05's symlink-escape case needs a privilege this
dev box lacks. It is B-05's discriminator, so it was verified separately under
WSL. It runs normally in CI on Linux.

---

- ✅ **BUG-1 (-smp 4): FIRST VALID DDR-777 READ — verdict (B), 2026-07-28.**
  Run **30323686134** (`smoke-swapgs`, -smp 4, head `ff6d1d4` which is PYTHON-ONLY,
  so the commit under test cannot be the cause) missed `[percpu] gs OK (syscall ctx)`.
  Discriminator read from the SERIAL DUMP (not the Makefile echo):
  probe `t=152` **present** and `user on AP OK` **present** (rules out (C));
  **23 heartbeats t=500→11500, gaps uniformly 500** (rules out (A) timer stall);
  no `[vblk] stuck`; no KHEAP PANIC (so **not** BUG-0).
  **Verdict (B): the BSP boot thread stopped progressing; the timer did not.**
  ALL userspace output lands before the first heartbeat (<5 s), then 115 s of
  nothing but heartbeats. Neither `gs OK` nor `gs FAIL` printed, proving the
  one-shot probe block was never entered rather than having run and failed.
  **DDR-776's negative is now ADMISSIBLE:** the heartbeat rides the same timer
  path and fired 23 times, so the watchdog provably ran ~115 times — its silence
  is real evidence that **no virtio-blk request was stuck**, removing virtio-blk
  from the suspect list on evidence rather than assumption.
  Last self-test region reached is the SFS churn/GC block in kmain, and the churn
  reported **FAIL** in this run. NO FIX PROPOSED — three theories were already
  refuted here by acting before the mechanism was named. Next step: an OPT-IN
  BSP-liveness marker in that region (same pattern as PIPE_TRACE so it cannot
  evict gate markers).
- ⚠️ **CORRECTION to the handoff briefing:** the DDR-790 pipe create/destroy
  traces are **NOT active in CI** — they were made opt-in (`PIPE_TRACE=0`) in
  `c068d8e` because unconditional traces evicted `smoke-dmesg`'s log-ring marker.
  "Read the traces from the next CI run" cannot work until a run is built with
  `PIPE_TRACE=1`. Also: the current CI red is `smoke-swapgs`, not `smoke-blkmq`;
  `8bfbad0` was kernel-GREEN.
- 🚨 **OPEN REGRESSION — DDR-790: kernel heap DOUBLE-FREE PANIC in CI, on `main`.**
  Run **30215987521** (`ba5770e`) died at step 54 `smoke-blkmq` (-smp 4) with
  `[kheap] double-free ptr=0x7E29F80 objsize=0x20` / `*** KHEAP PANIC ***`.
  **Prime suspect DDR-787** (pipe refcount split) — `struct pipe` is 24 B, the
  32-byte bucket — and DDR-787 **is already promoted to main**.
  TWO MISREADINGS CORRECTED ALONG THE WAY, both worth remembering:
  (a) `multi-inflight FAIL` in the log is the Makefile echoing the gate's
  `FORBIDDEN_SENTINEL=` line, NOT a kernel print; (b) zero `[hb]` heartbeats
  looked like the DDR-777 timer-stall verdict, but the run PANICKED — so the
  silence is a consequence, not evidence. **B#3 was NOT validly read here and
  remains open.**
  Diagnostic added (temporary): `[pipe] create/destroy p=… r=… w=…`.
  **It has NOT found the bug.** Its first reading — three pointers each freed
  twice — looked conclusive but was **kheap ADDRESS REUSE**; adding the paired
  create trace showed creates=4/destroys=4, perfectly balanced. *Pointer identity
  is not evidence of a double free; create/destroy pairing is.*
  Not reproduced locally: 3/3 smoke-blkmq clean, trace clean.
  One hardening applied and labelled as such, NOT as the fix: `pipe_close` now
  frees only if the call actually DROPPED a reference (the first cut freed
  whenever both counts merely READ 0, so a close decrementing nothing could free
  twice; the old single refcount masked that shape by going negative).
  **NEXT: let CI run with the traces; a `destroy` with no matching live `create`
  names the pipe, and its absence exonerates it and points at `struct vfs_file`.**
- **(previous) LAST_COMPLETED_TASK:** DDR-789 — PRISM **exit status `$?`**
  (Section B#12). **The tree check REORDERED the queue:** SIGPIPE was next, but
  (1) signal defaults are a WHITELIST not a table (`signal_deliver` terminates on
  SIGKILL, and on SIGTERM with no handler, and IGNORES everything else — so
  defining SIGPIPE 13 and raising it would silently do NOTHING), and (2) SIGPIPE
  cannot be gated discriminatingly today: PRISM has no `head`-like builtin and a
  stage's outcome is unobservable, so "writer killed" vs "writer ran on into a
  dead pipe" look identical. `$?` makes outcomes observable (a signal-killed
  thread exits -1), so it lands first AND unblocks SIGPIPE.
  Ring-3 only — `sys_wait4` already returns the raw exit code and PRISM already
  collected it and threw it away. Pipeline reports its LAST stage.
  **Design changed under test:** the draft expanded a token equal to exactly `$?`,
  which testing showed useless (the idiom `echo status=$?` is ONE token), so it
  widened to a token ENDING in `$?` — the gate was not contorted to fit the
  design. Two incidental fixes: `snprintf` is NOT in the musl subset (same gap as
  DDR-784's stderr) so a bounded `fmt_long` replaces it; and DDR-786's stage loop
  read wait4 into a `long` while the kernel copies out `sizeof(int)` — upper bytes
  were uninitialised, now `int`.
  Local: st-ok=0, st-fail=127, no literal `$?`, DDR-786/787 regressions intact
  (200/200 big pipe), zero panics/warnings.
  **NEXT: SIGPIPE is now gate-able** — a killed stage exits -1, distinguishable
  via `$?`. It still needs SIGPIPE added to the default-terminate list.
- **(previous) LAST_COMPLETED_TASK:** DDR-788 — retire the DDR-783 flake class now
  that DDR-785's early exit makes margin FREE for eligible gates. Measured scope
  first and it was smaller than assumed: of 92 boot_test.sh invocations, 38
  declare FORBIDDEN_SENTINEL (untouched — for them the timeout IS the runtime, so
  raising them would add ~57 min to every green run), 43 already had an explicit
  TIMEOUT_S, leaving **11** on the default.
  **A first-draft claim was corrected by measurement:** I asserted two gates were
  at risk; only **`smoke-fs-sfs-rw`** actually is (30 s against a 30 s window — it
  asserts the journal/version-isolation/compress chain DDR-783 timed at
  24.09–24.26 s). `smoke-fs-rw` measured **5 s** and was never at risk.
  Seven gates raised to TIMEOUT_S=120; three left alone on purpose (`smoke` ×2
  asserts only NEXUS KERNEL OK at t=0.31 s; `smoke-mkfs-sfs` is host-side).
  "Free on success" was CHECKED, not assumed: 4/3/4/5/30/34 s, all PASS under the
  120 s ceiling and all against the DDR-787 kernel. Cost on failure is real and
  stated: a failing eligible gate now burns 120 s instead of 30 s.
- **(previous) LAST_COMPLETED_TASK:** DDR-787 — **blocking pipe semantics**
  (kernel). Fixes a LATENT CORRECTNESS BUG in shipped DDR-780/786 behaviour: the
  `FD_PIPE` read path returned 0 whenever the ring was momentarily empty and every
  reader treats 0 as EOF, so `a | b` was TIMING-DEPENDENT — `b` printed nothing if
  scheduled first. Those pipeline gates were passing by scheduling luck.
  Neither that nor the >4 KiB silent truncation was fixable on the old
  `struct pipe`: a SINGLE `refcount` cannot tell readers from writers, and a
  reader may block only while a writer remains (and vice versa), so blocking
  without those counts would be UNBOUNDED — a direct S2 violation.
  Now: `refcount` → `readers`+`writers`, the end passed explicitly at all six
  incref/close sites (`pipe.c`, `fd_free`, `fd_clone`, `sys_dup2`) plus
  `pipe_destroy` for the never-installed error path; frees only when both hit 0.
  Reader waits while empty && writers>0 (and only when it has delivered nothing
  yet); writer waits while full && readers>0, else `-EPIPE` (**`EPIPE` did not
  exist in errno.h** — added as 32). Waiting via the `yield()` poll already used
  for FD_CONSOLE reads: no wait queue, no new scheduler hook, termination always
  the refcount condition and never a timeout.
  **Second bug found while implementing:** the writer's `if (w < chunk) break;`
  dropped the remainder of a partly-fitting chunk — blocking alone would NOT have
  fixed that; the loop now re-copies it.
  Gate: `cat /BIG8K.TXT | cat` (~7.8 KiB, 200 lines) asserting >=180 — pre-fix
  ceiling is ~107 at PIPE_SIZE 4096. **Counted, not exact-matched:** kernel prints
  share COM1 and can split a payload line mid-string (measured: 3/500 lost that
  way), so an exact assertion would flake for non-pipe reasons. Local at CI-like
  pacing: **200/200**, head+tail intact, all pipeline regressions PASS, zero
  panics. smoke-shell's 60 s window NOT raised.
- **(previous) LAST_COMPLETED_TASK:** DDR-786 — **multi-stage pipelines `a|b|c`**
  (Section B#12, fifth shell slice). DDR-780 honoured only the FIRST `|`, and its
  right child fell through having already passed the pipe block, so a second `|`
  hit the builtin as a literal token (`cat` tried to open a file named `|`).
  **Answer to the standing question: N stages do NOT force the ~120-line refactor
  DDR-780 deferred** — the fall-through dispatch is reused unchanged, only the
  pipe block grows (~40 lines). The ~10-line design (right child re-scans and
  re-forks) was **REJECTED**: it leaves an intermediate shell at each boundary
  holding the previous read end open, and with non-blocking 4 KiB pipes that
  silently DROPS output instead of hanging — invisible in a small test. Chosen:
  the shell splits all stages up front, forks each itself, threads one pipe
  between neighbours, closes `prev_read`+`fds[1]` in the parent right after each
  fork (DDR-780's EOF lesson, once per boundary), reaps all N. Malformed `|`
  (first/last/doubled) rejected BEFORE any fork. No kernel change.
  **Pre-existing limitation recorded, not introduced:** PIPE_SIZE 4096 +
  non-blocking `pipe_write` ⇒ a stage producing >4 KiB faster than its reader
  drains LOSES data (already true of DDR-780). Blocking pipe writes are a
  kernel-side slice if pipelines are ever used for real streaming.
  Local: 3-stage, 4-stage, malformed rejection, DDR-780/782/784 regressions, and
  shell-still-alive all PASS, zero panics, zero warnings.
- **(previous) LAST_COMPLETED_TASK:** DDR-785 — **`boot_test.sh` early exit**, the
  DDR-783 systemic finding now fixed. The harness always burned the full
  `TIMEOUT_S` then grepped, so the timeout WAS the runtime. Measured: 91
  invocations = **7590 s (126.5 min) of pure waiting per CI run**; the 53 gates
  with no forbidden patterns hold 4050 s of that, ~**43 min/run** of it idle.
  Now: poll the serial capture FILE (QEMU writes a file, not a pipe) and stop the
  guest once `$SENTINEL` + every `EXTRA_SENTINEL` is present; the verification
  block is untouched so verdicts and log-deletion are identical.
  **THE HAZARD IS EXCLUDED BY CONSTRUCTION, NOT MITIGATED:** early exit runs ONLY
  when `FORBIDDEN_SENTINEL` is empty — a forbidden pattern must NOT appear, and
  stopping early would prove only "not yet", so a gate that should FAIL could
  PASS. The 38 forbidden-declaring gates keep today's behaviour byte for byte
  (3540 s left unclaimed on purpose — a guarantee beats an argument). A settle
  window was REJECTED as a heuristic that cannot rule the false-negative out.
  New host-only `make smoke-selftest` (stub qemu, no kernel; wired into CI BEFORE
  the harness judges anything) asserts verdict AND timing — early exit 2 s vs a
  60 s window; **late forbidden pattern still took the full window and FAILED**;
  missing required still failed; declared-but-absent forbidden still passed.
  End-to-end: smoke-fs 30 s (60 s window), smoke-uaccess 4 s (30 s window).
  ✅ **CONFIRMED IN CI: run 30200918063 = 105.8 min vs the 152.3 min baseline
  (run 30193738689) — 46.5 min saved on the same 113-step suite**, matching the
  ~43 min predicted from the timeout-budget measurement. `smoke-selftest` passed
  as CI step 5, before the harness judged any gate.
- ✅ **PROMOTED (2026-07-26): `main` fast-forwarded to `ebd708d`** on green run
  30200918063 (113/113) — carries DDR-784 and DDR-785.
- ⚠️ **Watch for two runs per push:** `gh run list` also shows a **Dependabot
  Updates** workflow that completes fast and green. Do NOT mistake it for CI —
  filter on `workflowName == "pradyos-ci"` (this nearly caused a false "green"
  read this session).
- **(previous) LAST_COMPLETED_TASK:** DDR-784 — PRISM diagnostics on **stderr** +
  **`2>`** (Section B#12, fourth shell slice). **The prerequisite check re-scoped
  it again:** PRISM had ZERO writers to fd 2 (everything went `printf` → fd 1), so
  `2>` alone would have been untestable sugar. Two halves: route genuine errors to
  `fprintf(stderr, …)`, then add `2>`. Success messages stay on stdout —
  **`rm: removed …` is gate-asserted** and moving it would have broken
  `smoke-shell` silently (checked first, per lesson 3). No kernel change (fd 2 is
  already `FD_CONSOLE`; `fd_write_user` is fd-agnostic). **Surfaced only at link
  time:** the musl SUBSET had no `stderr.c`/`fprintf.c` (`undefined symbol:
  stderr`) and no `snprintf` either, so `tools/build_musl.sh` gained those two
  upstream sources — recorded, not folded in quietly. Gate discriminates by
  construction (stdout and stderr to DIFFERENT files in one command, so a broken
  `2>` hides the error in the stdout file). All local checks PASS, zero panics.
- **(previous) LAST_COMPLETED_TASK:** DDR-783 — `smoke-fs` ran at the harness
  default `TIMEOUT_S=30` while asserting the **last sentinel in the whole boot
  chain**. Measured: that sentinel lands at **t=24.26 s** locally (19 % margin),
  so a slower CI runner flakes it — which is what killed run 30192189559 at step
  10. **NOT a DDR-782 regression** (that slice never touches the kernel-internal
  `sfs_selftest_lz4`, and the same image passes locally). `smoke-user` already
  asserts the identical sentinel at 60 s, so this gate was simply never updated as
  the SFS chain grew. Fixed: `TIMEOUT_S=60` on `smoke-fs` alone — the other 56
  default-30 gates assert earlier sentinels and were NOT blind-tuned. Cannot mask
  a hang (`boot_test.sh` greps after the window regardless).
  **Systemic finding, proposal only, NOT applied:** the harness always burns the
  full window instead of exiting once all sentinels are seen — that is why every
  timeout needs hand-tuning and why wall-clock is the sum of timeouts, not of
  work. Early exit would kill this whole flake class AND speed up CI, but it sits
  behind 100+ gates and `FORBIDDEN_SENTINEL` must still fail if the forbidden
  pattern would have appeared after the early exit. Worth its own slice.
- ✅ **DDR-779 / ADR-033 IMPLEMENTED (2026-07-26) — musl mirror, maintainer
  signed off.** The hard stop is cleared. `.gitmodules` now fetches
  `third_party/musl` from `https://github.com/ifduyue/musl`; **the pinned commit
  is byte-identical** and the diff is one URL line. Forced by a **third** outage:
  run **30188805082** (`a077ccd`, DDR-782) died at step 2 `actions/checkout@v5`
  with the identical signature, 3 of 113 steps run — NOT a DDR-782 regression.
  **Two corrections found during verification:** (1) the mirror DDR-779 named,
  `bminor/musl`, **does not exist** (404 — applying the proposal verbatim would
  have broken CI differently); (2) content identity is confirmed by SHA **and
  tree** across three independent mirrors (`ifduyue`, `tianon`, `kraj` — all tree
  `2deb5f7c62d8c9e9733c9ed77d9210b708bbb69e`, equal to the local submodule fetched
  from **upstream**); `EOSIO/musl` and `AssemblyScript/musl` lack the commit and
  were rejected. Principle: the SHA is the contract, the host is availability
  only and swappable in one line. Upstream stays canonical for version bumps.
  **Residual risk:** a third-party mirror could be deleted — that fails loudly,
  not silently. **The proof this worked is a run getting PAST step 2.**
- ✅ **PROMOTED (2026-07-26): `main` fast-forwarded to `721807f`** on CI run
  **30184411583** (conclusion `success`, 112/112 steps, zero failures). That run
  promotes the whole backlog — `21e4a51` (DDR-778 `>`), `d8d78e4` (DDR-779
  finding), `6561a74` (DDR-780 `|`), `721807f` (DDR-781 `>>`/`<`) — and CI-validates
  all four PRISM shell assertion sets, retiring the risk that the gate's tighter
  FIFO pacing (0.5–0.7 s vs the 1–2 s used locally) would drop markers.
- **LAST_COMPLETED_TASK (newest):** DDR-782 kernel `O_TRUNC` + atomic `O_APPEND`
  (Section B#12, kernel-side remainder) — closes the two REAL defects DDR-781
  recorded. **The prerequisite check re-scoped it:** `struct vfs_fs_ops` has **NO
  truncate op** and no FS driver (FAT32/SFS/ext4) can shorten a file, so a literal
  `ftruncate` would be a new VFS op implemented three times — reported instead of
  invented. `O_TRUNC` = `vfs_unlink` + `vfs_create` on an existing file
  (truncation-to-zero, all `>` needs; honest limitation: fresh inode/cookie,
  non-zero `ftruncate` still impossible). `O_APPEND` = fd-layer only, `e->off =
  e->file->size` once per `write()` call in `sys_io.c` before the chunk loop —
  that IS the atomicity property. Flags live in `sys_file.h` (Linux values
  `O_TRUNC 0x200`, `O_APPEND 0x400`) since two TUs honour them. No new syscall, no
  new VFS op, no on-disk change, **no capability change** (CAP_FS_WRITE already
  gates create/unlink/write). PRISM's `>`/`>>` use the flags and the
  `SYS_LSEEK`/`SEEK_END` defines are removed (no dead refs). New `smoke-shell`
  check is discriminating BY CONSTRUCTION — long write then short write, and the
  long record's `TAIL9x3` tail must be ABSENT (it survives under the old
  behaviour). Local: truncate, append, pipe and `<` all PASS, zero panics.
  S2 + S6 apply; no invariant weakened.
- **(previous) LAST_COMPLETED_TASK:** DDR-781 PRISM `<` and `>>` (Section B#12,
  third slice) — ring-3 only. **The prerequisite check changed the mechanism:**
  the kernel has **NO `O_APPEND`** (`sys_file.c` honours only `O_CREAT`), so the
  planned open-flag does not exist; `SYS_LSEEK = 10` DOES support `SEEK_END`, so
  append = `open` + seek-to-EOF in ring 3. Documented as NOT atomic `O_APPEND`
  (fine: one writer per command). Also exposed a PRE-EXISTING gap — there is no
  `O_TRUNC`, so DDR-778's `>` does not truncate; recorded, out of scope.
  All discriminating checks PASS locally (append kept BOTH records; `<` gave the
  marker with no `cat: cannot open <`; pipe regression intact; no panic).
- **NEXT_TASK — B#12 remainder or a new item.** Left in B#12: stderr (`2>`),
  multi-stage pipelines (`a|b|c`), job control (`&`, job table — needs signal
  plumbing). Kernel-side prerequisites for atomic `O_APPEND` / `O_TRUNC` would be
  their own slice touching `sys_file.c` + the VFS write path — scope explicitly,
  do not slip in. Otherwise pick a fresh Section B item, and CHECK THE TREE FIRST
  (#5, #7, #8 were all stale "planned" entries already shipped).
- **(superseded) earlier NEXT_TASK: `|` between two commands.** NOTE (already
  established, don't rediscover): PRISM builtins are INTERNAL FUNCTIONS, not
  execs, so piping needs a fork around the **builtin dispatch** itself. Then `<`,
  `>>`, stderr; job control (`&`, job table) is a separate slice needing signal
  plumbing. Alternatives if B#12 stalls: B#4 SFS-as-default-root (invasive —
  global boot topology), B#6 ext4 write (large). **Check the tree before planning
  ANY Section B item** — #5, #7 and #8 were all stale "planned" entries that were
  already shipped.
- **(superseded) earlier framing: B#12 pipes / redirection / job control** (best remaining
  bounded+gateable item: ring-3 only, `user/prism.c`, zero kernel risk, and the
  kernel side already ships — `SYS_PIPE`, `dup2`, `SIGPIPE`, gates `smoke-syspipe`
  / the CI "pipe/dup2" step). Scope it SMALL in its DDR: start with output
  redirection `>` for one command (uses existing SYS_OPEN + dup2), then `|`
  between two commands via fork+pipe. NOTE PRISM builtins are internal functions,
  not execs, so piping *builtins* needs a fork around the builtin dispatch —
  decide that explicitly in the DDR rather than discovering it mid-implementation.
  Gate: extend `smoke-shell` with a deterministic redirect/pipe assertion.
  Alternatives weighed and rejected for now: B#4 SFS-as-default-root (invasive —
  changes global boot topology, needs broad gate validation), B#6 ext4 write
  (large, ADR-019 extension + journal), B#9 I/O APIC (low priority, replaces 8259).
- **(superseded) earlier framing: read the discriminator, then fix what it names.** Grep every
  future run for `\[hb\] t=`, `user-on-AP probe t=`, and `vblk\] stuck`. Only
  after the mechanism is named should a behavioural fix be written — blind fixes
  are now THREE times refuted (DDR-771 timeout bump; the virtio-blk theory; the
  "timeout ⇒ hang" reading).
- **(superseded) earlier plan: prove or refute the stalled-timer hypothesis.**
  Decisive, cheap experiment: (a) a heartbeat print driven from the timer path in
  `idt.c` (throttled, e.g. every 500 ticks: `[hb] t=<g_ticks> cpu=<id>`), and
  (b) print `g_ticks` at entry and exit of the AP proofs in main.c. If the
  heartbeat STOPS during a failing CI run, the LAPIC/timer path under `-smp 4` is
  the root cause and B#3 moves there entirely. If the heartbeat KEEPS TICKING
  while `smpuser_proof` still prints nothing, the hypothesis is refuted and the
  stall is inside the proof/scheduler instead. Same design rules as DDR-776:
  passive observer, timer-driven, NO locks from the ISR (S6), bounded + print
  throttled (S2), deterministic. Do NOT add another timeout bump and do NOT
  attempt a behavioural fix — blind fixes are now TWICE refuted.
- **(superseded) earlier plan: instrument the AP/scheduler path, NOT another
  blk fix.** Do for the SMP/AP proofs what DDR-776 did for blk: bounded,
  deterministic progress prints so the next failure names WHICH AP proof stalls
  and where. Candidates to instrument: the user-on-AP handoff (`[smp] user on AP
  OK` emitter) and the percpu run-queue/AP wake path. Keep every added wait
  bounded (S2) and every print deterministic; do NOT add another timeout bump.
  Only after the failing step is named should a behavioural fix be attempted —
  blind fixes are now TWICE refuted (timeout bump, then the blk hypothesis).
- **(deferred, still valid on merit) the virtio-blk S2 fix (bound the wait),** FIRST check whether any CI run since DDR-776 printed
  `[vblk] stuck dev=… slot=… lba=… age=…`: if it did, that names the stuck request
  and the trigger (missed IRQ vs lost `virtq_pop_used` vs `head2slot` corruption)
  becomes decidable — root-cause THAT rather than guessing. Then bound
  `submit()`'s completion wait, which per DDR-776 means adding a **scheduler
  timed-block** (`sched_block_on()` has no timeout today) — a genuine scheduler
  feature touching `kernel/proc/sched.{c,h}`, so scope it in its own DDR, do not
  slip it in. On timeout you MUST release the slot (`v->req[s].used = 0`), clear
  `v->req[s].waiter`, and wake any `v->slot_waiter`, or you leak a slot. Also fix
  Hazard 2: `slot_waiter` is a single `struct tcb *`, so >VBLK_NREQ(8) concurrent
  submitters lose a waiter — make it a real wait list or wake-all + re-check.
  Remember: the bug does NOT reproduce locally and is intermittent in CI, so ONE
  green run does not prove a fix — say "not yet proven".
- **(superseded) earlier framing of the B#3 fix slice, per DDR-775:**
  (1) BOUND the wait first (S2): replace the unbounded `sched_block_on` loop with
  a deadline so a missed completion becomes an I/O error + `[vblk] completion
  timeout slot=N lba=…` diagnostic. ⚠ `sched_block_on()` has NO timeout today, so
  this needs either a new scheduler timed-block OR a bounded `g_ticks` yield-loop
  — that choice is the design decision of the slice and must be made explicitly,
  since a yield-loop gives up the deliberate "sleep, never spin" property.
  (2) Then let the diagnostic name the real trigger (missed IRQ vs lost
  `virtq_pop_used` vs `head2slot` corruption) instead of guessing.
  (3) Fix `slot_waiter` into a real wait list (or wake-all + re-check).
- **(superseded framing) Section B#3, the `-smp 4` hang — TOP BLOCKER:** The reproducer requirement is now partly met — a
  concrete CI signature exists (hang after `SYSFSTAT OK`, `-smp 4`, 180 s). Plan:
  (1) try to reproduce locally with `make smoke-surfdestroy` a few times (it has
  historically passed locally, so expect CI-only timing); (2) if it will not
  reproduce, add bounded `g_ticks` deadline polls + progress prints around the
  ring-3 systest sequence *after* `SYSFSTAT` so the next CI failure pinpoints the
  stuck step instead of just ending at a sentinel; (3) DO NOT paper over it with
  another timeout bump — the 180 s bound was already ample. Write the DDR first
  citing Section B#3; S2 (bounded everything) governs any new wait.
- **DEFERRED — DDR-774c phase c-2** (Master doc Section B#1, optional perf).
  Delivery is now PROVEN (`irqs=6`, `PRADYOS_NVME_IRQ_OK` gated), so the
  completion path can finally be converted: make `nvme_submit()` wait on an
  IRQ-set completion flag with a **BOUNDED** spin fallback (S2) so a lost or
  misrouted interrupt degrades to polling instead of hanging the boot.
  ⚠ The real risk in c-2 is **not** the interrupt — it is **CQ head/phase
  ownership (S6)**. Today the polling loop in `nvme_submit()` exclusively owns
  reading the phase bit, advancing `cq_head` and ringing the CQ head doorbell,
  while the ISR only increments a counter. c-2 must transfer that ownership
  atomically, not run both concurrently, or an interrupt arriving mid-poll will
  corrupt an in-flight command. Simplest safe design: ISR sets a per-queue
  "completion pending" flag only; `nvme_submit()` still does all CQ mutation, but
  waits on the flag instead of spinning on the phase bit. Keep `smoke-nvme`'s
  RW_OK / PRP_OK / IRQ_OK all green, and keep the DDR-772 multi-command PRP loop
  correct (it issues several commands per call).
  After c-2, Section B#1 is complete; next candidates are B#3 (`-smp 4` race —
  needs a narrow reproducer first) and B#4 (SFS as default process root —
  invasive, global boot topology).
  Alternatives if 774a is rejected: B#3 `-smp 4` race (needs a narrow reproducer
  first; the DDR-771 timeout bump is mitigation, not root cause) or B#4 SFS as
  default process root (invasive — global boot topology, broad gate validation).
  Section E/F stay gated behind a DDR answering the architecture prerequisite
  checklist; F#68 metric lockbox (= invariant S3) is the highest-priority proposed
  item now that Section D is confirmed built.
- **NEXT_TASK (M2/M3):** the mkfs.sfs storage chain (765-770) is complete —
  NVMe + host mkfs.sfs + kernel reads it + nested dirs + persistent root from a
  build image. Remaining open items (pick per priority): lift `VBLK_MAX` past 4
  (MSI-X vector remap) so the provisioned root can be the DEFAULT alongside ext4
  + retire blk2's dual role; FS write budget (1 MiB per-thread LIFETIME) design
  review; `-smp 4` SMP-race root-cause (timeout bumped as interim mitigation);
  NVMe PRP-list (>page single commands) + NVMe IRQ; mkfs multi-leaf trees (>14
  slots). Consider whether M2 is declarable done.

### 0.-1b PRIOR TRACKER (superseded)

- **LAST_COMPLETED_TASK:** DDR-763 SFS B+tree churn (misdiagnosis corrected) —
  CI-green on `main` at `d7d4123`. 97 gates.
- **CURRENT_ACTIVE_TASK:** DDR-762-v2 SFS free-space reclamation — free-EXTENT-RUN
  allocator (`free_runs[256]`, `alloc_run` first-fit+split, snapshot-guarded
  `free_run` on unlink, write uses `alloc_run(nblocks)`). Gate `smoke-sfs-gc` (300×
  create/write/unlink, incompressible data, budget-refreshed) — verified
  discriminating. Full SFS+SMP regression green. Local green; pushed to
  `dev/phase1`, CI-verifying. 98 gates.
- **RETRACTED FINDING — there was NO SFS B+tree bug.** DDR-763 reproduced the
  prior "bug" with per-return-path instrumentation in `sfs_write`: NONE of the
  `sfs_write` markers fired, so `sfs_write` was never reached. The write failed
  earlier in `vfs_write`: `if (current_thread->fs_write_budget < len) return -1`
  (FS_WRITE_BUDGET_DEFAULT = 1 MiB, `sched.h`). The boot thread writes all ~20
  embedded ELFs to SFS (incl. ~100 KB+ musl daemon/PRISM) + FS self-tests,
  consuming most of its 1 MiB; only ~10 more 64 KB writes fit → fail at ~cycle 11.
  The ~14-slot leaf-split coincidence was a red herring. PROOF: refresh the boot
  thread's budget → churn reaches 40/40, failop=0. `sfs.c` UNCHANGED. B+tree is
  sound.
- **NEXT_TASK:** M2 continues —
    1. **NVMe driver** (registers with the blk layer; needs an NVMe QEMU disk —
       adds boot disk topology). Large new driver.
    2. **host `mkfs.sfs`** — cross-reboot SFS persistence + on-disk free-extent
       tree (the in-memory `free_runs` from DDR-762-v2 is within-a-boot only).
    3. (Design note, revisit before M2 done) the 1 MiB *lifetime* per-thread FS
       write budget (`FS_WRITE_BUDGET_DEFAULT`) is very restrictive for a real
       process writing many files on the persistent SFS root — a separate future
       decision (higher / refillable / per-op), not a bug.
    4. (Open audit) `smoke-percpu-sched` intermittent `-smp 4` early-boot FS flake
       — must be root-caused before M2 is declared complete.
- **AUDIT FINDING (open, recurring low-freq): `-smp 4` one-shot boot-proof
  window flakes.** Two distinct `-smp 4` gates have each failed ONCE on CI, always
  0/3–4 locally, always with the commit only shifting boot timing (never in the
  failing path's logic):
    - `smoke-percpu-sched` (run 29634662558): boot FS self-tests red
      (`/HELLO.TXT not found`, `[sfs] created 0`) — run before any user spawn.
    - `smoke-msixap` (run 29648027891): `[blk] msix on AP OK` not observed under
      DDR-761 (a daemon-config change unrelated to the blk/MSI-X path).
  ROOT PATTERN: these gates assert a one-shot boot proof (`compl_ap` set, per-CPU
  sched OK) by CHECKING ONCE at a fixed boot point; on slow TCG under `-smp 4` the
  awaited cross-CPU event sometimes hasn't landed yet, so the check fails even
  though the mechanism is correct. FIX: poll the condition with a bounded `g_ticks`
  deadline instead of a single check.
    - **`msixap` FIXED (this commit):** the proof now polls
      `virtio_blk_completed_on_ap()` (deadline `g_ticks+200`) while ISSUING blk0
      reads — unit 0's MSI-X vector targets an AP, so the loop forces the very
      completion it waits for. Deterministic; 5/5 local. No kernel-logic change.
    - **STILL OPEN:** `percpu-sched` (different symptom — early block I/O
      intermittently failing, not a check-once proof; needs its own root-cause)
      and `smpjob` (already has a per-AP deadline; lower risk). Later audit slice.
- **Milestone track:** M1 kernel hardening (per the revised master prompt §11);
  then M2 storage (persistent SFS root half-2/2, GC, NVMe).

### 0.0.1 STATE DELTA (2026-07-15 — DDR-743..756, supersedes §0.0 below)

- **HEAD:** `69d3474` (`main` == `dev/phase1`, both pushed, CI-green). **92 gates.**
- Shipped since §0.0 (each DDR-first + CI-green before main ff):
  **743** SYS_GETPROCS/`ps` · **744** ring-3 file lifecycle (O_CREAT + SYS_UNLINK
  + the FD_VFS write path, which was a stub) · **745** PRISM touch/rm ·
  **746** ACPI S5 poweroff (SYS_POWEROFF 69, CAP_SOVEREIGN; compositor `p`) ·
  **747** ACPI reboot (SYS_REBOOT 70; FADT reset reg + 0xCF9 + 8042; `b`) ·
  **748** SYS_SYSINFO 71 (CPUID vendor/brand, cpus, uptime, free pages) ·
  **749** SYS_TIME 72 (RTC broken-down) · **750** klog ring + SYS_DMESG 73 ·
  **751** PRISM uname/date/uptime/dmesg · **752** SYS_MEMINFO 74 + PMM
  total-RAM tracking + PRISM free · **753** TCP loopback echo self-test
  (client path; `smoke-net-tcp-lo`) · **754** ps CPU accounting (procinfo grew
  run_ticks/dispatches) · **755** kill end-to-end (`smoke-kill`) + PRISM kill ·
  **756** SYS_SETNAME 75 (tcb.name_buf; `smoke-setname`).
- **NSI extends through 75.** PRISM builtins: help echo cat run ls ps kill
  setname touch rm uname date uptime dmesg free mode exit.
- Known env quirks: WSL pipe exit codes flake (route output to build/*.log);
  boot_test deletes its serial log (use explicit qemu run to inspect output);
  `ls`-gate prompt-prefix flake fixed in 743 (`(^|prism> )` anchor pattern).

### 0.0 CURRENT STATE (updated 2026-07-04 — superseded by §0.0.1 above)

- **HEAD:** `59d7ac4` (`main` == `dev/phase1`, both pushed). **CI: all 57 gates
  green** (run 28683624946). Git runs from native Windows (PowerShell); builds
  in WSL **Ubuntu-24.04**. `third_party/{musl,lwip}` are submodules — run
  `git submodule update --init --recursive` after a fresh clone/hard reset.
- **SMP track (ADR-030, staged — 5 slices CI-green after the 52-gate state):**
  stage 1 PMM/kheap/console under spinlocks (`smoke-smplock`); stage 2 percpu
  identity (`smoke-percpu`); stage 3a **SWAPGS discipline** on all 4 ring
  transitions + `this_cpu()` = `%gs:0` (`smoke-swapgs`); stage 3b
  `current_thread`@`%gs:8` + SYSCALL kstack@`%gs:16` — `current_thread` is a
  sched.h macro, `percpu_init_early` runs right after `gdt_init` (its gs reload
  zeroes the base!) (`smoke-percpu-sched`); stage 3c-alpha **AP work dispatch**
  — wake IPI vector 49 + single-slot percpu mailbox, `smp_run_on`/
  `smp_job_done`; APs `idt_load_ap()` before sti (real-mode IDTR leftover
  triple-faulted otherwise) (`smoke-smpjob`).
- **3c-locks-1 shipped (`smoke-crosswake`):** the scheduler runs under
  `g_sched_lock` (held across `context_switch`; a NEW thread's first entry
  releases it in `thread_trampoline` — a leak would deadlock); `sched_unblock`
  is an atomic CAS callable from APs. **Loader contract changed
  (DDR-boot-authority-race, fixed the 3× `smoke-agents` CI flake):**
  `elf_load` returns threads **BLOCKED** — callers set `is_sovereign`/`is_agent`
  and then `sched_unblock` (see `elf.h`; `user_boot_from_sfs` gained a
  `sovereign` param). HEAD is now `e5e9f56`, 58 gates green (run 28699475194).
- **Full 3c is DONE — ADR-031 complete (2026-07-05, HEAD `62b9a97`, 61 gates).**
  The lock slices (locks-2 block sleep-mutex, locks-3 per-mount VFS, locks-4
  IPC/bcast + `sched_block_on`) then the capstone: cap-1 per-CPU TSS/GDT/TR,
  cap-2a SMP-safe ring (`on_cpu` claim, locked topology, BLOCKED-create),
  cap-2b APs run kernel threads (per-CPU idle — AP idles kmalloc'd, not BSS;
  `g_sched_ready`; `smoke-smpsched`), cap-3 per-AP LAPIC-timer preemption
  (global tick side-effects BSP-only or `g_ticks` runs Nx fast;
  `smoke-smppreempt`), cap-4 ring 3 on every core (per-CPU SYSCALL snapshot in
  percpu @56..120; per-AP `cpu_enable_sse`/NXE/SYSCALL-MSRs — see memory
  `ap-percpu-machine-state`; `smoke-smpuser`). ADR-016 fully superseded for the
  scheduler. Still deferred: IDT gate for the spurious vector (0xFF), per-CPU
  runqueues/affinity.
- **DDR-714 stage C is DONE (2026-07-06, HEAD `ea7977f`, 62 gates):** C1/C2 =
  MSI-X for ALL virtio devices (blk vectors 50..53, net 54 both queues, input
  55; `virtio_pci_msix_setup`, IDT→56 stubs, `msix_register` + LAPIC-only EOI;
  INTx fallback kept; engagement pinned in smoke-fs/smoke-net-lo). The 8259
  now carries ONLY ISA (keyboard IRQ1, COM1 RX IRQ4) — I/O APIC deliberately
  skipped (q35 PIC-mode Interrupt Line ≠ IOAPIC GSI; MSI-X sidesteps it). C3 =
  blk vectors distributed round-robin across APs; the locks-2 completion review
  done (per-device `compl_lock` + `sched_block_on` closes the cross-CPU lost
  wakeup; net/input stay BSP-routed — lwIP is single-threaded by design). Gate
  `smoke-msixap`. Also fixed en route (CI-caught): the exit-vs-collect kstack
  use-after-free (`sched_exit` now holds `g_sched_lock` ACROSS its final
  switch — DDR-SMP-exit-stack-race).
- **Multi-in-flight blk is DONE (DDR-BLK-1, HEAD `e0c8f79`, 63 gates):** the
  one-in-flight `busy` mutex is deleted — 8 per-request slots per disk
  (`head2slot[]`), ALL vq+slot state under `compl_lock`, slot exhaustion
  sleeps via `sched_block_on`. Gate `smoke-blkmq`.
- **Per-CPU runqueues are DONE (DDR-SMP-rq-1, 64 gates):** O(1) pick from
  per-CPU FIFOs + trylock work stealing; the ring is topology only. Bugs found:
  idle starvation (rotate through the own idle on empty queues); COMPOSIT now
  spawns before SURFTEST (client outraced compositor init). `smoke-rqstress`.
- **`g_sched_lock` is OFF the switch path (DDR-SMP-rq-2, HEAD `176e329`,
  73 gates):** the hot path takes only local IRQ masking + its own rq leaf
  lock. The lock's two real jobs became an explicit **off-CPU handshake**:
  `on_cpu >= 0` = "still executing / rsp not saved", release-stored AFTER the
  switch by whoever resumes on that CPU (`finish_task_switch` reading the new
  `percpu.prev`); pickers `switch_wait_offcpu()` (acquire-spin) before touching
  `next->rsp`; `sched_free_tcb` waits on it, so `sched_exit` no longer holds the
  lock across its final switch (exit-stack-race guarantee preserved, mechanism
  local). `rq_take` dropped `on_cpu` from its filter — the DEQUEUE is the
  exclusion — which RETIRED rq-1's transient re-append. `g_sched_lock` now
  covers ring topology only. `percpu.prev` must stay AFTER the asm-consumed
  `u_*` block (static asserts enforce).
- **Per-wake reschedule IPIs are DONE (DDR-SMP-rq-3, 74 gates):** rq-2's
  deferred half. `sched_unblock` now finds an idle CPU (`percpu.idle`) and sends
  a directed `smp_resched_one` wake IPI (vector-49 wake ISR) so the woken thread
  is stolen immediately instead of on the AP's next 100 Hz tick. The wake/halt
  race is closed by the idle loop's double-check (set `idle=1`, re-scan
  `rq_has_ready()`, loop before `hlt`), timer as backstop. Gate `smoke-resched`
  (`g_resched_ipis > 0`). **Fixed a latent bug rq-3 exposed:** `sys_write`'s
  console path emitted user bytes via a bare per-char `kputc` loop with no lock,
  so a ring-3 printer on an AP garbled mid-line against another CPU's `kputs`.
  Root-caused with `kwrite(buf,n)` in `console.c` (takes `g_console_lock` once
  per chunk → user writes get `kputs`'s line-atomicity). `u_*` static asserts
  unaffected (`idle` appended after `prev`).
- **L7 polish resumed:** **DDR-720** Tab window cycling (compositor hotkey,
  `smoke-alttab`), **DDR-721** double-buffered page flip (two host GPU
  resources over one guest buffer; flush = transfer-offscreen → scanout-flip;
  `smoke-flip`), **DDR-722** real glass blur + saturation (separable box blur
  under cards, tint BLENDS over — an opaque fill erases the blur;
  `smoke-glassblur`), **DDR-723** multi-stop gradient backdrops
  (`smoke-gradient`), **DDR-724** window decorations — focus-colored frame +
  drop shadow (`smoke-decor`). HEAD `a184120`, **69 gates**, all CI-green.
  **DDR-725** scroll-wheel plumbing (REL_WHEEL → `mouse_state.wheel` →
  type-2 surface event to focus; `wheel_inject.sh`; `smoke-scroll`).
  **DDR-726** auto ambiance cadence + pre-transition pulse (frame-loop time
  base, `k` test knob, `smoke-cadence`). **DDR-727** spring toggle + click
  ripple (`smoke-motion`; `smoke-mouse` also asserts the ripple). **DDR-728**
  the Inter typeface as a 16px alpha glyph ATLAS — `tools/fontgen/gen_inter.c`
  + vendored public-domain `stb_truetype.h` rasterize Inter-Regular (SIL OFL)
  into the generated, committed `user/inter_font.h` (~22 KB); the TTF is NOT
  vendored (rendered bitmaps aren't font software; the no-out-of-tree-libs wall
  governs the OS IMAGE, not build-host tooling). Proportional titles via
  `draw_str_inter`; 8×8 face kept for small labels. `smoke-font`.
  HEAD `aa7f548`, **73 gates**, all CI-green.
- **The DDR-702..709 deferred VISUAL list is CLOSED** — glass blur, gradients,
  typeface, cadence + pre-transition, spring/ripple, page-flip, scroll,
  decorations, alt-tab all shipped.
- **Surface destroy is DONE (DDR-729, 75 gates):** completes the surface
  lifecycle. Root-fixed ownership: client/compositor mappings are now
  `PTE_SW_SHARED` views (vDSO precedent), so `vmm_destroy_address_space` never
  frees surface/FB frames — the surface layer is the sole owner. `sched_exit`
  calls `surface_reap_pid()` so a client that dies without `SYS_SURFACE_CLOSE`
  no longer leaks its 16-slot table entry + frames (previously `-EMFILE` after a
  few create/exit cycles; also closed a latent double-free vs. AS teardown and on
  sovereign-close). A `g_surf_lock` leaf spinlock makes the slot lifecycle
  SMP-safe. Gate `smoke-surfdestroy` (`-smp 4`, freestanding
  `user/surfdestroytest.c` — musl-free to stay inside the 512 KiB kernel-image
  budget) proves churn + slot-reuse + exit-reclamation. Also fixed the identical
  missing-shared-bit flaw in `sys_fb.c`.
- **Per-agent live metrics are DONE (DDR-730, 76 gates):** roster slots are now
  `{used, pid, actions}`; "active" is DERIVED (slot pid resolves to a live agent
  tcb), root-fixing DDR-707's never-cleared active bit — dead agents' cards
  self-dim with no teardown hook. New `SYS_AGENT_METRICS` (NSI 64;
  `MAX_SYSCALLS` 64->80) returns `{pid, state, mem_used, actions}` live from the
  tcb; `SYS_AGENT_ROSTER` shares the liveness check. Witness = freestanding
  `user/agentmetricstest.c` probe (gate `smoke-agentmetrics`); the compositor is
  untouched (its musl ELF had no image headroom).
  **Kernel load window is at its real-mode CEILING: 544 KiB** (stage2 17x64
  sectors, ends 0x98000 < conventional-RAM top 0x9FC00). A 24-chunk/768 KiB
  attempt hung the boot — the 0xA0000 VGA/ROM hole makes a bigger flat load at
  0x10000 impossible. Past 544 KiB the kernel must be relocated above 1 MiB
  (unreal-mode bounce copy in stage2) — plan it as a dedicated boot slice.
- **CAP_NET is DONE (DDR-731, 77 gates):** the socket NSI is gated. New
  `tcb.is_net` (granted to agents at spawn; zeroed in `sched_create_state`, not
  fork-inherited); `SYS_SOCK_CONNECT` needs `is_net || is_sovereign` (audited
  `-EPERM`); per-slot `g_sock_owner` enforced on WRITE/READ/CLOSE (no
  cross-process hijack); `socket_reap_pid` from `sched_exit` (DDR-729 pattern).
  Gate `smoke-capnet` (freestanding `user/capnettest.c`).
- **Kernel relocated to 4 MiB (DDR-733, 77 gates):** the flat load at 0x10000
  hit its ~575 KiB file+BSS ceiling (BSS tail crossed 0x9FC00 into the EBDA →
  #GP on the first tick, gate-caught). Stage2 now INT13-reads into a 0x10000
  bounce buffer and unreal-mode-copies each chunk to `KERNEL_PHYS = 0x400000`
  (SDM Vol.3 §9.9.2; DS/ES limits re-armed per chunk under cli). 24-chunk /
  768 KiB read window; runtime ceiling = the 2 MiB PT_HI span, enforced by an
  nm-based `__bss_end` Makefile check. kernel.ld `KERNEL_LMA = 0x400000`.
- **AETHER boot config is DONE (DDR-732, 78 gates):** the daemon reads
  `/AETHER.CFG` (FAT32 boot volume, mcopy'd at image build) for mode/task/slot;
  compiled defaults + `PRADYOS_AETHER_CFG_DEFAULT` on any config problem. Gate
  `smoke-aethercfg`. (Not SFS: ring 3's root_mnt is the FAT volume; renaming to
  /etc/aether/config awaits SFS-as-process-root.)
  **ALL deferred non-visual L7 items are now CLOSED.**
- **CAP_NET allowlist is DONE (DDR-734, 79 gates) — live-agent hardening 1/3:**
  deny-by-default egress for CAP_NET agents. Bounded kernel list ({host_be,
  port}, port 0 = any), consulted in `sys_sock_connect` (sovereign bypasses);
  sovereign-only install NSI `SYS_NET_ALLOW` (65, install-only — no revocation
  surface); daemon installs `net=<ip>:<port>` rows from `/AETHER.CFG` before
  any agent spawns (default `net=10.0.2.2:11434` keeps smoke-agent-live
  working). Gate `smoke-netallow`.
- **Agent CPU metric is DONE (DDR-735, 79 gates) — live-agent hardening 2/3:**
  `SYS_AGENT_METRICS` gains `run_ticks` (from `sched_tick`) + `dispatches` (from
  `schedule()`), owner-CPU-written (no hot-path lock). The roster slot retains
  the counts past agent exit so a short-lived agent is provable; the probe
  latches "alive" and "dispatched" independently. Extended `smoke-agentmetrics`.
  Also: `boot_test.sh` `SERIAL_LOG` is now overridable (this WSL wipes /tmp
  mid-run — set SERIAL_LOG to a persistent path for reliable local gates; CI
  default unchanged). Hardening 3/3 = draw the counts on the agent cards.
- **rq double-enqueue FIXED (DDR-736):** the DDR-735-era CI failures (kfree
  double-free; smpsched hang) had one root cause — `rq_push`'s `rq_on` check
  ran under the target queue's lock, so a waker's unblock and the blocker's own
  schedule() re-queue (two DIFFERENT leaf locks) could link one tcb into TWO
  FIFOs via its single `rq_next` (list corruption = hang; double-pop/double-run
  = double free). The `rq_on` claim is now an atomic exchange BEFORE any queue
  lock; `rq_take` clears it with RELEASE. Also hardened `thread_trampoline`
  (keeps `on_cpu` set until `finish_task_switch`, matching sched_exit) and the
  KASAN double-free panic now prints ptr+objsize. Race predates DDR-735; CI's
  TCG runners surfaced it. LOCAL GATE NOTE: back-to-back local runs flake on a
  QEMU image-lock release race + this WSL wipes /tmp (use SERIAL_LOG=<persistent
  path>, sleep 1 between gates); CI (isolated steps) is authoritative.
- **smoke-agentmetrics made TCG-deterministic (DDR-735 gate fix):** the gate's
  alive-window assertion was racy on TCG (an agent's whole life fits inside one
  slow compositor frame). `sched_exit` now captures final counters into the
  roster slot (`agent_metrics_reap`) and retains the pid; the probe asserts
  post-mortem facts (`pid!=0 && dispatches>=1` vs empty slot 7), RTC-bounded.
- **Agent-panel metrics are DONE (DDR-737, 80 gates) — hardening campaign
  CLOSED (1/3 CAP_NET allowlist, 2/3 CPU metrics, 3/3 panel UI):** cards render
  from `SYS_AGENT_METRICS` — state-colored dot (dim green = ran-and-exited via
  retained pid) + up to 4 action pips; one-shot `AGENT_PANEL KRYOS` witness on
  the post-mortem-stable fact. Gate `smoke-agentpanel` (GPU). The DDR-730
  image-budget blocker is gone (DDR-733: kernel 545 KiB of 768).
- **SFS hierarchical dirs are DONE (DDR-738, 81 gates):** path walk +
  `SFS_INO_DIR` inode flag (root implicitly a dir) + mkdir-p on create; `open`
  requires dir intermediates, `readdir` walks to the target dir. SFS-local (VFS
  vtable unchanged; process root still FAT). Gate `smoke-sfs-dirs` builds
  `/etc/aether/config`. FOLLOW-ONS: (a) switch a process's root_mnt to SFS
  (needs image-time SFS tree provisioning — the kernel currently writes SFS
  files at boot, there is no host mkfs.sfs); (b) then move /AETHER.CFG ->
  /etc/aether/config. rmdir/unlink-dir, `.`/`..`, hard links still absent.
- **Per-process root mount is DONE (DDR-739, 82 gates) — SFS-as-root half 1/2:**
  a process can be spawned with a chosen root_mnt (spawner sets t->root_mnt
  before sched_unblock; fork inherits). Proven by an ext4-rooted ring-3 probe
  (user/rootmounttest.c) opening /EXT4.TXT + failing /HELLO.TXT. ext4 (blk3) is
  mounted ONCE early in kmain and reused by the ext4 self-test. Gate
  smoke-rootmount (needs ext4 disk). HALF 2/2 (the actual SFS root) still needs a
  PERSISTENT SFS volume — the destructive SFS self-tests reformat the only SFS
  disk — plus image-time /etc/aether/config provisioning.
- **Context-switch lazy FPU is DONE (DDR-740):** schedule() only fxsave/fxrstor's
  the FPU across USER threads now (guarded on is_user) — kernel threads are
  -mgeneral-regs-only and never touch it. Switch cost ~1881 -> ~1054 ns (44%),
  under the <=1500 ns target. smoke-fpu is the correctness gate; the perf number
  is real-hardware (not CI-assertable on TCG). No new gate.
- **SFS unlink+rmdir are DONE (DDR-741, 83 gates):** removal tombstones the DIR
  entry (inode_num==0) since the B+tree has no delete and bt_insert replaces;
  lookup/dir_walk/create honor tombstones (re-creatable, readdir-invisible);
  sfs_unlink does files + empty dirs via the existing vfs_unlink op. Block
  reclamation deferred (bounded leak until reformat). Gate smoke-sfs-unlink.
- **SYS_GETDENTS is DONE (DDR-742, 83 gates):** ring-3 directory listing (NSI 66:
  path,index,name_buf -> namelen|0|-errno), resolves against the caller's
  root_mnt+fs_cap (per-process-root aware). PRISM `ls [dir]` now works; gate is
  smoke-shell extended with `ls /` asserting `^HELLO.TXT$` (bare name, distinct
  from the kernel's boot fs_list). `ps` still a stub (needs a process-table
  syscall). Remaining wall: wlroots/Wayland (out-of-tree).
- **Shipped since `199a637` (each CI-green, DDR/ADR before code):**
  - **DDR-711** window close+resize (`SYS_SURFACE_CLOSE/RESIZE` 59/60, `smoke-winops`).
  - **DDR-712** glass panels + particle field (`blend_px`, `smoke-visual`).
  - **DDR-713** agent-card click → `SYS_SPAWN_AGENT` (`smoke-agent-click`).
    **Root-cause kernel fix:** the AETHER spawn hook is registered BEFORE the
    first user process (boot race — user threads run while kmain boots).
  - **DDR-714 stage A** LAPIC + APIC timer own the 100 Hz tick on vector 48,
    PIT masked, 8259 kept for device IRQs (`kernel/apic/lapic.c`, `smoke-apic`).
  - **DDR-714 stage B / ADR-029** SMP bring-up: INIT-SIPI trampoline at 0x8000
    (`arch/x86_64/ap_boot.asm`), APs online **parked**, spinlock primitive
    (`kernel/include/spinlock.h`), `QEMU_SMP` runner knob (`smoke-smp` -smp 4).
  - **DDR-715** window titles + close button (`SYS_SURFACE_SET_TITLE` 61,
    `smoke-wmclose`); **DDR-716** ambiance backdrops (DAY mesh / DUSK sun-bloom /
    NIGHT nebulas, settled-frame guard, `smoke-backdrop`); **DDR-717** minimize
    + `r` restore (compositor-local, `smoke-wmmin`); **DDR-718** surface event
    channel (`SENDEV/GETEV` 62/63, corner drag-resize, owner-honored,
    `smoke-evresize`); **DDR-719** maximize + geometry restore (+ kernel
    `SURF_POS_KEEP` commit sentinel, `smoke-wmmax`).
  - CI robustness: `smoke-agents` TIMEOUT_S 90→150 (flaked twice on GitHub).
- **NSI now extends through 63.** Window management is feature-complete
  (title/drag/min/max/resize/close). The remaining big tracks: **distributed SMP
  scheduling** (per-CPU + subsystem locking, its own ADR superseding ADR-016),
  **I/O APIC + MSI-X** (DDR-714 stage C), **real glass blur/gradients**.
- Gate wall-clock: ~70 min on CI; locally run the batches from
  `docs/build_status.md`'s gate list (background tasks cap at 10 min — split
  into <10-min chunks, and kill orphaned WSL qemu/make between failed runs).

### 0.1 OLDER STATE (2026-06-28) + resume plan — historical

- **HEAD:** `5e5fb78` (`main` == `dev/phase1`, both pushed; this worktree branch is
  `claude/pedantic-shirley-a27bf3` — see §7). Since the old 5d/5e state (`9f310da`)
  the following shipped, **each CI-green**: **NET-B** (lwIP TCP/IP over virtio-net),
  **Layer 6 AETHER** (agent queue/audit/mem/rate + daemon + agent template),
  **ring-3 socket NSI** (ADR-027, live Ollama path), and **Layer 7** so far —
  mode binding (DDR-701), VirtIO-GPU framebuffer (ADR-028, slice 0), ring-3
  framebuffer surface (DDR-702), and PS/2 keyboard input (DDR-703).
- **CI: all 35 gates green at HEAD `5e5fb78`.** New since 5d/5e: `smoke-net`,
  `smoke-net-lo`, `smoke-net-fuzz`, `smoke-aether`, `smoke-aether-queue`,
  `smoke-aether-sec`, `smoke-mode`, `smoke-gpu`, `smoke-fb`, `smoke-input`
  (+ the dev-only `smoke-agent-live`, not in CI). NSI now extends through 46
  (`SYS_INPUT_POLL`); see `kernel/syscall/syscall.h`.
- **Build distro:** **Ubuntu-24.04** (NOT 22.04 — 22.04 is gone; `sudo` needs a
  password now: the WSL password is the user's to supply).
- **Toolchain:** `llvm-objcopy` symlinked to `llvm-objcopy-18`; `clang-18`/
  `ld.lld-18`/qemu/mkfs.fat/mcopy/mkfs.ext4/nasm/rustup/**python3** present
  (python3 is used by the `smoke-input` keyboard gate).
- **Git note:** the worktree gitdir is a Windows path, so **git runs from native
  Windows (PowerShell)**, not WSL (WSL git errors on the path). Build/test in WSL.
- **Stored remote PAT in `.git/config` is EXPIRED** — push with a fresh token
  inline in the URL (never write it to a tracked file).
- **Boot-memory layout note:** the boot page tables now live at **0x300000** (moved
  off 0x70000 once the kernel image+BSS grew); Stage 2 loads **16×64 sectors
  (512 KiB)**; the 1 MiB disk image accommodates it.

**Completed (PROC-D — musl libc — is COMPLETE):**
- **Layer 5b + IMP/PROC/NET series** were already complete on entry (the old
  handoff was stale — claimed HEAD `4608e9b`/"5b not started"). Reconciled.
- **PROC-D Step 1** (`f2bd207`): `SYS_SET_TLS`=27 (FS-base thread pointer,
  user-range-validated, restored on switch-in from `tcb.fs_base`, fork-inherited),
  `SYS_WRITEV`=28 (iovec gather-write via the validated copyin path; shared
  `fd_write_user`), `EXEC_MAX` 8 KiB→**256 KiB** (PMM-pool buffer). Probe
  `user/tlstest.asm` (`PRADYOS_TLS_OK WRITEV_OK`).
- **PROC-D Steps 2+3** (`8dd2162`, `0cfd957`): musl **v1.2.5** (`third_party/musl`,
  commit `0784374d`) builds to `build/musl/lib/{libc.a,crt1.o}` via `make musl` /
  `tools/build_musl.sh` + `third_party/musl-overlay/`. `user/cmusl.c` is the first
  ring-3 C program — links static against the subset with `user/user_c.ld`, runs
  from SFS, prints `PRADYOS_MUSL_OK v1.2.5 2026` via `printf`→`SYS_WRITEV`.
  `cpu_enable_sse()` enables x87/SSE for the varargs ABI. Design + the hard-won
  details are in **ADR-023** (§D1–D8).

**musl usage for 5d/5e (how to build a ring-3 C program):**
- Compile: `clang --target=x86_64-elf -ffreestanding -fno-pic -fno-pie
  -mcmodel=large -nostdinc -nostdlib -Wall -Wextra -Werror
  -Ibuild/musl/include -Ithird_party/musl/arch/x86_64
  -Ithird_party/musl/arch/generic -Ithird_party/musl/include -c foo.c -o foo.o`
  (**`-mcmodel=large` is mandatory** — the 0x8000000000 base exceeds 32-bit relocs).
- Link: `ld.lld -nostdlib -static -no-pie -T user/user_c.ld
  build/musl/lib/crt1.o foo.o build/musl/lib/libc.a -o foo.elf`.
- Embed like `cmusl` (incbin in `arch/x86_64/user_image.asm` →
  `user_boot_from_sfs` in `kernel/main.c`), or place on FAT/SFS for execve.
- If a libc call hits an undefined symbol at link, add its musl source file to the
  `SRCS` list in `tools/build_musl.sh` (the subset is link-resolved, not exhaustive).
- A new syscall musl needs that the NSI lacks: add it to the NSI (append-only) and
  remap it in `third_party/musl-overlay/syscall_overrides.h`.

**⚠️ BEFORE 5e (or any two concurrent ring-3 C/SSE processes):** add per-thread
`FXSAVE`/`FXRSTOR` to the context switch (512-byte 16-aligned area in the TCB,
saved/restored in `schedule()` like `fs_base`). Today FPU/XMM state is NOT saved
across switches — correct only while one thread uses the FPU at a time (**ADR-023
§D8**, binding trigger). 5d (single PID-1 process) is still safe.

**Next build order:** **5d pradyos-init (PID 1 + orphan reaper)** → 5e PRISM shell
→ NET-B lwIP → Layer 6 AETHER. ADR/DDR before each slice's code.

---

## 1. PROJECT IDENTITY

- **Name:** PRADYOS — Sovereign Edition (kernel: **NEXUS**).
- **Purpose / one-liner:** a from-scratch, bare-metal, AI-native operating system
  with an original x86_64 kernel; built in strict layer/slice order — *a slice
  ships when it is correct, not when it is fast.*
- **Active branch:** `dev/phase1` (fast-forwarded into `main` per slice).
- **Current HEAD:** `5e5fb78` (`main` == `dev/phase1`). **§0.1 is authoritative for
  current state.** The §2 layer-by-layer list below predates the NET-B / Layer 6 /
  Layer 7 work and is kept only for the L1–L5 history; see §0.1 + `docs/build_status.md`
  for everything after PROC-D (NET-B, AETHER, socket NSI, GPU FB, FB surface, input).
- **Repo path (this machine):**
  - Windows: `C:\Users\prady\Documents\Claude\Projects\Prady4OS`
  - WSL: `/mnt/c/Users/prady/Documents/Claude/Projects/Prady4OS`
- **Remote:** `https://github.com/prady4the4bady/Prady4OS.git` (a PAT lives only in
  the local `.git/config`; **never** write it to a tracked file. On a new account,
  configure your own credentials; do not reuse a leaked token.)

---

## 2. CURRENT STATE — LAYER BY LAYER

### ✅ Complete & CI-verified
- **L1 Boot** — MBR two-stage loader (`boot/mbr`, `boot/stage2`) → long mode → ring-0 C. Gate: `smoke`.
- **L2 NEXUS kernel core** — GDT/IDT + CPU-exception panic path; 8259 PIC + 8254 PIT @100 Hz; buddy PMM (ADR-003); SLAB heap; higher-half VMM (ADR-007); round-robin preemptive scheduler (ADR-008); NCS capability system (ADR-009); NIA IPC sync/async/broadcast (ADR-010/011); syscalls + ring 3 via SYSCALL/SYSRET + TSS (ADR-012); preemption-safe shared state (ADR-016). Gate: `smoke`.
- **L3 Drivers** — ACPI + PCIe ECAM (ADR-013); reusable virtio 1.0 transport + generic block layer + interrupt-driven virtio-blk (ADR-014); CMOS RTC (ADR-020). Gates: covered by FS gates.
- **L4 VFS + filesystems** — mount table + per-mount context (ADR-015/017); **FAT32 read-write** + VFAT long-name read + RTC timestamps (ADR-015/020); **SFS** inode CoW B+tree, file extents, journal + atomic tx + crash replay, snapshots, per-extent LZ4, 4 KiB tags (ADR-018); **ext4 read-only** (ADR-019). Gates: `smoke-fs`, `smoke-fs-rw`, `smoke-fs-sfs-rw`, `smoke-fs-ext4`.
- **L5a Userspace — static ELF loader + W^X (ADR-021)** — per-process address spaces + per-process CR3; `EFER.NXE` (CPUID-gated); ELF64 loader maps `PT_LOAD` with `p_flags`→W^X perms; 8 MiB RW+NX stack + guard page; SysV `argc/argv/envp/auxv`; ring-3 entry; user fault → clean kill. Test ELF written to SFS and **loaded back from SFS**. Gate: `smoke-user`.

CI status (this §2 list covers L1–L5a only — the historical baseline). **Current
CI is 35 gates green at HEAD `5e5fb78`** (see §0.1 for the full list); the L1–L5a
gates named here remain part of that set, plus the NET-B / Layer 6 / Layer 7 gates.
**Re-verify the current run yourself before trusting this** (see §9).

### 🟡 In progress / partial
- **L5 Userspace** — only slice **5a** done. Slices 5b (syscalls), 5c (musl),
  5d (pradyos-init PID 1), 5e (PRISM shell), 5f (prad) are **pending**. Next = 5b.

### ⏸ Deferred (tracked, with governing ADR)
- **Kernel-self W^X** (kernel text RX / kernel data NX) — the bootloader maps the
  kernel image RWX; only *user* W^X is enforced today. Governed by **ADR-021**;
  build before running untrusted code in kernel space (kernel-hardening pass).
- **COW fork** — interim will be copy-all-pages (5b); COW later. Record in **DDR-5b**.
- **Process reaping / AS teardown on exit** — `vmm_destroy_address_space` exists and
  is used on load-failure paths, but an exited user process leaks its AS + kstack
  until a reaper. Build with the PID-1 orphan reaper (slice **5d**).
- **Dynamic linking** (`ld-pradyos.so`) — static ELF only for now.
- **APIC / SMP** — legacy PIC+PIT only (**ADR-006**).
- **3-lane NAS scheduler** — round-robin placeholder (**ADR-008**).
- **PMM variable-weight / predictive** — buddy interim (**ADR-003**, OPEN).
- **ext4 write**, **SFS free-space tree / snapshot GC** — read/feature scope set by **ADR-018/019**.
- Full deferred list with "build-before" triggers: `docs/build_status.md` → DEFERRED.

---

## 3. ARCHITECTURE CONTRACTS THAT MUST NEVER BE BROKEN

These are **binding**. ADR-021 is a binding security ADR and may only be changed by
a new superseding ADR — never quietly amended.

1. **W^X invariant (ADR-021).** No page is ever simultaneously writable and
   executable, in any address space, anywhere in Layer 5.
   - text/`PF_X` → present, USER, **RX** (no W, no NX).
   - rodata/`PF_R` → present, USER, **R + NX**.
   - data/BSS/`PF_W`, heap, user stack → present, USER, **RW + NX**.
   - guard page → **not present** (PTE = 0), placed immediately below the stack.
   - A segment that is both `PF_W` and `PF_X` is **rejected** (`ELF_E_WX`), never mapped RWX.
   - Enforced in `kernel/exec/elf.c` (flag derivation) + `kernel/mm/vmm.c`
     (`map_core` writes PTE bit 63 = NX). **What breaks it:** mapping any user page
     with both `VMM_RW` and without `VMM_NX`; reusing the deleted RWX `user_demo`;
     dropping the W+X rejection. Negative regression `user/wxviol.asm` must keep
     producing `#PF err=0x7 → clean kill`.
2. **NX gating.** `EFER.NXE` is enabled **only if** `CPUID.8000_0001h:EDX[20]`
   reports NX (`g_nx_ok` in `kernel/mm/vmm.c`). When `g_nx_ok` is false, `map_core`
   must **not** set PTE bit 63 (setting it with NXE clear faults). Never force NXE
   unconditionally.
3. **User-fault isolation (ADR-021).** A fault from **CPL 3** (`isr_dispatch`,
   `kernel/idt.c`) must become a clean process kill via `sched_exit` with a
   `[trap] user …` line — **never** a kernel panic. CPL-0 faults still panic.
4. **Root-cause-only fixes.** No patchwork, no warning suppression, no masking
   symptoms. If a design issue appears, write/adjust an ADR or DDR first.
5. **`-Werror` must stay green at all times** — clang **and** nasm
   (`NASM_WERROR := -Werror`). Any warning is a build failure.
6. **Repo stays clean & organized.** No `TODO`/`FIXME`, no dead code/refs, no new
   flat files in `kernel/` root (use subsystem subdirs). `docs/build_status.md` is
   updated in the **same commit** as the code it describes; keep
   `docs/platform_profiles.md` accurate too.

---

## 4. PHASE C — LAYER 5b EXACT PLAN

Build order is **mandatory**; do not skip/reorder without a DDR. Each slice:
runs `graph_session_primer()` + `graph_deps()` before editing; writes its
ADR/DDR **before** code; adds `smoke-user` coverage; keeps `-Werror` clean;
updates `docs/build_status.md` in the same commit; and must pass its gate before
the next slice starts.

1. **DDR-5b (design, written FIRST)** — the syscall-expansion plan; the validated
   user-pointer contract (`copyin` / `copyout` / `copyinstr`, **EFAULT, never
   panic**, all user pointers validated against the process AS); and the explicit
   **fork = copy-all-pages now, COW deferred** decision.
2. **`copyin` / `copyout` primitives** + a bad-pointer / guard-page test
   (→ `EFAULT`, kernel survives). Comes after DDR-5b.
3. **`read` / `write`** (using the validated copy path).
4. **`open` / `close`.**
5. **`lseek` / `fstat` / `getcwd`.**
6. **`mmap` baseline** (MAP_ANON; W^X-respecting flags).
7. **`execve`** (replace the AS; reuse the ELF loader).
8. **`wait4`.**
9. **Signal groundwork** (+ the fork decision record if true COW is deferred).

**Gate report format (print after each slice gate, exactly):**
```
Phase 5b — gate report
<commit> | <gate> | PASS/FAIL | next: <next slice>
```

---

## 5. GRAPH TOOLING (use it FIRST, every session)

- **Location:** `tools/graph_mcp/` — pure JS/WASM code knowledge graph (MCP + CLI).
  Full usage: `tools/graph_mcp/CLAUDE_GRAPH_USAGE.md`.
- **MCP server:** registered in `/.mcp.json` (`pradyos-graph`). The client prompts
  once to trust it. Manual start: `node tools/graph_mcp/server.js mcp`.
- **Fresh clone / first run:** `bash tools/graph_mcp/setup_graph.sh`
  (ensures Node ≥18, `npm ci`, builds + validates the graph). On Windows:
  `cd tools/graph_mcp && npm ci && node server.js rebuild`.
- **MANDATORY workflow:**
  - **`graph_session_primer()` before opening ANY source file** (orientation).
  - **`graph_deps("<file>")` before editing any file** (includes / included-by / users).
  - **`graph_blast_radius("<file>")` before any refactor or signature change.**
  - `graph_query("…")` / `graph_files("<subsystem>")` to locate code instead of blind reads.
  - `graph_callchain("<fn>")` before changing a function's contract.
  - **`graph_rebuild()` after structural changes** (new/renamed/moved files, new functions).
- **CLI fallback** (if MCP not connected):
  `node tools/graph_mcp/server.js {primer|query <t>|files <ss>|deps <f>|callchain <fn>|blast <f>|rebuild|selftest}`.

---

## 6. CI DETAILS

- **Workflow:** `.github/workflows/ci.yml`. Runs on push to `main` / `dev/**` and PRs.
- **Job `build-and-boot`** — must stay green:
  `toolchain-check` → `image` (`-Werror`) → `smoke` → `smoke-fs` → `smoke-fs-rw`
  → `smoke-fs-sfs-rw` → `smoke-fs-ext4` → `smoke-user`.
- **Job `code-graph`** (isolated, non-destructive) — `npm ci` then
  `node server.js selftest`: builds the graph from source and validates the 7
  query tools (writes only the git-ignored `.graph/` DB; cannot make kernel gates flaky).
- **Local full gate set (run before committing):**
  ```
  wsl -d Ubuntu-24.04 -- bash -lc 'cd /mnt/c/Users/prady/Documents/Claude/Projects/Prady4OS \
    && source "$HOME/.cargo/env" && make toolchain-check && make image && make smoke \
    && make smoke-fs && make smoke-fs-rw && make smoke-fs-sfs-rw && make smoke-fs-ext4 && make smoke-user'
  ```
- **Verify CI from the host:** `gh run list/watch --repo prady4the4bady/Prady4OS`
  (use the Windows `gh`; it is not on the WSL PATH).

---

## 7. TOOLCHAIN NOTES

- **Builds run in WSL**, not native Windows:
  `wsl -d Ubuntu-24.04 -- bash -lc 'cd <repo> && source "$HOME/.cargo/env" && make <target>'`.
  Cross toolchain (ADR-001): clang + `ld.lld` + `llvm-objcopy` + nasm; Rust nightly
  `x86_64-unknown-none`; QEMU q35.
- **WSL has no Linux `node`** — only the Windows `node`/`npm` leak in via PATH
  interop. So the graph tooling uses a **pure JS/WASM stack** (`sql.js` +
  `web-tree-sitter@0.22.6` + `tree-sitter-wasms`): one `node_modules` works on the
  Windows host, WSL, **and** Linux CI — no `node-gyp`, no native prebuilds, no CI
  flakiness. `setup_graph.sh` installs a Linux Node (≥18) in WSL when missing.
- **Shell-quoting gotcha (cost real time):** inline shell variables and loops
  inside `wsl … bash -lc '…'` from the agent's Bash tool **expand to empty**
  (MSYS2/Git-Bash argv mangling) — e.g. `L=foo; … "$L"` → empty, `for g in …; do
  make $g; done` → no target. `$HOME`, `$(…)`, and `<<'EOF'` heredocs survive. Use
  **literal commands / fixed paths / heredocs**, or put loops in a Makefile target.
- **WSL git identity** is per-repo (set this session): user.name
  `pradyun kumar sinha`, user.email `pradyun4kumar4sinha4@icloud.com`. A new
  account should set its own.
- **NASM** is parsed by a focused extractor (not Tree-sitter) — ASM grammars lack
  reliable prebuilt WASM; this is intentional and documented.

---

## 8. FILE OWNERSHIP MAP

| Path | Layer / role | Read ADR before editing |
|------|--------------|--------------------------|
| `boot/mbr/`, `boot/stage2/` | L1 bootloader | ADR-005 |
| `arch/x86_64/*.asm` | L2 asm (boot, cpu, isr, context, syscall_entry, usermode) | ADR-005/012 |
| `arch/x86_64/user_image.asm` | L5 — embeds `build/hello.elf` + `build/wxviol.elf` | ADR-021 |
| `kernel/mm/pmm.*`, `kheap.*` | L2 memory | ADR-003 |
| `kernel/mm/vmm.*` | L2 VMM + **per-process AS / NX** | **ADR-007 + ADR-021 (binding)** |
| `kernel/proc/sched.*`, `tss.*` | L2 scheduler + per-process CR3 | ADR-008 (+ ADR-021 for cr3) |
| `kernel/cap.*` | L2 capabilities | ADR-009 |
| `kernel/ipc/*` | L2 IPC + broadcast | ADR-010/011 |
| `kernel/syscall/*` | L2 syscalls (5b extends this) | ADR-012 |
| `kernel/idt.c` | L2 exception dispatch + **user-fault kill path** | ADR-021 |
| `kernel/exec/elf.*` | **L5 ELF loader (binding security)** | **ADR-021** |
| `kernel/acpi/*`, `kernel/drivers/*` | L3 drivers | ADR-013/014/020 |
| `kernel/fs/vfs/*` | L4 VFS + mounts | ADR-015/017 |
| `kernel/fs/fat32/*` | L4 FAT32 RW + LFN | ADR-015/020 |
| `kernel/fs/sfs/*` | L4 SFS engine | **ADR-018** |
| `kernel/fs/ext4/*` | L4 ext4 read-only | ADR-019 |
| `kernel/main.c` | boot orchestration + in-kernel self-tests | — |
| `user/hello.asm`, `user/wxviol.asm`, `user/user.ld` | L5 ring-3 test programs | ADR-021 |
| `tools/graph_mcp/**` | tooling (not a kernel layer) | `CLAUDE_GRAPH_USAGE.md` |
| `docs/decisions/ADR-*.md` | architecture decisions | — |
| `docs/build_status.md`, `docs/platform_profiles.md` | status (update with code) | — |

**Generated — never commit (git-ignored):** `build/`, `tools/graph_mcp/.graph/`,
`tools/graph_mcp/node_modules/`, `*.o`/`*.elf`/`*.bin`/`*.img`, `target/`.
The graph DB and `node_modules` are reproduced via `npm ci` + `rebuild`.

---

## 9. WHAT NOT TO DO

- ❌ **Do not weaken W^X or NX to make a test pass.** Fix the test or the real bug.
- ❌ **Do not commit generated artifacts** (`build/`, `.graph/`, `node_modules/`, images, objects).
- ❌ **Do not patchwork.** Root-cause fixes only; ADR/DDR before governed code.
- ❌ **Do not claim CI is green without verifying the current run** (`gh run watch`).
- ❌ **Do not proceed past a failing gate** — stop and fix the root cause first.
- ❌ **Do not skip `graph_session_primer()` at session start**, or `graph_deps()`
  before editing, or `graph_blast_radius()` before a refactor.
- ❌ **Do not start slice N+1** until slice N boots clean and passes its gate.
- ❌ **Do not invent ISA/register details** — cite Intel/AMD SDM or say "I don't know".
- ❌ **Do not write the PAT/secrets into any tracked file.**

---

*Generated at the end of the session that completed Layer 5a (ADR-021) and added
the `tools/graph_mcp` code knowledge graph. HEAD `4608e9b`. Next: Phase C, Slice 1
— DDR-5b.*

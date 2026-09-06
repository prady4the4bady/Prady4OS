# DDR-1066 — the AETHER agent never executed anything, and the gate's execute arm was the string it printed

**Status:** DESIGN (§NON-NEGOTIABLE 5 — written before the code).
**Class:** dead arm — thirteenth-plus instance, and the first found in the
**product** (the one shipped agent) rather than in a gate or an instrument.
**Baseline:** `kernel.bin` `a9d8bc933595ec0d`, 1,282,442 B.

---

## 1. THE FINDING

`smoke-aether` is the AETHER end-to-end gate. Its own Makefile comment states the
claim:

> the daemon (CAP_SOVEREIGN) boots, spawns the test agent (CAP_AGENT); the agent
> submits `ACTION_WRITE_FILE` which sovereign mode auto-approves; **the agent
> executes it** and exits. End-to-end: queue -> daemon -> agent -> approve ->
> **execute** -> done.

**The agent does not execute it.** `user/agent_base.c:182`:

```c
    if (st == AE_APPROVED)
        printf("AETHER_AGENT_EXEC WRITE_FILE %s\n%s\n", path, data);
```

That is a *narration*, not a write. `data` is `"PRADYOS_AGENT_VERIFIED"` — and
`PRADYOS_AGENT_VERIFIED` is one of the four sentinels `smoke-aether` requires:

```make
smoke-aether: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 EXTRA_SENTINEL="$$(printf 'PRADYOS_AETHER_QUEUE_OK\nPRADYOS_AETHER_DAEMON_OK\nPRADYOS_AGENT_VERIFIED\nPRADYOS_AGENT_DONE')" \
```

**So the gate's execute arm asserts on a string literal the agent prints from
`.rodata` on approval.** It cannot fail for the reason it exists. An agent that
performs no action at all passes it, which is exactly what ships.

### 1.1 Measured, not read

- `grep -n "SYS_OPEN\|SYS_WRITE\|SYS_CREAT" user/agent_base.c` returns **nothing**.
  The file contains **zero** filesystem syscalls, in *either* branch.
- `AETHER_TEST_MODE` defaults to **1** (`agent_base.c:17-19`), and the live branch
  is the Ollama path, excluded from CI (`smoke-agent-live` is developer-run only,
  checklist §5.4). **The shipped and CI-exercised configuration is the `#else`
  branch** — the one quoted above.
- The `#else` branch's own comment reads *"a fixed model response ->
  ACTION_WRITE_FILE, submitted **and executed**"*. Three separate comments assert
  an execution that does not occur.
- `grep -rn "AETHER_AGENT_EXEC" Makefile tools/` returns **nothing** — no gate
  asserts on that line, so it is not itself a vacuous assertion today. **What is
  asserted is `PRADYOS_AGENT_VERIFIED`**, which the same `printf` emits.

### 1.2 This is NOT the architecture working as designed

DDR-1013 §2 recorded the correct shape: *the kernel is the policy engine and the
AGENT executes after approval* — there is no kernel executor for
`ACTION_WRITE_FILE` and there should not be. The 3C probes implement that shape
faithfully; `user/actionreadtest.c:101` is the model, and says so:

```c
    /* 3. EXECUTE, and only now. */
    long fd = nsi(SYS_OPEN, (long)path, O_RDONLY, 0);
```

…followed by *"4. VERIFY THE BYTES. This is what makes the gate non-vacuous: a
probe that skipped step 3 still prints an OK line, but cannot print the
content."* **The discipline was already written down, in this repository, for
this exact hazard — and the one real agent does not follow it.**

DDR-1022 established that `user/agent_base.c` is **the only agent program**; the
roster is generic and a slot is filled by `SYS_SPAWN_AGENT` launching this
template. So "the AETHER agent executes approved actions" is a claim resting
entirely on this file.

### 1.3 It is not blocked — it was never wired

Both preconditions hold and were checked in the tree rather than assumed:

- **Capability.** `kernel/exec/elf.c:320` gives every `elf_load`ed process
  `fs_cap = cap_create(t->caps, RES_FILE, FS_RES_ID, CAP_FS_READ | CAP_FS_WRITE)`.
  The agent is loaded by `elf_load` from `aether_spawn_agent_hook`
  (`main.c:1137`), so it **holds `CAP_FS_WRITE`**.
- **Root mount.** `elf.c:321` sets `root_mnt = vfs_default_mnt()`, which
  `main.c:1293` points at the FAT32 volume. `sys_open` uses `t->fs_cap` and
  `t->root_mnt` (`sys_file.c:45-48`), and `fat32_write` exists
  (`kernel/fs/fat32/fat32.c:529`, wired at `:732`).

So an approved `ACTION_WRITE_FILE` could always have been executed. Nothing
blocked it.

**And this is confirmed by measurement, not only by reading the source:** the
fixed agent's round trip *succeeds* — `n=22 first=P last=D` — which is a stronger
statement than the source reading, because a capability or mount that was in fact
wrong would have produced `-EPERM` or `-ENOENT` on the write instead.

### 1.4 And the path would have failed anyway

`agent_base.c:157` writes to `/tmp/aether_test.txt`. The FAT volume's only
directory is `::/DOCS` — `grep -n "mmd" Makefile` shows `mmd -i $(FAT_IMG)
::/DOCS` and nothing else, and `/tmp` appears nowhere else in the tree. **There is
no `/tmp`**, so even a correct implementation of this exact path would have
failed with `-ENOENT`. Recorded because it is the trap waiting for anyone who
"just adds the write": the path has to move to the root.

---

## 2. WHY THIS MATTERS MORE THAN A MISSING FEATURE

A gate that cannot fail is worse than no gate: it consumes a shard slot, it
appears in the count, and it licenses a sentence — *"end-to-end: queue -> daemon
-> agent -> approve -> execute -> done"* — that the system does not support. The
project has hit this twelve-plus times in gates and instruments. **This instance
is in the product**, and the consequence is a capability claim: AETHER is
described as an agent layer whose approved actions take effect, and today an
approved action takes no effect whatsoever.

---

## 3. DESIGN

### 3.1 The agent executes, then reads back

On `AE_APPROVED`, `agent_base.c` performs the action it submitted:

1. `SYS_OPEN(path, O_WRONLY|O_CREAT)` → `SYS_WRITE(data)` → `SYS_CLOSE`.
2. **Reopen with no `O_CREAT`** and `SYS_READ` into a buffer.
3. Print the marker line **from the read-back buffer**, never from `data`.

Step 2's *absence* of `O_CREAT` is load-bearing: `vfs_open` on a missing file
returns `-ENOENT`, so a build that skipped step 1 cannot reach step 3.

### 3.2 The existing sentinel becomes live rather than being replaced

`PRADYOS_AGENT_VERIFIED` is already in `smoke-aether`'s `EXTRA_SENTINEL` list.
After this change it is emitted **only** from the read-back buffer, so the
sentinel the gate has always required starts meaning what the gate's comment
always said. **The gate file needs no edit for that arm to become live** — which
is the cleanest available proof that the arm was dead: the same assertion, the
same gate, and now it can fail.

A second, additive sentinel carries the quantities:

```
PRADYOS_AGENT_EXEC_OK path=/AETHER.TXT n=<bytes read> first=<c> last=<c>
```

`n=` catches a write that succeeds and stores the wrong length — presence of the
string alone cannot (DDR-1039's rule: assert both directions).

On any failure the agent prints an explicit error line and **does not print the
data**, so a failed round trip cannot be mistaken for a successful one.

### 3.3 Path

`/AETHER.TXT` — 8.3, on the FAT32 root, matching the `/HELLO.TXT` precedent.
`/tmp/aether_test.txt` is removed (§1.4).

### 3.4 The measured lines are built whole, and the claim is stated precisely

The two asserted lines are assembled in a `uline` buffer and handed to a single
`printf("%s", …)`, rather than built from a sequence of `printf`s.

**Being exact about what that buys, because `agent_base.c` is a musl program and
the obvious claim would be wrong.** `uline` alone does *not* make this "one
`write(2)` per line" here: stdout is **fully buffered** in this environment
(DDR-1056 measured why — `__stdout_write` falls back to `lbf=-1` because
`ioctl(TIOCGWINSZ)` fails and this kernel registers no `SYS_IOCTL`), so the line
lands in the stdio buffer and is emitted later by `fflush`. What `uline` does
guarantee is that the line is **contiguous in that buffer** — it can never be
interleaved with another line of this program's own output — and DDR-1056's
`FD_CONSOLE` gather in `sys_writev` makes the flush that carries it atomic
against every other printer in the system. Those two together are what a gate's
whole-line `grep` needs; neither alone is.

---

## 4. WHAT IS DELIBERATELY NOT DONE

- **No kernel executor.** DDR-1013's scope correction stands: the kernel approves,
  the agent acts. This change makes the agent act; it does not move execution
  into ring 0.
- **`ACTION_SEND_IPC` is still not wired** (checklist §4.1). That row is correct
  about SEND_IPC and **too narrow about the cause** — it reads as if SEND_IPC were
  the exception, when in fact the agent executed *nothing*. §4.1 is corrected
  rather than closed: after this change `ACTION_WRITE_FILE` executes and SEND_IPC
  still does not, which is the first time that row's framing is accurate.
- **The live (Ollama) branch is unchanged.** It is excluded from CI and cannot be
  exercised here, so touching it would ship untested code.

---

## 5. MEASURED

All four boots are `make smoke-aether`, one at a time, `pgrep -f
"[q]emu-system-x86_64"` clear before each (§NON-NEGOTIABLE 12).

| tree | `kernel.bin` | rc | what the capture says |
|---|---|---|---|
| **baseline (pre-fix)** | `a9d8bc933595ec0d` | **0 — PASS** | `AETHER_AGENT_EXEC WRITE_FILE /tmp/aether_test.txt` / `PRADYOS_AGENT_VERIFIED` — **zero filesystem calls made** |
| **fixed** | `dde6c5d10748842d` | **0 — PASS** | `PRADYOS_AGENT_EXEC_OK path=/AETHER.TXT n=22 first=P last=D` |
| **M1** (no write, print from the read-back buffer) | `248afcf994645ab5` | **2 — FAIL** | `AETHER_AGENT_EXEC_FAIL step=open_r rc=-2` and `[smoke] FAIL — required pattern 'PRADYOS_AGENT_VERIFIED' not found` |
| **M2** (no write, print the LITERAL — the pre-fix behaviour) | `46aaf0304f395b6f` | **0 — PASS** | the gate is green on an agent that touched nothing |

Reverting the mutants returns `dde6c5d10748842d` **bit-for-bit**, 1,286,538 B
(headroom 286,326 B).

### 5.1 M1 and M2 are the whole argument, and they differ in one thing

**M1 and M2 do exactly the same amount of filesystem work: none.** The only
difference between them is *where the printed bytes come from* — the read-back
buffer or the `.rodata` literal — and that difference alone decides whether the
gate can see it. The dead arm is demonstrated rather than argued: the same
sentinel, the same gate, the same absent write, opposite verdicts.

The baseline row is M2's independent confirmation: M2 is a *reconstruction* of
the pre-fix behaviour, and the baseline is the pre-fix tree itself, both green.

### 5.2 A limit of the new sentinel, found by M2 and not predicted

§3.2 introduced `PRADYOS_AGENT_EXEC_OK … n= first= last=` as if it added
discriminating power. **M2 prints it too, with the correct values** — an agent
holding the data can compute `n`, `first` and `last` without touching a
filesystem, so the *new* sentinel does not convict either. What convicts is the
**read-back open with no `O_CREAT`**: `-ENOENT` is a value the agent cannot
manufacture, and every failure path refuses to print `data`.

`n=` still earns its place, but for a narrower claim than §3.2 implied: it
catches a **real** write that stores the wrong length. Recorded as a corrected
design claim rather than left reading as more than it is.

### 5.3 `-Werror` caught a mutant dropping an error path

M2's first build failed: `error: unused function 'agent_exec_fail'
[-Werror,-Wunused-function]`. That is a small but real property worth naming — a
future change that quietly deletes every failure path from this executor **will
not compile**, so the round trip cannot become unchecked by attrition. M2 had to
reference the helper explicitly to reproduce the defect.

## 6. THE GATE

`smoke-aether` gains two things and keeps its four existing sentinels:

- `PRADYOS_AGENT_EXEC_OK path=/AETHER.TXT n=22 first=P last=D` as an **exact**
  required pattern — the DDR-1044 discipline (an exact value, not a shape), so a
  write that stores the wrong number of bytes fails rather than matching a
  loose pattern.
- `AETHER_AGENT_EXEC_FAIL` as a `FORBIDDEN_SENTINEL`, so a failed execution
  **names itself** instead of only being detectable by an absence.

No new gate is created. The whole point is that `smoke-aether` — the gate that
already claimed this — becomes able to fail for the reason its comment states.

## 7. NOT CLAIMED

- **`ACTION_SEND_IPC` is still not wired.** Checklist §4.1 is corrected, not
  closed: after this change `ACTION_WRITE_FILE` executes and SEND_IPC does not,
  which is the first time that row's framing is accurate.
- **No kernel defect is fixed.** The kernel's policy engine, capability check and
  FAT32 writer were all correct and complete; what was missing was the agent
  calling them. `vfs_write`, `cap_ok(CAP_FS_WRITE)` and `fat32_write` are
  untouched.
- **No open issue moves.** OPEN-1, OPEN-2, OPEN-12, OPEN-13 are untouched.
- **The live (Ollama) branch is unchanged and unexercised** — excluded from CI by
  `smoke-agent-live`'s developer-run-only status (checklist §5.4), so a change
  there would ship untested.
- **This does not make the agent general.** It executes the one action type it
  submits. The other seven 3C types have per-type probes that execute correctly
  (DDR-1015..1020, 1033, 1034); wiring the *agent template* to dispatch on type
  is a larger change and is not attempted here.

---

## 8. A RED IN THE REGRESSION SUITE, READ AND NOT ATTRIBUTED

The regression suite ran 11 gates on `dde6c5d10748842d`, hash-verified before and
after (§DDR-1060 §9's pin). **Ten green; `smoke-blk-integrity` rc=2.**

```
kernel=dde6c5d10748842d
smoke-aether rc=0            smoke-shell rc=0
smoke-aether-queue rc=0      smoke-selftest rc=0
smoke-agents rc=0            smoke-fs rc=0
smoke-aether-sfsroot rc=0    smoke-blkmq rc=0
smoke-iso-userspace rc=0     smoke-rqstress-liveness rc=0
                             smoke-blk-integrity rc=2      <-- 
kernel_after=dde6c5d10748842d
```

### 8.1 The capture was read, and every detector is silent

`build/gatelogs/serial-29768.log.fail-29768`, 351 lines. Scanned for the whole
known set: **zero** `[apfreeze]`, **zero** `panic_stage=`, **zero**
`[percpu] gs FAIL`, **zero** `*** NEXUS KERNEL PANIC ***`, **zero** `resched
FAIL`, **zero** `[vblk] compl wait timeout`. Output ends **mid-line** at
`[svc] exit`.

### 8.2 It is a STOP, not a slowdown — measured against a passing capture

A passing run of the same gate on the same binary was captured for comparison:

| | PASS (`blkint-pass.log`) | RED (`serial-29768.log.fail-29768`) |
|---|---|---|
| lines | 466 | 351 |
| `[boot-load] PRISM.ELF` | **t=361** | **t=386** |
| last tick stamp anywhere | **t=28627** | **t=386** |

**The early boot is not slow** — both reach PRISM at essentially the same tick.
The passing boot then advances to t=28,627; the red one produces **no further
tick stamp at all** and stops emitting. That is a silent stop with every
detector quiet.

### 8.3 What was checked before drawing a conclusion, and what it ruled out

§INV.8 says a gate's timeout is a claim about timing, so elapsed was measured:
a **passing** boot of this gate consumes **180 s of its 180 s window**. That
looks like zero margin — and it is not. `smoke-blk-integrity` declares a
`FORBIDDEN_SENTINEL`, and per DDR-1043 `early_exit_eligible` is 0 for any such
gate, so **it always runs the full window by design** and elapsed time carries no
information about margin here. The "no margin" reading was formed, checked, and
**discarded** before it was written down as a finding.

### 8.4 NOT ATTRIBUTED, and specifically not exonerated

- **The binary is identical** before and after the suite, so this is not a
  build-side effect.
- The permitted **single re-run** on that identical binary came back **green**
  (rc=0), which separates "did not reproduce on re-run" from "reproduces".
- **It is not OPEN-2's signature** — no `[apfreeze]`, and DDR-1010's local
  reproduction on this same gate carried `[percpu] gs FAIL`, which is absent
  here. Per DDR-1019, a matching *shape* is not the same defect: OPEN-1 route 1
  is also a silent hang, but that is `smoke-surfdestroy`, and reading this as
  that would be colour-matching.
- **The changed code did not execute in the failing boot — MEASURED 2026-09-06,
  and this narrows the position below rather than replacing it.** The red
  capture contains **zero** `PRADYOS_AGENT_START`, while the passing capture of
  the same gate on the same binary reaches it at **line 413 of 466**:

  ```
  PASS  413: PRADYOS_AGENT_START task=test mode=test
        414: PRADYOS_AGENT_EXEC_OK path=/AETHER.TXT n=22 first=P last=D
        417: PRADYOS_AGENT_DONE
  RED   (351 lines, no AGENT_START anywhere)
  ```

  DDR-1066's diff is **entirely inside the post-`AE_APPROVED` path of
  `agent_base.c`**, which is reached only after that first line prints. So the
  boot stopped **before any of this change could run**, and the new FAT32 write
  never happened in that boot. That is a positive statement from the capture,
  not the "the diff is elsewhere" hand-wave DDR-1042 warns about.
- **Still not fully exonerated, and the residual is named.** `agent_base.elf` is
  embedded in the kernel image, so `kernel.bin` differs from the pre-DDR-1066
  build; a **layout or size** effect is not excluded by the above, only a
  **behavioural** one. What is now established is narrow and worth having: the
  changed code path is provably unreached in the failing run.
- **One occurrence is not a rate**, and no local campaign is run to make it one:
  DDR-1023 recorded that route as exhausted for this gate's family, and DDR-1061
  §2 costed what a reachable N actually buys.

**No fix, and none is permitted** — §NON-NEGOTIABLE 3 requires a named mechanism
and there is not one. Recorded in checklist §2 so the next occurrence has
somewhere to land and a measured comparison to be read against.

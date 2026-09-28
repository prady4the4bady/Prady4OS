# DDR-1085 — the configured agent task never reached the agent, and the Group F row's stated blocker was never the real one

**Status:** IMPLEMENTED + gated + M1/M2 on distinct hashes
**Date:** 2026-09-07
**Branch:** `dev/phase1-seyp3n`

---

## 1. The row I came for, and why it is not what it says

Group F carries:

> **Agent `execve`-on-respawn from SFS** — *needs FAT32 fix or SFS as agent root*

DDR-1072 §5 already corrected half of that: the FAT32 multi-cluster defect was
**REFUTED and gated** (DDR-973), so the named alternative has been available for
a long time. I came to build the row on that basis. **The row's blocker is still
not the real one, and the real one must not be removed.**

Measured, not reasoned:

* **A supervisor is already shipped.** DDR-891's service manager lives in
  `user/init.c`: a bounded static table, `fork`+`execve` start, exit attribution
  by pid, `RESTART_ON_FAILURE` with a budget of 3 and a loud give-up.
* **It already has an agent row**, `user/init.c:68`:

  ```c
  { "agentsvc", "/AGENT.ELF",    RESTART_NEVER,      CAP_AGENT, 0, 0, 0 },
  ```

* **And that row is refused before the fork, deliberately**, `svc_spawn`:

  ```c
  if (s->caps_required & ~(unsigned)INIT_CAPS) {
      printf("[svc] refuse %s: requires caps init does not hold\n", s->name);
  ```

  with `#define INIT_CAPS CAP_NONE` and the file's own statement of intent:
  *"init holds none of the privileged ones, so anything but `CAP_NONE` is
  refused before the fork. **init never grants; it only refuses.**"*

So the thing standing between this row and a working agent-from-disk is **not a
filesystem**. It is a capability refusal in the only supervisor that would run
it, and that refusal is a shipped security property with its own gate arm. A
session taking the row at its word — "needs the FAT32 fix" — would place
`/AGENT.ELF` on the FAT volume, find init still refusing, and the shortest path
from there is to widen `INIT_CAPS`. **That is the DDR-1068 §2 failure mode: a
row that reads as unbuilt work inviting the removal of a recorded refusal.**

This is the **third consecutive instance** of DDR-1084 §1's pattern and the
sharpest of the three, because the previous two named blockers that had *become*
false. This one names a blocker that **was never true**, while the true blocker
sits unmentioned in the source. No checker is built; the signal is semantic, the
wall DDR-1071 §5, DDR-1072 §2 and DDR-1081 §3 each hit independently.

**The row is CORRECTED, not closed.** No respawn is built here. `/AGENT.ELF` is
not placed. `INIT_CAPS` is not touched. The correct supervisor for an agent is
the **daemon** — `sys_spawn_agent` requires `is_sovereign || is_agent` and the
daemon is sovereign — and whether agents should be respawned at all is a policy
question with a fork-bomb hazard DDR-891 already analysed. Recorded, not taken.

---

## 2. What I found instead, and it is a defect

`smoke-aethercfg`'s own Makefile header (`Makefile:4253`) says:

> the daemon reads `/etc/aether/config` off the SFS root … and **applies
> mode/task/slot from it**

Two of those three arrive. **The task does not.** The whole path, read in the
tree:

| step | file | what happens to `task` |
|---|---|---|
| parse | `user/aether_daemon.c` | `cfg.task` filled from `task=` in the config |
| submit | `user/aether_daemon.c:165` | `nsi(SYS_SPAWN_AGENT, 0, (long)cfg.task, cfg.slot)` |
| syscall | `kernel/syscall/sys_aether.c:196` | `copyinstr(task, …, sizeof task, 0)` into `char task[64]` |
| hook | `kernel/main.c:1156` | **`(void)task;`** |

`aether_spawn_agent_hook` then calls `elf_load(agent_base_elf, …)`, which passes
`0` for `struct exec_args` — DDR-1032's documented "behave exactly as before"
path, `argc = 1`, `argv[0] = the name`.

So the kernel validates the string, copies it across the ring boundary into a
64-byte buffer, and **throws it away with a cast**. That is DDR-1032's own
shape, which DDR-877 called *"worse than incomplete"*: `sys_execve` read
`uargv`/`uenvp`, `(void)`-cast them, and `execve` with arguments succeeded while
delivering none. Here the call succeeds, the agent starts, the roster slot is
claimed, the action runs — and the one field that says *what the agent was asked
to do* is dropped between ring 3 and ring 3.

### 2.1 Why no gate could see it — and this is the part worth carrying

`user/agent_base.c:165-167`:

```c
const char *task = (argc > 2) ? argv[2] : "test";
printf("PRADYOS_AGENT_START task=%s mode=%s\n", task, …);
```

and `Makefile:2258`:

```make
AETHER_CFG_TEXT := mode=sovereign\ntask=test\nslot=0\nnet=10.0.2.2:11434\n
```

**The configured value is byte-identical to the compiled-in fallback.** Every
boot prints `PRADYOS_AGENT_START task=test` — the correct answer, produced
without the value ever leaving the daemon. An arm asserting that line passes on
a kernel with no plumbing whatsoever.

This is the vacuity class again, and the **seventh time it has been caught in
design text before the arm was written** (after DDR-1039 §3.1, 1058 §2, 1067 §2,
1070 §4, 1083 §4, 1084 §4) — but the first time the vacuity lives in the
**product's own shipped configuration** rather than in a test. Nothing in the
tree is wrong for `test` to be the config's value; what it costs is that the
config→kernel→agent wire is unobservable, and it was in fact unbuilt.

### 2.2 A second wire error underneath the first

`argc > 2 ? argv[2]` reads the task from **index 2**. DDR-1032b settled this
project's convention when it wired PRISM's `run`: *"`av[0]` the path (execv(3)
convention)"*, so a program's first argument is `argv[1]`. Index 2 is a claim
about a calling convention **no caller has ever implemented** — measured, the
hook is the only spawner of this program and it has always passed `NULL`.

It could not be wrong in any observable way while argv was never passed at all.
It becomes wrong the moment §3 passes one, which is why both halves land in the
same change.

---

## 3. The fix

**`elf_load_args()`** — `kernel/exec/elf.h`/`.c`. The existing `elf_load` body
gains a `const struct exec_args *` parameter; `elf_load` becomes a two-line
wrapper passing `0`. Structurally identical to DDR-1032's own compatibility
argument: **all 61 existing `elf_load` call sites are untouched and cannot
change behaviour**, because the wrapper passes exactly the literal the body used
to pass.

**The hook marshals `{ "AGENT", task }`** into DDR-1032's flat blob:

```c
char blob[8 + 64];                 /* "AGENT\0" + the 64-byte task buffer */
```

**An empty task passes `NULL`**, i.e. exactly today's frame (`argc = 1`).
`sys_spawn_agent` already sets `task[0] = 0` when the caller passes no string,
so the no-config path stays bit-identical rather than acquiring an empty
`argv[1]`.

**The agent reads `argv[1]`** and prints `argc`.

**The config's task becomes `verify-boot-chain`** — a string that, measured,
appears **nowhere in the tree**, and specifically not in `agent_base.c`. That is
what makes the arm discriminating: the agent cannot print it without it having
crossed daemon → syscall → argv frame → `main`. The DDR-1066 discipline —
*print from the thing you cannot manufacture* — applied to a string instead of
an errno.

### 3.1 Blast radius, measured before writing

* `PRADYOS_AGENT_START` is asserted by **no gate** — `grep -rn` over `Makefile`,
  `tools/` and `kernel/` returns one Makefile *comment* and the `printf` itself.
  So changing that line breaks nothing.
* `PRADYOS_AETHER_CFG_OK` has **two** assertion sites: `Makefile:2270` matches
  only `mode=sovereign` (unaffected), and `Makefile:4260` matches
  `… task=test slot=0` — **one line, updated here.**
* **The config text is DUPLICATED** and both copies had to move: `Makefile:2258`
  (`AETHER_CFG_TEXT`, the host `mkfs.sfs` image DDR-770 roots at) and
  `kernel/main.c:2929` (`CFGTEXT`, kernel-provisioned per DDR-760/761 — **this is
  the one `smoke-aethercfg` actually reads**). Found by changing the Makefile
  first and watching the gate still print `task=test`. Two copies of one boot
  policy is a drift hazard: keep them in step or the two config gates disagree
  about what the policy is.
* **`build/sfsroot.img` did not depend on the Makefile that defines its content.**
  Its rule read `build/sfsroot.img: $(MKFS_SFS)`, so editing `AETHER_CFG_TEXT`
  left a stale image locally while CI, cloning fresh, built the new one — the
  same commit meaning two different boot policies depending on where it ran.
  `Makefile` is now a prerequisite. Not a defect this change introduced; a
  defect this change was the first to trip over.
* The agent is a musl program and this is the first musl program to receive a
  marshalled argv, which is worth checking rather than assuming: `elf_build_image`
  lays a full SysV frame — `argc`, `argv[]`, NULL, `envp[]`, NULL, `AT_PAGESZ`,
  `AT_NULL` — and the agent **already runs through this exact code** at
  `argc = 1`, so musl's `_start` reaches `main` today. One extra `argv` slot does
  not change the frame's shape, and DDR-1032's parity pad (7 fixed slots + one
  per string) handles the alignment it does change.

---

## 4. The gate

**No new gate (178 unchanged)** — the arms go on **`smoke-aethercfg`** (shard 1,
90 s, **strict**), the DDR-1039/1070 reasoning. That is the right gate rather
than a convenient one: its entire subject is config → behaviour, and `task` is
the one of its three fields that never arrived.

Two required patterns, independent greps over the capture:

| arm | pattern | what only it can show |
|---|---|---|
| A | `task=verify-boot-chain` | the **string** crossed both boundaries |
| B | `mode=test argc=2` | the **count** crossed (the NULL-args frame is `argc=1`) |

plus the existing `PRADYOS_AETHER_CFG_OK mode=sovereign task=verify-boot-chain
slot=0` and `PRADYOS_AGENT_DONE`, and the existing forbidden
`PRADYOS_AETHER_CFG_DEFAULT`.

**The obvious arm is rejected and §2.1 is why:** `task=test` passes with no
plumbing at all.

---

## 5. Mutants

Both land, and they land on **different arm sets, neither carrying the other**
(the DDR-1044 M2/M3 check):

| mutant | kernel | change | capture | fails |
|---|---|---|---|---|
| **M1** | `a67891d6652dbe44` | the hook restores `(void)task;` and calls `elf_load` — literally the pre-fix tree | `PRADYOS_AGENT_START task=test mode=test argc=1` | **A and B** |
| **M2** | `de670aee90a1cb7d` | the agent restores `argv[2]` — the pre-fix index, §2.2 | `PRADYOS_AGENT_START task=test mode=test argc=2` | **A only** |

M1's capture is worth reading past its verdict: it still carries
`PRADYOS_AETHER_CFG_OK mode=sovereign task=verify-boot-chain slot=0`. The daemon
parsed the value correctly and the agent printed the fallback anyway — the defect
localised to the hook, in one capture, without inspection.

M2 is the load-bearing one for §2.2: the frame is correct, the count is correct,
and the program still cannot find its own argument. It is also the mutant that
demonstrates the two halves are genuinely separate defects rather than one
described twice.

Reverting both returns `kernel.bin` to `d4b148faaca8ce09` **bit-for-bit**,
verified by rebuild rather than assumed.

### 5.1 M2 first PASSED, and it was this DDR's own gate that was wrong

Arm A was specified in §4 as the bare string `task=verify-boot-chain`, described
there as an independent grep. **It is not independent: it is a SUBSTRING of the
`PRADYOS_AETHER_CFG_OK mode=sovereign task=verify-boot-chain slot=0` line that
the same gate already requires.** So M2 — the agent reading an index nothing
supplies — ran, printed `task=test`, and the gate went green, because the
*daemon* echoing its own parse satisfied the arm meant to prove the *agent* had
received anything.

Measured, not deduced: on the M2 capture `grep -c 'task=verify-boot-chain'`
returns **1**, and that one occurrence is the daemon's line.

Arm A is now `PRADYOS_AGENT_START task=verify-boot-chain`, a prefix the daemon
cannot produce. The corrected gate was re-run **with M2 still applied** before
reverting anything, and fails arm A alone with arm B present in the capture.

Two things about this are worth carrying rather than quietly fixing. First, it is
the same vacuity class as §2.1 one level up — an arm whose passing value is
reachable without the thing under test — arriving in the gate written *to catch*
that class. Second, and this is the part §4's "measured before writing"
discipline missed: **checking a new pattern against the product is not enough; it
must also be checked against the gate's OTHER required patterns**, because a
sentinel list is a set of substring greps over one capture and any pattern can be
satisfied by any line. Caught by a mutant rather than by reading, the DDR-1033
shape.

Mutating both in one edit was deliberately avoided — attribution from a mutation
that changed two things is the DDR-1042 failure mode.

---

## 6. NOT CLAIMED

* **No respawn is built.** The Group F row is corrected, not closed; §1 is a
  reading of the row, not a delivery against it.
* **`INIT_CAPS` is untouched and `/AGENT.ELF` is not placed.** init's refusal
  stands exactly as DDR-891 recorded it.
* **No policy decision is taken** on whether agents should be respawned, or by
  whom. DDR-891's budget analysis is cited as the hazard, not applied.
* **No kernel defect beyond the dropped argument is fixed and none is alleged** —
  `copyinstr`, `elf_build_image`, the frame layout and the roster were all
  correct; what was missing is a **caller**, the same sentence DDR-1083 and
  DDR-1084 each ended on.
* **`task` is informational.** Measured: the agent only prints it; nothing
  branches on it. So this makes an unobservable field observable and correct —
  it does not change what any agent does.
* **§2.1 is not a defect in the shipped config.** `task=test` was a reasonable
  value; what it cost is that the wire under it could not be tested, and that
  is what changes.
* **Section 3C is unaffected**; it closed at 8/8 in DDR-1084 and this touches no
  action type.
* **`kernel.bin` is 1,307,018 B — SIZE UNCHANGED**, the additions fitting inside
  existing page padding, so CLAUDE.md's size/headroom pair is untouched and
  `ci-docstate-check` is unaffected. Verified by rebuild, not assumed.
* **No new gate (178 unchanged)** and no shard rebalancing —
  `smoke-aethercfg` keeps its 90 s window.
* `GLOBAL_FORBIDDEN` **76 unchanged**; no open issue moves (OPEN-1/2/12/13
  untouched); this is not an apfreeze and not OPEN-2.

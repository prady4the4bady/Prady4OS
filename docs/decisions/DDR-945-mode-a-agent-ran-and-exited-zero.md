= DDR-945 — mode A: the agent RAN and exited 0. DDR-936's framing is refuted.

**Status:** ACCEPTED (refutation is measured). Leading hypothesis is flagged as
hypothesis. **No fix.**
**Date:** 2026-08-16
**Evidence:** CI run 31958185299 (tip `1516720`), shard 3, `smoke-agent-click`.
**Lineage:** DDR-936 → DDR-940 (two modes; pid instrument) → **DDR-945 (this)**.

## The measurement

```
PRADYOS_AGENT_TRIGGER name=PRAX slot=1 pid=82
sys_exit(0) pid=82
```

**pid 82 exited, cleanly, with status 0.** The pid tag added in DDR-940 exists
precisely to make this readable; without it the bare `sys_exit(0)` was
ambiguous between "the agent ran" and "some other thread exited".

## What this refutes — including my own work

**DDR-936's framing was "created but never executes its first instruction".
For mode A that is FALSE.** The thread was created, scheduled, executed, and
terminated normally. Mode A is **not a scheduler defect**.

Everything built on that framing for mode A is retired:

| retired for mode A | why |
|---|---|
| `sched_unblock` CAS gate (DDR-936) | already excluded by `ubcas=0` |
| `rq_on` gate (DDR-936) | already excluded by `ubrq=0` |
| pick-time drop (DDR-941) | already excluded by `rqmiss=0` |
| non-present-CPU strand (DDR-944) | `rqq=1 rqpres=1` — queued CPU **is** present |
| "the thread never ran" (DDR-936) | **`sys_exit(0) pid=82`** |

That is eight mechanisms retired by instruments across this investigation, and
**not one was retired by argument**. Each was plausible; each was wrong.

§6.0-C: this says nothing about the blk workers or rqstress. Those are kernel
threads with `done=0x0` and `n=8/24`, a different signature in a different
subsystem. They keep their own root cause.

## Leading hypothesis — stdout is not wired, so the agent's output is discarded

**HYPOTHESIS, not a finding.**

`main()` in `user/agent_base.c:135-140` prints `PRADYOS_AGENT_START` as its
**first** statement. The agent ran and exited 0 without it appearing, so either
`main` was not entered or **its output went nowhere**.

`sys_write` requires a wired descriptor (`sys_io.c:144-147`):

```c
struct fd_entry *e = fd_get(current_thread, (int)fd);
if (!e) return -EBADF;
```

`sched_create_state` calls `fd_table_init(&t->fdt)` — "all fds free; user
threads get stdio wired below" (`sched.c:868`) — and
`aether_spawn_agent_hook` (`main.c:856-868`) wires **nothing**:

```c
if (elf_load(…, &ut) != ELF_OK || !ut) return -1;
ut->is_agent = 1; ut->is_net = 1; ut->parent_pid = g_aether_daemon_pid;
sched_unblock(ut);
```

If fd 1 is unwired, every `printf` returns `-EBADF`, musl discards it, and the
agent completes its work and exits 0 **invisibly** — exactly what was measured.

### The obvious objection, and why it may still hold

If the hook never wires stdio, mode A should fail **100%** of the time, not
~11%. It does not. The candidate explanation is that the gate's assertion is
satisfied *coincidentally* on passing runs:

```make
awk '/PRADYOS_AGENT_TRIGGER name=PRAX/{t=1} t&&/PRADYOS_AGENT_DONE/{ok=1} END{exit !ok}'
```

It accepts **any** `AGENT_DONE` after the trigger — not the clicked agent's.
The AETHER daemon runs its own test agents throughout the boot, and those do
print. So a "pass" may mean "a daemon agent happened to finish after the click",
and the clicked agent may print **nothing on every run**, pass or fail.

If that is right, `smoke-agent-click` has been ~89% green for the wrong reason
and has never actually verified what its name claims. That is a bigger problem
than the flake.

## What would confirm / refute

- **Confirm:** on a PASSING run, no `AGENT_START` appears between the trigger
  and the `AGENT_DONE` that satisfies the gate — i.e. the DONE belongs to a
  daemon agent. And/or `sys_write` returning `-EBADF` for pid 82.
- **Refute:** a passing run shows the clicked agent's own
  `AGENT_START`/`AGENT_DONE` pair, in which case the click path does wire stdio
  and the ~11% is a genuine intermittent elsewhere.

**The discriminating measurement is on a PASSING run**, which is unusual and is
the whole point: the failing runs cannot distinguish these, and the passing ones
can.

## Not doing

No fix. Not touching the hook, the gate assertion, or the fd table until the
passing-run measurement is in. Wiring stdio "because it looks missing" is
exactly the move that produced eight retired mechanisms.

---

## The passing-run measurement REFUTES the stdout hypothesis (same day)

Ran the discriminating test immediately. `smoke-agent-click`, **passing** run,
tip `1516720` + instruments:

```
328: PRADYOS_AGENT_TRIGGER name=PRAX slot=1 pid=45
332: PRADYOS_AGENT_TRIGGER name=PRAX slot=1 pid=46
335: PRADYOS_AGENT_TRIGGER name=PRAX slot=1 pid=47
338: PRADYOS_AGENT_TRIGGER name=PRAX slot=1 pid=48
355: PRADYOS_AGENT_START task=test mode=test      <- after the triggers
358: PRADYOS_AGENT_DONE
359/362, 363/366, 367/373                          <- four START/DONE pairs
381: sys_exit(0) pid=45
382: sys_exit(0) pid=47
383: sys_exit(0) pid=48
385: sys_exit(0) pid=46
```

Four triggers, **four** `START`/`DONE` pairs after them, then exactly those four
pids exit. So:

- **stdio IS wired** for click-spawned agents — `printf` reaches the serial log;
- the gate is **not** passing coincidentally on daemon agents;
- the "obvious objection" in the section above was the correct instinct, and the
  hypothesis it was defending is dead.

**Hypothesis refuted. Ninth mechanism retired, again by measurement.**

Also newly visible: the injector produces **four** triggers per run (its three
press rounds plus one), each spawning a distinct agent. Any reasoning that
assumed one clicked agent per run was working from a false premise — including
DDR-936's, which read a single `pid=82`.

## Where mode A actually stands

On a **failing** run the clicked agent runs and exits 0 **without printing**; on
a **passing** run an identically-spawned agent prints normally. Same code path,
same wiring. So the defect is intermittent *between the exec and the first
`printf`*, and the remaining candidates are:

1. `main` is entered and the output is **lost in transport** — the UART drops
   under contention. The `[hb]` line already carries `thre_drops=`; it read 0 in
   the samples inspected, but it was **not** checked on the failing boot
   specifically. **Cheapest next measurement — do this first.**
2. `main` is **not** entered: musl crt/`__libc_start_main` fails and exits 0.
   Would need a pre-`main` marker to distinguish.
3. The agent is killed/exits early via a path that reports status 0.

Do not guess between these. Read `thre_drops` on the next mode-A failure first.

### Candidate 1 (UART transport loss) is ALSO refuted

Checked on the failing boot itself (CI 31958185299): **`thre_drops=0
rx_drops=0` on all 21 heartbeats.** The UART dropped nothing, so the missing
`AGENT_START` was never written, not written-and-lost.

**Tenth mechanism retired.** Remaining: `main` not entered (musl crt exits 0),
or an early exit that reports status 0. Distinguishing them needs a marker
emitted *before* the first `printf` — via `kputs` from the kernel side at the
exec, not from ring 3, since ring-3 output is the thing in question.

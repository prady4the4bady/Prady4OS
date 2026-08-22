# DDR-979 — Intermittent ring-0 panic in early boot (OPEN), and the capture bug that hid it

Status: OPEN defect, recorded with the evidence that exists. **No fix** — the one
artefact was destroyed at capture time, and §NON-NEGOTIABLE 3 forbids guessing.
A harness fix ships here so the next occurrence is diagnosable.
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§INV.4).

---

## 1. The event

CI run `32595646699`, shard 0, head `b43d6b0`, first gate of the shard:

```text
SYSCLOSE OK

*** NEXUS KERNEL PANIC ***
component: NEXUS isr
```

`component: NEXUS isr` is `idt.c:463` — a **ring-0** exception. Everything before
it is a normal boot: `[boot-stamp] A … t=181`, HELLO/WXVIOL/SYSTEST/INPUTTST
loaded, all the SYS* self-tests OK. The panic lands at **t≈185**.

`shard 0: FAILED at smoke-blk-integrity after 1 of 22 gates`.

## 2. It is not from the diff it failed on

Three independent reasons:

1. **`b43d6b0`'s only kernel change is gated off at that point.** It adds
   `cputicks[…]` to the heartbeat, inside `if ((now % 500) == 0)`. The panic is
   at t≈185 and **the log contains no `[hb]` line at all** — the first heartbeat
   would be t=500. That code never executed.
2. **Same SHA, one red and one green.** `b43d6b0` ran two independent full
   matrices: run `32595644683` (push) **succeeded**, run `32595646699`
   (pull_request) **failed**. Same commit, same binary.
3. **0/10 locally** at branch HEAD (`smoke-blk-integrity`, ten consecutive runs,
   zero panics).

So: a pre-existing intermittent, not a regression. It is nonetheless a **real
ring-0 panic** and it has not been recorded before, so it is logged here rather
than absorbed as noise.

## 3. Why it cannot be diagnosed yet — the capture bug

The panic prints its identifying block right after the banner:

```c
kputs("exception: "); kputs(name);
kputs("  vector="); kputhex(r->vector);
kputs("  error="); kputhex(r->err_code);
dump_line("RIP=", r->rip);   /* … CS, RFLAGS, RSP, CR2, GPRs */
```

None of it survived. What the job log holds is:

```text
component: NEXUS isr
make: *** [Makefile:2307: smoke-blk-integrity] Error 1
exception: make: Leaving directory '/home/runner/work/Prady4OS/Prady4OS'
```

`exception: ` is guest serial on **stdout**; `make: ***` is make's **stderr**.
Two separate file descriptions writing one pipe with no ordering guarantee, so
they interleave — here **mid-line**, overwriting the exception name, vector,
error code and RIP. The only lines that identify the fault are exactly the lines
that were lost.

**Fix (shipped here):** `tools/ci/run_shard.sh` now runs `make … 2>&1`, putting
both streams on one file description so writes are ordered by the kernel. Costs
nothing; makes the next occurrence readable.

This is the same family as the vacuous gates this project keeps finding
(DDR-971, DDR-973 §6, DDR-978 §3.2, DDR-880's harness-echo detector) — not a
test that asserts too little, but a harness that destroys the evidence it
collected. Worth naming separately because the failure mode is invisible until
you need the artefact.

## 4. What is known, and what a diagnosis needs

**Known:** ring-0 exception; ~t=185; after `SYSCLOSE OK` and the INPUTTST.ELF
load; on shard 0's first gate; rate low enough that 10 local runs and one of two
CI matrices on the same SHA saw nothing.

**Needed:** one capture with the `exception:`/`vector=`/`RIP=` block intact. With
§3's fix the next occurrence supplies it; `tools/ci/sym_at.sh` then maps the RIP
to a symbol.

**Do not guess from `component: NEXUS isr` alone.** It is printed for *every*
non-recoverable ring-0 vector — #GP, #PF, #UD, #DF all land there. Nothing in the
surviving text distinguishes them, and picking one would be colour-matching.

## 5. Tracking

Added to `CLAUDE.md` §OPEN ISSUES as **OPEN-12**, and to `docs/build_status.md`.
Reopen-on-sight: any `*** NEXUS KERNEL PANIC ***` in CI or a local run. The
PR-#6 branch is unaffected — its head is green and this does not gate it.

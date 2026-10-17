# DDR-1110 — SECTION 3's corrections live in the wrong document, and one of them RELOCATES its blocker rather than removing it

**Assessment + correction. Docs-only: no code change, no gate, NO DEFECT FOUND
AND NONE ALLEGED.** Written from CLAUDE.md's own queue position 2 ("remaining
`docs/PRE_LAUNCH_CHECKLIST.md` items") while DDR-1108's follow-up suites run on
a bit-identical binary.

---

## §1 — The shape, before any individual row

`PRE_LAUNCH_CHECKLIST.md` §3 is titled *"CORRECTIONS: DEFERRAL ENTRIES THAT ARE
NOW STALE"* and opens by saying `docs/BUILD_TRACKER.md`'s pre-approved-exception
list *"still carries three `[DEFERRED]` entries for work that **has since
shipped**. They are corrected here; **the tracker rows themselves are superseded
by this section**."*

That sentence is the defect. **A correction that supersedes a row from a
different document does not change what a reader of that row sees** — and the
tracker is where a session looking for unbuilt work looks, because that is what
the tracker is for. This is the DDR-1084 §1 family in the form where the remedy
was *written down* and not *applied at the site*, and it is the expensive
direction of it: a stale `[DEFERRED]` on shipped work does not merely
under-report progress, it tells the next session not to look.

**One of the three was corrected in place and is stated first, because an audit
that only reports errors is not an audit:** `ACTION_RUN_EXPERIMENT`'s row carries
an indented **"RETIRED 2026-09-07 — DDR-1083"** block naming all three of its
retired blockers and the caller that closed the gap. That is exactly the right
shape. The other two do not have it.

---

## §2 — `ACTION_SEND_IPC`: stale, and its own sub-bullet is falsified

The row still reads `[DEFERRED: no ring-3 IPC surface — ipc_send/ipc_recv are
kernel-internal … there is no SYS_IPC_*, so an approved SEND_IPC has no executor
in any ring]`, and **underneath DDR-1083's retirement block sits the line
`ACTION_SEND_IPC` *stays deferred* and is not re-assessed**.

Both are false, measured rather than inherited:

- `user/actionipctest.c` (8,783 B) hand-copies `ACTION_SEND_IPC 7` and calls
  `SYS_SUBMIT_ACTION` at `:136`.
- `smoke-sendipc` requires `PRADYOS_IPCACT_A st=2 …` among its sentinels.
- `SYS_IPC_SEND`/`SYS_IPC_RECV` are NSI 98/99 (DDR-1033), so the *door* half of
  the original blocker went first and the *caller* half went in DDR-1084.
- Section 3C closes at **8 of 8**.

**The timing is the point, not an accident.** DDR-1083 wrote "stays deferred"
and **DDR-1084 wired it one commit later** — so the sentence was already on its
way to being false when it was written, by the session that was about to
falsify it. DDR-1084 §1 named exactly this pattern from two instances and
declined to build a checker because the signal is semantic; **this is the
pattern appearing inside its own predecessor's text**, which is the sharpest
form of it so far and still does not make a checker buildable.

---

## §3 — F#73: the stated blockers ARE gone, and the substantive one was never named

§3's F#73 row says *"Both blockers are gone."* **True of both stated blockers**,
verified in the tree rather than taken from the checklist (DDR-1007's
discipline):

| stated blocker | state |
|---|---|
| *"no windowed terminal client"* | **GONE** — `user/term.c`, 9,230 B; `smoke-ctrlaltt` exists and is registered on **shard 0** (DDR-1027) |
| *"`sys_exec.c:47` discards argv/envp"* | **GONE** — `marshal_vec` at `sys_exec.c:53`, called at `:113` for `uargv` **and** `:116` for `uenvp` (DDR-1032) |

**And the row is still blocked, on something neither it nor §3 names: there is
no natural-language capability on this machine at all.** Measured:

- The **only** inference path in the tree is `ollama_generate()`
  (`user/agent_base.c:97`), which `SYS_SOCK_CONNECT`s to **`10.0.2.2:11434`** —
  `OLLAMA_HOST_BE`'s own comment says `10.0.2.2 = the QEMU SLIRP gateway (host)`.
  **The model is off-box, on the developer's machine.**
- `AETHER_TEST_MODE` **defaults to 1** (`agent_base.c:19`), and that is the CI
  path: the "response" is a fixed string.
- `smoke-agent-live` is in `shard_check.sh`'s `EXCLUDE` set with the reason
  stated there — *"developer-run only: needs a live Ollama endpoint on the host,
  so **CI stays in test mode** (ADR-027)"*.
- No in-tree model, no tokenizer, no local inference: `grep -rniE
  'ollama|llama|inference|tokenizer|\bLLM\b'` over `kernel/` and `user/` returns
  the Ollama client and nothing else.

So an F#73 surface would be a window onto **a process on another machine**,
reached over the proxy socket — which is the cloud-bridge posture **DDR-793
deferred post-1.0** as a *security-posture change*, i.e. an operator decision,
not a feature someone can pick up.

**THIS RELOCATES THE BLOCKER AND DOES NOT SHRINK IT** — the DDR-1104 shape,
where the TLS row's named library turned out to be the wrong obstacle and the
trust anchor was the real one.

### §3.1 — Why this correction matters more than the two in §2

The §2 rows are stale in the *safe* direction once found: they under-report
finished work, and a session that checks finds the work done. **§3's F#73
correction is stale in the dangerous direction**: read alone it says the row is
unblocked, which would send a session to build a natural-language interface on a
system with **no natural-language model** — and the stale `[DEFERRED]` it
replaces would at least have stopped them. A correction that removes a wrong
blocker without naming the right one is worse than the blocker it removed.

---

## §4 — What is changed

Three edits, all at the **site** rather than in a second document — which is
this DDR's own finding applied to itself:

1. `BUILD_TRACKER.md`: `ACTION_SEND_IPC`'s row gets a **RETIRED — DDR-1084**
   block in DDR-1083's established shape, and DDR-1083's *"stays deferred"*
   sub-bullet is corrected in place rather than deleted, so the record shows
   what was believed and when.
2. `BUILD_TRACKER.md`: F#73's row keeps its `[DEFERRED]` status — **it is still
   deferred** — with its two retired plumbing blockers named as retired and the
   relocated blocker stated.
3. `PRE_LAUNCH_CHECKLIST.md` §3: the F#73 correction gains the relocated
   blocker, so the row can no longer be read as "unblocked"; and its preamble
   stops saying the tracker rows are "superseded by this section", because they
   are now corrected there.

**No checker is built.** The mechanical signal available is "a document says a
row is corrected and the row does not say so", which is semantic — the wall
DDR-1071 §5, DDR-1072 §2, DDR-1081 §3 and DDR-1086 §4 each measured and each
refused a checker for. The cheap substitute is DDR-1107 §3's, sharpened by this
instance: **a correction belongs in the row it corrects, and a DDR that retires
a blocker names the rows that blocker was holding.**

---

## §5 — NOT CLAIMED

- **NO code change, NO gate, NO defect found and none alleged.** `user/term.c`,
  `sys_exec`'s marshalling, `agent_base`'s live branch and `smoke-agent-live`'s
  exclusion are all correct for what they were built to do; what is corrected is
  the **rows**.
- **F#73 IS NOT UNBLOCKED** and no NL surface is built, designed or scheduled.
  DDR-793 is **not revisited** and no operator decision is taken — the cloud
  bridge stays a recorded deferral, cited because its measurement applies here
  unchanged.
- **DDR-1083 is not criticised.** Its work is real and two-sided; the defect is
  in one sentence about a *different* action type, which the very next commit
  falsified — the DDR-1084 §1 failure, not a mistake in its own subject.
- **Section 3C's 8-of-8 is not re-proved here**, it is cited: DDR-1084 carries
  the arms and the mutants.
- **No rate and no new pattern claimed** — §2 is one further instance of a family
  already named from two, and one instance is not a rate.
- `kernel.bin` **NOT rebuilt**, so the size/headroom pair and
  `ci-docstate-check` are unaffected; `GLOBAL_FORBIDDEN` **77**; **179 gates**;
  **79 probe ELFs**; no open issue moves (OPEN-1/2/12/13 untouched).

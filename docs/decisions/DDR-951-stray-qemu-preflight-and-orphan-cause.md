# DDR-951 — the stray-QEMU guard contained the symptom; the orphan had a cause

Status: ACCEPTED
Supersedes: nothing. Extends the OPEN-9 handling added in DDR-823.
Governs: `tools/qemu_runner/boot_test.sh`

## a. What was already true

OPEN-9 is recorded in the backlog as "leaked QEMU holds image write-lock — add
lock-precheck to boot_test.sh", with the failure mode "reports 'kernel sentinel
not found' instead of naming the host problem".

That misattribution was **already fixed** by DDR-823. `boot_test.sh` captures
QEMU's stderr and, when it matches the write-lock message, prints
`STALE QEMU HOLDING IMAGE LOCK` and exits **3** — a distinct code, so a
host-environment failure is never summed with a failing gate. This DDR does not
re-fix that; claiming otherwise would be a false close.

## b. What was actually still missing

The DDR-823 handler is **post-hoc**. It can only fire after the gate has burned
its entire timeout window, and only when QEMU emits the lock message. Two host
conditions slip past it:

1. **The stray holds a different image.** No lock error is printed, so the run
   proceeds normally — but two QEMUs now compete for host CPU. Every timing gate
   in this tree is a claim about wall-clock (standing rule: "a gate's timeout is
   a claim about how long the system takes"). This is a *silent* perturbation of
   every timing measurement, which is strictly worse than a loud failure,
   because nothing in the output says the measurement is void.
2. **The stray is mid-shutdown.** Whether the lock error appears is then a race,
   so one host condition produces two different verdicts across runs — the
   signature of an intermittent that has no kernel cause at all.

Both are now collapsed into one deterministic pre-flight `exit 3`.

## c. The part that was never diagnosed: where the orphan comes from

A pre-check contains the symptom. It does not explain why an orphan exists.

`boot_test.sh` kills its QEMU child when the timeout expires, so a run that
**completes** leaks nothing. The leak source is a run that is **interrupted**:
the shell dies, the QEMU child is reparented, and it holds the image write lock
until someone notices.

Interruption is not rare in this project — it is routine. The CI workflow's
concurrency group cancels the older run whenever two dispatches land on one ref,
which is the documented reason a serialised dispatch discipline exists. Ctrl-C
does the same locally.

> **CORRECTION (DDR-988 §12.4, DDR-993).** The two sentences above are FALSE and
> this is where the claim originated. There is no `concurrency:` block anywhere
> under `.github/workflows/` — `grep -n concurrency .github/workflows/*.yml`
> returns nothing — and run `32657350756` stayed `in_progress` across two
> subsequent pushes to the same ref, which a concurrency group would have
> cancelled twice. Interruption here is **occasional** (a human cancelling a
> job, runner eviction, local Ctrl-C), not routine.
>
> The claim was copied from here into `boot_test.sh`'s trap comment, from there
> into DDR-988 §12.2, and from there into a public PR comment, each time as
> established fact. DDR-988 §12.4 retracted its own copy and f45f266 corrected
> one of the two copies in `boot_test.sh`; neither traced the claim back to this
> document, so it went on being true-looking at its source for three more
> sessions. **The conclusion below is unaffected** — the trap is worth having
> for an occasional interruption exactly as much as for a routine one, since the
> orphan it prevents costs the same either way. Only the stated frequency was
> wrong.



Fix: `trap 'kill "$qemu_pid" 2>/dev/null; exit 130' INT TERM` immediately after
the child is spawned. This removes the cause; the pre-flight check above only
contains the symptom. Both are kept — the trap cannot help against an orphan
left by an *earlier* build of the script, or by `kill -9`.

## d. Verification — two arms, and one voided first attempt

Arm A (no stray): gate proceeds, boots, reports a normal kernel verdict.
  Observed: `did not trip`, rc=1 from the deliberately-absent sentinel.
Arm B (stray present, name synthesised via `exec -a`): pre-flight fires.
  Observed: rc=**3**, `HOST-ENV FAIL ... (pre-flight)`, offending pid named.
Residue after both arms: clean.

**The first attempt at this test was VOID and is recorded here because the
failure mode is one this project keeps repeating.** The harness was passed to
WSL as `bash -c '<entire script text>'`. That outer process's *argv therefore
contained the literal string* `qemu-system-x86_64`, because the script text
mentions it — and `pgrep -f` matches against full command lines. Arm A "detected
a stray" before any stray had been started. The apparatus was matching itself.

This is the same class of error as the `pgrep` name-form defect (§0.3) and the
`$(md5sum …)`-through-WSL defect (RULE 24): **the measurement layer altered what
was measured.** The rule that follows: a probe for a process name must never be
invoked from a command line that contains that name — put the harness in a file,
where only the filename reaches argv.

## e. Not done, and why

`pkill -f qemu-system-x86_64` on every exit was considered and **rejected**. It
kills processes this script did not start. On a shared host that silently
destroys a gate the operator launched deliberately, and the operator would see
the same "stale QEMU" story with no indication that this script was the killer.
The pre-flight check therefore *names the pids and stops*, leaving the kill to a
human. Only our own `$qemu_pid` is ever killed automatically.

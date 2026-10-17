#!/usr/bin/env bash
# resize_inject.sh — multi-edge resize injector for smoke-resizeall (DDR-997).
#
# drag_inject.sh drives ONE drag from ONE published handle. DDR-997 adds seven
# more handles and needs four drags in a single boot, with the handles
# re-resolved between them: a W drag moves the window, so every other handle
# published before it is stale. This script therefore re-reads the compositor's
# own PRADYOS_WM_GEOM line before each arm and waits for a FRESH one after it.
#
# Nothing here is a pixel constant (§INV.5 / §NON-NEGOTIABLE 9). Even the drag
# END points are derived from published handles: rzw= and rze= bracket the
# window (x+7 and x+w-7), so "drag the west edge past the 32 px floor" is
# expressed as "drag rzw to rze's column" and holds for any window size.
#
#   $1 log   $2 qmp sock   $3 readiness sentinel
#   RZ_ID    surface id to resize
#   RZ_ARMS  space-separated arms, default "e s w n" (order matters: the
#            shrink arms run last so the grow arms have room)
set -u
log="$1"
sock="$2"
sentinel="${3:-PRADYOS_AMBIANCE_OK}"

# DDR-997 §13.4, SECOND attempt. The first set the budget as "240s from
# when I connect", which silently assumes a boot time. MEASURED: CI boots in
# ~49s but this container boots in ~110s, so 110+240 overran a 340s cap and
# the guest was SIGTERM'd again -- the exact failure the budget exists to
# prevent. A budget denominated in post-boot seconds cannot bound a cap
# denominated in wall-clock seconds.
#
# T0 is stamped here, before the sentinel wait, i.e. within a second of QEMU
# starting. The injector derives its deadline from the SAME cap the recipe
# gives QEMU, so however long the boot takes it still stops in time to report.
RZ_T0="$(date +%s)"

for _ in $(seq 1 600); do
    grep -q "$sentinel" "$log" 2>/dev/null && break
    sleep 0.1
done
sleep 0.5

SOCK="$sock" LOGFILE="$log" RZ_ID="${RZ_ID:-1}" RZ_ARMS="${RZ_ARMS:-e s w n}" \
GROW_X="${GROW_X:-3200}" GROW_Y="${GROW_Y:-2560}" \
ARM_TIMEOUT_S="${ARM_TIMEOUT_S:-25}" RZ_BUDGET_S="${RZ_BUDGET_S:-240}" \
RZ_T0="$RZ_T0" RZ_CAP_S="${RZ_CAP_S:-0}" RZ_MARGIN_S="${RZ_MARGIN_S:-30}" python3 - <<'PY'
import os, socket, json, time, sys

sock    = os.environ["SOCK"]
logfile = os.environ["LOGFILE"]
sid     = os.environ["RZ_ID"]
arms    = os.environ["RZ_ARMS"].split()
grow_x  = int(os.environ["GROW_X"])
grow_y  = int(os.environ["GROW_Y"])
arm_to  = float(os.environ["ARM_TIMEOUT_S"])
budget  = float(os.environ["RZ_BUDGET_S"])
t0      = float(os.environ["RZ_T0"])
cap     = float(os.environ["RZ_CAP_S"])      # 0 = no cap given, use budget
margin  = float(os.environ["RZ_MARGIN_S"])

# Edge bits — must match user/compositor.c's RZ_N/S/W/E.
RZ_N, RZ_S, RZ_W, RZ_E = 1, 2, 4, 8
TRACK_WAIT_S = 20.0     # how long to wait for the compositor to SEE the drag
FIX_WAIT_S   = 8.0      # how long to wait for its commit to reach the log
NEED = {"e": RZ_E, "s": RZ_S, "w": RZ_W, "n": RZ_N}


def log_lines():
    try:
        with open(logfile, "r", errors="replace") as fh:
            return fh.read().splitlines()
    except OSError:
        return []


def geom_lines():
    tag = "PRADYOS_WM_GEOM id=%s " % sid
    return [ln for ln in log_lines() if tag in ln]


def parse_geom(ln):
    """Field name -> (x, y). Isolate the field before splitting on ',' (§INV.5)."""
    out = {}
    for tok in ln.split():
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        if "," not in v:
            continue
        a, b = v.split(",", 1)
        if a.lstrip("-").isdigit() and b.lstrip("-").isdigit():
            out[k] = (int(a), int(b))
    return out


def newest_geom(need, deadline):
    """Newest geom line for this surface carrying every field in `need`."""
    while time.monotonic() < deadline:
        for ln in reversed(geom_lines()):
            g = parse_geom(ln)
            if all(k in g for k in need):
                return g
        time.sleep(0.1)
    return None


s = None
for _ in range(50):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(sock)
        break
    except OSError:
        s = None
        time.sleep(0.2)
if s is None:
    print("[resize_inject] FAIL — could not reach QMP at %s" % sock)
    raise SystemExit(0)

f = s.makefile("rw", newline="\n")
f.readline()                                    # QMP greeting


def cmd(o):
    f.write(json.dumps(o) + "\n"); f.flush(); f.readline()


def absmove(x, y):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": x}},
        {"type": "abs", "data": {"axis": "y", "value": y}}]}})


def btn(down):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"button": "left", "down": down}}]}})


def track_count():
    return sum(1 for ln in log_lines()
               if "PRADYOS_RESIZE_TRACK id=%s " % sid in ln)


def drag(sx, sy, ex, ey):
    """Press on the handle, drag, and release ONLY once the compositor has been
    seen at the new position.

    The fixed 0.45 s that used to sit before the release was a bet that the
    compositor polled in between. It holds locally and LOST IN CI: every arm
    committed at its own START coordinate (measured — DDR-997 §9.8), because
    SYS_MOUSE_POLL reads current state rather than an event queue (DDR-941), so
    a pointer move that falls entirely between two polls is never observed at
    all. PRADYOS_RESIZE_TRACK is one line per drag saying the compositor has
    now seen the pointer away from the press point; waiting for it turns the
    bet into a precondition. DDR-910's rule — poll for the outcome, never a
    fixed wait — applied to the drag phase instead of the click.
    """
    before_track = track_count()
    absmove(sx, sy); time.sleep(0.35)           # over the handle
    btn(True);       time.sleep(0.45)           # press: latches the edge mask
    absmove(ex, ey)                             # drag, button held
    seen = False
    # 6 s was measured too short on a loaded CI runner: every arm reported
    # "no RESIZE_TRACK within 6s" and released blind, which is exactly the
    # garbage observation this wait exists to prevent (DDR-997 §11).
    stop = time.monotonic() + TRACK_WAIT_S
    while time.monotonic() < stop:
        if track_count() > before_track:
            seen = True
            break
        time.sleep(0.1)
    if not seen:
        # Do not release blind: a release the compositor scores at the press
        # point produces a self-consistent but WRONG-DISTANCE commit, which is
        # exactly the CI failure this replaced. Say so and let the arm retry.
        print("[resize_inject] no RESIZE_TRACK within %gs — compositor never "
              % TRACK_WAIT_S +
              "observed the drag; retrying this round")
        sys.stdout.flush()
    time.sleep(0.25)                            # let the position settle
    btn(False);      time.sleep(0.6)            # release: commit


def fix_count(edge_bit):
    n = 0
    for ln in log_lines():
        if "PRADYOS_RESIZE_FIX id=%s " % sid not in ln:
            continue
        for tok in ln.split():
            if tok.startswith("edge="):
                try:
                    if int(tok.split("=", 1)[1]) & edge_bit:
                        n += 1
                except ValueError:
                    pass
    return n


cmd({"execute": "qmp_capabilities"})

# ---- DDR-997 §13.4: a total injection budget, so SIGTERM never arbitrates ----
#
# MEASURED (DDR-997 §13.1). The guest is capped by `timeout` in the Makefile
# recipe. §12 raised the per-round waits to TRACK_WAIT_S + FIX_WAIT_S = 28 s,
# and 4 arms x 3 rounds x 28 s = 336 s, against a cap that also has to cover a
# 49 s boot. So a run that needed to retry was killed mid-arm rather than
# reporting, and CI run 33247210328 is that artefact: arm w was cut off and arm
# n never executed at all -- yet printed "FAIL" in the same words a real
# failure uses.
#
# The waits are upper bounds and break early, so a healthy run pays none of
# this. That is exactly why the overrun survived review: it is invisible until
# the run whose diagnosis matters most.
#
# Two rules follow, and they are separate:
#   1. No arm may consume the budget the later arms need -- each remaining arm
#      gets an equal share of what is left, so arm n cannot be starved by arm e.
#   2. An arm with no budget left is SKIPPED, and says so. "Did not run" and
#      "ran and failed" are different facts and a gate that prints the same word
#      for both cannot be read. That is the vacuity trap DDR-973 §6 and
#      DDR-996 each caught once.
# Wall-clock seconds already spent since the recipe started QEMU. Bridges
# `date +%s` (epoch, stamped in bash) to time.monotonic() (arbitrary base).
spent     = max(0.0, time.time() - t0)
if cap > 0:
    # Stop `margin` seconds before the guest is killed, so the report always
    # gets out. Never exceed the budget either -- the cap bounds it, it does
    # not license more.
    left = min(budget, cap - margin - spent)
else:
    left = budget
hard_stop = time.monotonic() + max(0.0, left)
print("[resize_inject] budget: %.0fs boot already spent, %.0fs to inject, "
      "guest cap %.0fs, margin %.0fs" % (spent, max(0.0, left), cap, margin))
sys.stdout.flush()
skipped   = []


def share_deadline():
    """Deadline for the arm about to run: an equal cut of what remains."""
    left = hard_stop - time.monotonic()
    n    = max(1, len(arms) - arms.index(arm))
    return time.monotonic() + max(0.0, min(arm_to, left / n))


for arm in arms:
    bit = NEED[arm]
    if time.monotonic() >= hard_stop:
        skipped.append(arm)
        print("[resize_inject] arm=%s SKIPPED — injection budget of %.0fs "
              "exhausted before this arm started; it did NOT run and its "
              "absence from the log is not a failure of the arm"
              % (arm, budget))
        sys.stdout.flush()
        continue
    # Every arm needs rzw= and rze= as well as its own handle: those two bracket
    # the window, which is how the over-drag END is derived without a constant.
    need = ["rzw", "rze", "rzn", "rzs"]
    g = newest_geom(need, share_deadline())
    if g is None:
        # §13.4: this used to sys.exit(1). That killed the injector outright,
        # so every LATER arm silently never ran AND the skipped sidecar was
        # never written -- reintroducing, inside the fix for it, the exact
        # "never ran looks like failed" ambiguity §13.4 exists to remove.
        # Record and carry on: a later arm may still find geometry, and
        # whatever happens the sidecar is written at the end.
        skipped.append(arm)
        print("[resize_inject] arm=%s SKIPPED — no PRADYOS_WM_GEOM for id=%s "
              "with %s within its share of the budget; it did NOT run"
              % (arm, sid, " ".join(need)))
        sys.stdout.flush()
        continue
    if arm == "e":                              # grow east; origin must not move
        sx, sy = g["rze"]; ex, ey = sx + grow_x, sy
    elif arm == "s":                            # grow south; origin must not move
        sx, sy = g["rzs"]; ex, ey = sx, sy + grow_y
    elif arm == "w":                            # shrink west PAST the 32 px floor
        sx, sy = g["rzw"]; ex, ey = g["rze"][0], sy
    else:                                       # "n": shrink north past the floor
        sx, sy = g["rzn"]; ex, ey = sx, g["rzs"][1]
    before_fix = fix_count(bit)
    print("[resize_inject] arm=%s start=%d,%d end=%d,%d" % (arm, sx, sy, ex, ey))
    sys.stdout.flush()
    # Three rounds: a missed press phase still lands.
    #
    # MEASURED (DDR-997 §11): geometry used to be resolved ONCE per arm and
    # reused by every round. When a round partly landed, the next round dragged
    # from coordinates that were correct BEFORE the window moved — so a retry
    # hit a different band and logged a legitimate-looking but wrong-arm
    # observation. In CI that produced `edge=8 … w0=150 → w=143`, an E-arm
    # "shrink", from a retry after the W arm had already moved the window.
    #
    # Re-resolving per ROUND makes every round a fresh, correct drag, which is
    # what lets the checker keep its "every observation must hold" rule: that
    # rule is right for independent repeats and wrong for retries against stale
    # geometry. Fixing the generator is the honest fix; loosening the checker
    # would have hidden this instead.
    for _round in range(3):
        # §13.4: the budget binds inside the arm too. Without this an arm that
        # entered with time left could still run three full 28 s rounds and
        # overrun the cap on its own.
        if time.monotonic() >= hard_stop:
            print("[resize_inject] arm=%s round %d ABANDONED — budget exhausted"
                  % (arm, _round))
            sys.stdout.flush()
            break
        if _round:                              # round 0 already resolved above
            g = newest_geom(need, share_deadline())
            if g is None:
                break
            if arm == "e":
                sx, sy = g["rze"]; ex, ey = sx + grow_x, sy
            elif arm == "s":
                sx, sy = g["rzs"]; ex, ey = sx, sy + grow_y
            elif arm == "w":
                sx, sy = g["rzw"]; ex, ey = g["rze"][0], sy
            else:
                sx, sy = g["rzn"]; ex, ey = sx, g["rzs"][1]
            print("[resize_inject] arm=%s round %d re-resolved start=%d,%d end=%d,%d"
                  % (arm, _round, sx, sy, ex, ey))
            sys.stdout.flush()
        drag(sx, sy, ex, ey)
        # MEASURED (DDR-997 §12): this used to test fix_count IMMEDIATELY after
        # drag() returned. The compositor commits on the release and then has to
        # get the line out; checking at once scores a SUCCESSFUL round as failed
        # and triggers a retry — and the retry then drags against geometry that
        # has not been republished yet, producing the wrong-arm observations
        # §11 blamed on stale handles. §11's diagnosis was right about the
        # symptom and wrong about the cause: the handles were stale because the
        # retry should never have happened.
        #
        # Poll for the commit instead. Third instance in this file of the same
        # mistake: checking a condition before the system has had a chance to
        # report it (§9.4 the press, §10 the drag, here the commit).
        got = False
        fix_stop = min(time.monotonic() + FIX_WAIT_S, hard_stop)
        while time.monotonic() < fix_stop:
            if fix_count(bit) > before_fix:
                got = True
                break
            time.sleep(0.2)
        if got:
            break
        # Only now is a retry justified. Wait for geometry published AFTER this
        # round before re-resolving, or the next round inherits the stale rect.
        tail_r = len(log_lines())
        gtag = "PRADYOS_WM_GEOM id=%s " % sid
        gstop = min(time.monotonic() + 10.0, hard_stop)
        while time.monotonic() < gstop:
            if any(gtag in ln for ln in log_lines()[tail_r:]):
                break
            time.sleep(0.2)
    # MEASURED, not assumed: the first version of this loop waited for "any new
    # geom line", and the S arm then failed every time while E/W/N passed. The
    # serial log named the reason — the compositor observed only 6 of the 10
    # injected button edges (PRADYOS_BTN_STATE), and the missing pair was S's.
    # A geom republish triggered by an unrelated event (GAMMA closing, in the
    # capture) satisfied the old wait BEFORE the previous arm's resize had even
    # committed, so the S drag was injected while the compositor was still
    # inside the E arm's client round-trip and recompose. SYS_MOUSE_POLL reads
    # current state rather than an event queue (DDR-941), so a press and a
    # release that both fall inside one busy window are not queued — they are
    # simply never seen.
    #
    # So wait for a geom line published AFTER this arm's drags, not merely for
    # one that is new relative to before them.
    tail = len(log_lines())
    tag = "PRADYOS_WM_GEOM id=%s " % sid
    # §13.4: this read a per-arm `deadline` that the budget rework deleted as
    # "dead code" -- it was not; grepping for the assignment found the write and
    # missed this read, and the injector then died with NameError right after
    # arm e. Every later arm silently never ran, which looked exactly like a
    # compositor that had stopped seeing presses (btnedge stuck at 1, preempt
    # flat) and was nearly root-caused as one. Bounded by hard_stop now, so the
    # settle wait can never eat the budget the remaining arms need.
    settle_stop = min(time.monotonic() + arm_to, hard_stop)
    while time.monotonic() < settle_stop:
        if any(tag in ln for ln in log_lines()[tail:]):
            break
        time.sleep(0.1)
    time.sleep(0.4)                             # let the recompose drain

# §13.4: hand the skipped list to the checker.
#
# NOT via stdout. This script's stdout is make's output, and the checker reads
# the GUEST SERIAL log -- two different streams. Printing a sentinel here and
# expecting resize_check.py to see it is precisely the vacuous-scan mistake
# recorded at DDR-1000 §10, where 60 logs were scanned for a sentinel that
# could never have been in them. A sidecar file beside the log is a channel
# both sides actually share.
print("PRADYOS_RESIZE_SKIPPED %s" % (",".join(skipped) if skipped else "none"))
sys.stdout.flush()
try:
    with open(logfile + ".skipped", "w") as fh:
        fh.write("\n".join(skipped))
except OSError:
    pass
PY

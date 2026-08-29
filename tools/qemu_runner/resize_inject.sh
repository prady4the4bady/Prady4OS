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

for _ in $(seq 1 600); do
    grep -q "$sentinel" "$log" 2>/dev/null && break
    sleep 0.1
done
sleep 0.5

SOCK="$sock" LOGFILE="$log" RZ_ID="${RZ_ID:-1}" RZ_ARMS="${RZ_ARMS:-e s w n}" \
GROW_X="${GROW_X:-3200}" GROW_Y="${GROW_Y:-2560}" \
ARM_TIMEOUT_S="${ARM_TIMEOUT_S:-25}" python3 - <<'PY'
import os, socket, json, time, sys

sock    = os.environ["SOCK"]
logfile = os.environ["LOGFILE"]
sid     = os.environ["RZ_ID"]
arms    = os.environ["RZ_ARMS"].split()
grow_x  = int(os.environ["GROW_X"])
grow_y  = int(os.environ["GROW_Y"])
arm_to  = float(os.environ["ARM_TIMEOUT_S"])

# Edge bits — must match user/compositor.c's RZ_N/S/W/E.
RZ_N, RZ_S, RZ_W, RZ_E = 1, 2, 4, 8
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
    stop = time.monotonic() + 6.0
    while time.monotonic() < stop:
        if track_count() > before_track:
            seen = True
            break
        time.sleep(0.1)
    if not seen:
        # Do not release blind: a release the compositor scores at the press
        # point produces a self-consistent but WRONG-DISTANCE commit, which is
        # exactly the CI failure this replaced. Say so and let the arm retry.
        print("[resize_inject] no RESIZE_TRACK within 6s — compositor never "
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

for arm in arms:
    bit = NEED[arm]
    # Every arm needs rzw= and rze= as well as its own handle: those two bracket
    # the window, which is how the over-drag END is derived without a constant.
    need = ["rzw", "rze", "rzn", "rzs"]
    g = newest_geom(need, time.monotonic() + arm_to)
    if g is None:
        print("[resize_inject] FAIL — no PRADYOS_WM_GEOM for id=%s with %s"
              % (sid, " ".join(need)))
        sys.exit(1)
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
    # Three rounds: a missed press phase still lands. Each PRADYOS_RESIZE_FIX
    # line carries its own before/after geometry, so a repeat is a second
    # independent observation rather than a corruption of the first.
    deadline = time.monotonic() + arm_to
    for _round in range(3):
        drag(sx, sy, ex, ey)
        if fix_count(bit) > before_fix:
            break
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
    while time.monotonic() < deadline:
        if any(tag in ln for ln in log_lines()[tail:]):
            break
        time.sleep(0.1)
    time.sleep(0.4)                             # let the recompose drain
PY

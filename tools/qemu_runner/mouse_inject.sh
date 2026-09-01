#!/usr/bin/env bash
# mouse_inject.sh — pointer injector for the Layer-7 pointer gates (DDR-705).
#
# Waits for a readiness sentinel ($3, default PRADYOS_COMPOSITOR_OK) in the
# serial log ($1), then drives the virtio-tablet through QEMU's QMP
# `input-send-event` on the unix socket ($2): an absolute move followed by a
# left-button click — the real virtio-input path. (HMP has no portable
# absolute-pointer command; QMP does.)
#
# DDR-910: THIS INJECTOR POLLS FOR THE OUTCOME. It used to settle for a fixed
# 0.5s after the readiness sentinel and then fire exactly five clicks. If the
# compositor had not yet been scheduled to service input, all five landed on the
# floor and no further click was ever sent — the gate then ran out its clock and
# reported "click did not close", which is indistinguishable from a broken close
# box. That is what bisected to item 16: a scheduler change that added input
# latency could not be told apart from a functional regression.
#
# $4 (optional) is the pattern that proves the click was ACTED ON. When given,
# the injector re-clicks until that pattern appears in the log or INJECT_TIMEOUT_S
# expires, and exits the moment it is observed. A gate that passes this argument
# can no longer fail merely because the guest was slow — only because it never
# responded at all.
set -u
log="$1"
sock="$2"
sentinel="${3:-PRADYOS_COMPOSITOR_OK}"
expect="${4:-}"

# Readiness: poll, never a fixed wait. 60s ceiling.
for _ in $(seq 1 600); do
    grep -q "$sentinel" "$log" 2>/dev/null && break
    sleep 0.1
done

SOCK="$sock" LOGFILE="$log" EXPECT="$expect" \
GEOM_TITLE="${GEOM_TITLE:-}" GEOM_FIELD="${GEOM_FIELD:-close}" \
GEOM_LINE="${GEOM_LINE:-PRADYOS_WM_GEOM}" \
ABSX="${ABSX:-16000}" ABSY="${ABSY:-12000}" \
INJECT_TIMEOUT_S="${INJECT_TIMEOUT_S:-60}" python3 - <<'PY'
import os, socket, json, time

sock     = os.environ["SOCK"]
logfile  = os.environ["LOGFILE"]
expect   = os.environ.get("EXPECT", "")
ax       = int(os.environ.get("ABSX", "16000"))
ay       = int(os.environ.get("ABSY", "12000"))
geom_title = os.environ.get("GEOM_TITLE", "")
geom_field = os.environ.get("GEOM_FIELD", "close")
# DDR-1008: which published line carries the geometry. Defaults to the literal
# this resolver used to hardcode, so every existing caller is unchanged; the dock
# publishes PRADYOS_WM_DOCK instead, with the same `title=` / `field=x,y` shape.
geom_line = os.environ.get("GEOM_LINE", "PRADYOS_WM_GEOM")
deadline = time.monotonic() + float(os.environ.get("INJECT_TIMEOUT_S", "60"))

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
    raise SystemExit(0)

f = s.makefile("rw", newline="\n")
f.readline()                                   # QMP greeting


def cmd(obj):
    f.write(json.dumps(obj) + "\n")
    f.flush()
    f.readline()                               # response (ignored)


cmd({"execute": "qmp_capabilities"})


def resolve_geometry():
    """DDR-910: take the click target from what the compositor SAYS it drew.

    The gate used to hardcode pixels, which silently encoded the window creation
    order. Fair-share scheduling is free to change that order, and when item 16
    did, every click landed on empty space while the input path worked perfectly
    (23x PRADYOS_MOUSE_OK, zero PRADYOS_WM_CLOSE). Reading the emitted geometry
    removes the assumption instead of re-tuning it.
    """
    global ax, ay
    if not geom_title:
        return True
    try:
        with open(logfile, "r", errors="replace") as fh:
            lines = fh.read().splitlines()
    except OSError:
        return False
    for ln in reversed(lines):                 # newest wins: layout can change
        if ("title=" + geom_title) not in ln:
            continue
        # DDR-1036: the newest record for this title decides, over BOTH record
        # types. The serial log is append-only, so a destroyed window's last
        # PRADYOS_WM_GEOM is still present and says nothing about being gone --
        # which is how this script came to click a dead window 45 times in one
        # capture (DDR-1028), and how smoke-wmclose came to report "close box
        # click did not close" about a window that no longer existed.
        #
        # Returning False rather than raising: the caller's wait loop already
        # treats False as "not ready yet" and retries to its deadline, so a
        # window that is about to be RECREATED is waited for (recycled slots
        # emit GONE then a fresh GEOM, and newest-wins then reports live), while
        # one that never returns times out with the existing [inject] TIMEOUT
        # message instead of silently clicking empty space.
        if "PRADYOS_WM_GONE" in ln:
            print("[inject] target gone title=%s — not clicking" % geom_title)
            return False
        if geom_line not in ln:
            continue
        for tok in ln.split():
            if tok.startswith(geom_field + "="):
                try:
                    nx, ny = tok.split("=", 1)[1].split(",")
                    ax, ay = int(nx), int(ny)
                except ValueError:
                    return False
                print("[inject] geometry for %s: %s=%d,%d"
                      % (geom_title, geom_field, ax, ay))
                return True
    return False


def observed():
    """Has the guest acted on the click yet? Empty pattern == nothing to wait for."""
    if not expect:
        return False
    try:
        with open(logfile, "r", errors="replace") as fh:
            return expect in fh.read()
    except OSError:
        return False


def click():
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]}})
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"button": "left", "down": True}},
    ]}})
    time.sleep(0.2)
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"button": "left", "down": False}},
    ]}})


# Resolve the target BEFORE the first click. Never fall back to a default pixel:
# clicking a guessed coordinate is precisely the defect being removed here.
if geom_title:
    while not resolve_geometry():
        if time.monotonic() >= deadline:
            print("[inject] TIMEOUT — no %s for title=%s"
                  % (geom_line, geom_title))
            raise SystemExit(0)
        time.sleep(0.2)

rounds = 0
while True:
    # DDR-983: RE-RESOLVE before every click, not just once.
    #
    # Resolving once is correct only for a target that never moves. smoke-wmmax's
    # second injection targets the max box AFTER the window has been maximized
    # and relocated to (8,26) at 512x512 — a different place entirely. Its
    # readiness sentinel (the client's PRADYOS_EV_RESIZE_OK) fires when the
    # CLIENT acks the resize, which is before the compositor has necessarily
    # re-composited and re-emitted PRADYOS_WM_GEOM for the moved window. A
    # resolve at that instant returns the PRE-move coordinates and every
    # subsequent retry re-clicks that same stale point: measured, the gate
    # failed "restore click did not un-maximize" with both injectors reporting
    # the identical mx=5317,5596.
    #
    # resolve_geometry() already takes the NEWEST matching line ("newest wins:
    # layout can change"), so re-running it per round is exactly what that
    # comment anticipated — the caller was the part that assumed a static
    # target. A failed re-resolve keeps the previous coordinates rather than
    # falling back to a default pixel, preserving the no-guessed-coordinate
    # property established above.
    if geom_title:
        resolve_geometry()
    click()
    rounds += 1

    # Poll for the outcome in short slices, so a fast response returns fast and
    # a slow one is still caught rather than being raced past.
    for _ in range(10):
        if observed():
            print("[inject] observed %r after %d click(s)" % (expect, rounds))
            raise SystemExit(0)
        time.sleep(0.1)

    if time.monotonic() >= deadline:
        if expect:
            # Genuine non-response. Say so plainly: this is a real failure, not
            # a margin, because the click was retried for the whole window.
            print("[inject] TIMEOUT — %r never appeared after %d click(s)"
                  % (expect, rounds))
        raise SystemExit(0)

    if not expect and rounds >= 5:
        # No outcome pattern supplied: preserve the historical bounded behaviour
        # for callers that assert separately.
        raise SystemExit(0)
PY

#!/usr/bin/env bash
# drag_inject.sh — window-drag injector for the smoke-drag gate (DDR-710). Waits
# for a readiness sentinel ($3) in the serial log ($1), then drives a drag via
# QEMU QMP input-send-event on the unix socket ($2): move onto window B's title
# bar, press, drag to a new location (button held), release.
set -u
log="$1"
sock="$2"
sentinel="${3:-PRADYOS_AMBIANCE_OK}"

for _ in $(seq 1 600); do
    grep -q "$sentinel" "$log" 2>/dev/null && break
    sleep 0.1
done
sleep 0.5

# DDR-894: OBSERVE the resize corner instead of assuming it. When RZ_ID is set,
# wait for the compositor's own PRADYOS_WM_GEOM line for that surface and take
# its `rz=X,Y` (the centre of the DDR-718 14x14 corner, already in tablet
# coordinates) as the drag START. The 14-pixel target moves whenever the window
# does, which is why hardcoded SX/SY made smoke-evresize flake.
# The drag END stays a relative offset from the observed start (RZ_DX/RZ_DY), so
# the drag distance is preserved without reintroducing an absolute assumption.
# DDR-897: same mechanism for the TITLE-BAR drag. DG_ID selects the surface and
# the start point comes from that surface's published dg= (a point on the title
# bar, left of the close/min/max boxes). smoke-drag previously hardcoded
# SX/SY and failed 0/3 locally; A/B against the pre-DDR-894 script confirmed the
# flake predates the rz= work rather than being caused by it.
if [ -n "${DG_ID:-}" ]; then
    geom=""
    for _ in $(seq 1 600); do
        geom=$(grep -a "PRADYOS_WM_GEOM id=${DG_ID} " "$log" 2>/dev/null | grep -a "dg=" | tail -1)
        [ -n "$geom" ] && break
        sleep 0.1
    done
    if [ -z "$geom" ]; then
        echo "[drag_inject] FAIL — no complete PRADYOS_WM_GEOM (with dg=) for id=${DG_ID}"
        exit 1
    fi
    # DDR-935: isolate the FIELD first (cut at the first space), THEN split on
    # the comma. Using ${x##*,} on the whole tail takes everything after the LAST
    # comma in the line, so any field appended after this one silently steals Y.
    dg=${geom##*dg=}
    dg=${dg%% *}
    SX=${dg%%,*}
    SY=${dg##*,}
    case "$SX$SY" in
        ''|*[!0-9]*)
            echo "[drag_inject] FAIL — malformed dg in: $geom"
            exit 1 ;;
    esac
    export SX SY
    export EX="$(( SX + ${DG_DX:-8000} ))"
    export EY="$(( SY + ${DG_DY:-9387} ))"
    echo "[drag_inject] DDR-897: observed dg for id=${DG_ID}: start=${SX},${SY} end=${EX},${EY}"
fi

if [ -n "${RZ_ID:-}" ]; then
    # DDR-997: which handle. Default `rz` = the DDR-718 SE corner, so
    # smoke-evresize is untouched. The seven new handles (rzn rzs rzw rze rznw
    # rzne rzsw) are published by the same compositor expressions the hit-test
    # uses. Every one of these names is a UNIQUE substring of the geometry line:
    # none of them contains another as a substring, and none contains "rz=", so
    # the ${geom##*FIELD=} parse below still isolates exactly one field.
    rzf="${RZ_FIELD:-rz}"
    # Require the line to actually CARRY rz=. Serial output from other threads
    # interleaves mid-line in this tree, so a matching-but-truncated
    # "PRADYOS_WM_GEOM id=1 …" is a real possibility; taking tail -1 blindly
    # then yields a non-numeric SX, and $(( SX + … )) makes bash evaluate the
    # log text as a variable name (observed: "PRADYOS_WM_GEOM: unbound variable").
    geom=""
    for _ in $(seq 1 600); do
        geom=$(grep -a "PRADYOS_WM_GEOM id=${RZ_ID} " "$log" 2>/dev/null | grep -a "${rzf}=" | tail -1)
        [ -n "$geom" ] && break
        sleep 0.1
    done
    if [ -z "$geom" ]; then
        echo "[drag_inject] FAIL — no complete PRADYOS_WM_GEOM (with ${rzf}=) for id=${RZ_ID}"
        exit 1
    fi
    # DDR-935: same field-isolation fix as the dg= parse above. This one was
    # ACTIVELY BROKEN by DDR-929 appending dg= after rz= on the same line:
    # SY=${rz##*,} then returned dg's Y (5596 instead of 8416), so the resize
    # click landed on the wrong row and missed the 14-pixel corner.
    rz=${geom##*${rzf}=}
    rz=${rz%% *}
    SX=${rz%%,*}
    SY=${rz##*,}
    # Validate before arithmetic: a partially-interleaved line must fail loudly
    # here, not as a confusing bash arithmetic error 20 lines later.
    case "$SX$SY" in
        ''|*[!0-9]*)
            echo "[drag_inject] FAIL — malformed ${rzf} in: $geom"
            exit 1 ;;
    esac
    export SX SY
    export EX="$(( SX + ${RZ_DX:-3296} ))"
    export EY="$(( SY + ${RZ_DY:-2686} ))"
    echo "[drag_inject] DDR-894: observed ${rzf} for id=${RZ_ID}: start=${SX},${SY} end=${EX},${EY}"
fi

# Optional SX/SY/EX/EY abs-coordinate overrides (DDR-718); defaults = the
# DDR-710 title-bar drag, so smoke-drag is untouched.
# Default start = pixel (150,130) on B's title-bar DRAG region (DDR-719 moved
# it left of the max box at pixel >=160; three boxes now occupy x+20..x+64).
SOCK="$sock" SX="${SX:-4800}" SY="${SY:-5546}" EX="${EX:-12800}" EY="${EY:-14933}" python3 - <<'PY'
import os, socket, json, time
sock = os.environ["SOCK"]
sx, sy = int(os.environ["SX"]), int(os.environ["SY"])
ex, ey = int(os.environ["EX"]), int(os.environ["EY"])
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

cmd({"execute": "qmp_capabilities"})
# Window B's title bar is around pixel (160,130) -> abs (5120,5546); drag target
# pixel (400,350) -> abs (12800,14933). Repeat so a missed phase still lands.
for _round in range(3):
    absmove(sx, sy);      time.sleep(0.35)      # over the drag start point
    btn(True);            time.sleep(0.45)       # press (start drag)
    absmove(ex, ey);      time.sleep(0.45)       # drag (button held)
    btn(False);           time.sleep(0.6)        # release (drop)
PY

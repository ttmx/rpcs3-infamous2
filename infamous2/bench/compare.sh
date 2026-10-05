#!/bin/bash
# compare.sh <label> <stock|stock-nostrict|play> <pier|dock|city|street> [arms]: boot a scene's savestate with RPCS3's
# own paths (stock; stock-nostrict also turns Strict Rendering Mode off) or with the launcher's settings (play), at the normal 60 FPS cap, and print FPS, power and GPU clock over
# arms of 10 s (default 3). 'street' is the city state with the camera turned about 60 degrees right; that turn does
# not land on exactly the same view every boot, so check sessions/<label>/view.png before comparing two boots.
# UNCAPPED=1 turns the frame limit off and sets the vblank rate to 120 (the game syncs to vblank): a benchmark
# setting to see headroom above 60 FPS, not a way to play.
cd "$(dirname "$0")" || exit 1
label="$1"; flags="$2"; scene="$3"; arms="${4:-3}"
case "$scene" in pier) state=pier-kb ;; dock) state=dock ;; city|street) state=city ;; *) echo "unknown scene"; exit 1 ;; esac
extra=(); [ "$flags" = stock-nostrict ] && extra=("CFG:Strict Rendering Mode='false'")
[ -n "$UNCAPPED" ] && extra+=("CFG:Frame limit='Off'" "CFG:Vblank Rate='120'")
export LIVE_TEST_FLAGS="${flags%%-*}" LIVE_TEST_BOOT="$PWD/states/$state.SAVESTAT.zst" LIVE_TEST_CACHE="${LIVE_TEST_CACHE:-cache-geom}"
python3 live_test.py launch "sessions/$label" "${extra[@]}" >/dev/null || exit 1
t=0; until python3 live_test.py status 2>/dev/null | grep -q 'FPS: [1-9]' || [ $t -ge 600 ]; do python3 live_test.py accept; sleep 1; t=$((t+1)); done
python3 live_test.py status 2>/dev/null | grep -q 'FPS: [1-9]' || { echo 'game did not start'; python3 live_test.py stop; exit 1; }
sleep "${SETTLE:-45}"
[ "$scene" = street ] && { python3 live_test.py key Next 0.30; sleep 6; }
python3 live_test.py shot "sessions/$label/view.png"
for i in $(seq 1 "$arms"); do
  python3 run.py fps 10 "$label $flags $scene" | python3 -c 'import json,sys;d=json.loads(sys.stdin.read());print("'"$flags $scene"': fps",d["mean"],"min",d["min"],"W",d["W"],"gpuMHz",d["gpuMHz"],"cpuC",d["cpuC"])'
done | tee "sessions/$label/compare.txt"
python3 live_test.py shot "sessions/$label/view-after.png"
python3 live_test.py stop >/dev/null

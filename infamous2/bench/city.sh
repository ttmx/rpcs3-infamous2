#!/bin/bash
# city.sh <label> [NAME=VALUE | CFG:setting=value ...]: boot the city savestate on the isolated profile with the
# play settings and wait until the game renders (GPU occlusion pass active). Sessions live in sessions/<label>.
# STATE=<name of a file in states/> picks another state (default city). Stop with: python3 live_test.py stop
cd "$(dirname "$0")" || exit 1
label="$1"; shift
export LIVE_TEST_BOOT="$PWD/states/${STATE:-city}.SAVESTAT.zst" LIVE_TEST_CACHE=cache-geom
log=cache-geom/rpcs3/RPCS3.log; rm -f "$log"
python3 live_test.py launch "sessions/$label" RPCS3_NATIVE_LIGHTING=61 "$@" >/dev/null || exit 1
t=0; until grep -aq 'samples the GPU occlusion image' "$log" 2>/dev/null || ! pgrep -x rpcs3 >/dev/null || [ $t -ge 400 ]; do sleep 0.5; t=$((t+1)); done
grep -aq 'samples the GPU occlusion image' "$log" 2>/dev/null || { echo 'game did not start rendering'; python3 live_test.py stop; exit 1; }
echo "running pid $(pgrep -x rpcs3) after $((t/2)) s"

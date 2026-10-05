#!/bin/bash
# replace_bench.sh <label> <RPCS3_NATIVE_LIGHTING mode>: boot the dock state on the isolated profile, let it settle,
# then report title FPS, power and emulator CPU use standing still and while firing lightning.
cd "$(dirname "$0")" || exit 1
label="$1"; mode="$2"; shift 2
export LIVE_TEST_CACHE=cache-bench; log=cache-bench/rpcs3/RPCS3.log
rm -f "$log"
python3 live_test.py launch "sessions/$label" RPCS3_NATIVE_LIGHTING="$mode" "$@" >/dev/null || exit 1
t=0; until grep -aq 'samples the GPU occlusion image' "$log" 2>/dev/null || ! pgrep -x rpcs3 >/dev/null || [ $t -ge 240 ]; do sleep 0.5; t=$((t+1)); done
grep -aq 'samples the GPU occlusion image' "$log" 2>/dev/null || { echo 'GPU passes did not start'; python3 live_test.py stop; exit 1; }
pid=$(pgrep -x rpcs3) || { echo "emulator exited"; exit 1; }
sleep 15
measure() {
  t0=$(awk '{print $14+$15}' /proc/$pid/stat); s0=$(date +%s.%N)
  out=$(python3 run.py fps "$1" "$label $2")
  t1=$(awk '{print $14+$15}' /proc/$pid/stat); s1=$(date +%s.%N)
  echo "$2: $(echo "$out" | python3 -c 'import json,sys;d=json.loads(sys.stdin.read());print("fps mean",d["mean"],"min",d["min"],"W",d["W"],"gpuMHz",d["gpuMHz"])') cores $(echo "($t1-$t0)/100/($s1-$s0)" | bc -l | cut -c1-5)"
}
measure 20 still
( for i in $(seq 1 14); do python3 live_test.py key q,e 0.5; python3 live_test.py key Next 0.4; done ) &
measure 14 lightning
wait
python3 live_test.py shot "sessions/$label/shot.png"
grep -a 'NativeLighting' "$log" | tail -3 | cut -c1-200
python3 live_test.py stop >/dev/null

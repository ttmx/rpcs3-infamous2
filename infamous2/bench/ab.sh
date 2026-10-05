#!/bin/bash
# ab.sh <label> <reps> <seconds> <mode A> <mode B> ...: in the running test emulator, switch live control 8 (geometry cache)
# between the given modes, interleaved, and print title FPS, power, GPU clock and emulator CPU use for each arm.
cd "$(dirname "$0")" || exit 1
label="$1"; reps="$2"; secs="$3"; shift 3
pid=$(pgrep -x rpcs3) || { echo "no emulator"; exit 1; }
rsx=$(for t in /proc/$pid/task/*; do grep -qx 'rsx::thread' $t/comm 2>/dev/null && basename $t; done | head -1)
for rep in $(seq 1 "$reps"); do for m in "$@"; do
  echo "1000 65536 1 0 0 0 0 0 $m 0" | tr "," " " | cut -d" " -f1-10 > live.ctl; sleep 3.5
  t0=$(awk '{print $14+$15}' /proc/$pid/stat); r0=$(awk '{print $14+$15}' /proc/$pid/task/$rsx/stat); s0=$(date +%s.%N)
  out=$(python3 run.py fps "$secs" "$label cache=$m")
  t1=$(awk '{print $14+$15}' /proc/$pid/stat); r1=$(awk '{print $14+$15}' /proc/$pid/task/$rsx/stat); s1=$(date +%s.%N)
  echo "cache=$m $(echo "$out" | python3 -c 'import json,sys;d=json.loads(sys.stdin.read());print("fps",d["mean"],"min",d["min"],"W",d["W"],"gpuMHz",d["gpuMHz"],"cpuC",d["cpuC"])') cores $(echo "($t1-$t0)/100/($s1-$s0)" | bc -l | cut -c1-5) rsx $(echo "($r1-$r0)/100/($s1-$s0)" | bc -l | cut -c1-5)"
done; done

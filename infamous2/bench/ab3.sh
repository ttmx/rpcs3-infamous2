#!/bin/bash
# ab3.sh <label> <reps> <seconds> <"values of live controls 8, 9, 10, ..."> ...: like ab.sh, each arm sets the controls from 8 upward
cd "$(dirname "$0")" || exit 1
label="$1"; reps="$2"; secs="$3"; shift 3
pid=$(pgrep -x rpcs3) || { echo "no emulator"; exit 1; }
busy=$(ls /sys/class/drm/card[0-9]*/device/gpu_busy_percent 2>/dev/null | head -1); stop=$(mktemp -u)
rsx=$(for t in /proc/$pid/task/*; do grep -qx 'rsx::thread' $t/comm 2>/dev/null && basename $t; done | head -1)
for rep in $(seq 1 "$reps"); do for m in "$@"; do
  echo "1000 65536 1 0 0 0 0 0 $m" > live.ctl; sleep 3.5
  r0=$(awk '{print $14+$15}' /proc/$pid/task/$rsx/stat); s0=$(date +%s.%N)
  ( n=0; t=0; while [ -e /proc/$pid ] && [ ! -e "$stop" ]; do t=$((t+$(cat "$busy" 2>/dev/null || echo 0))); n=$((n+1)); sleep 0.2; done; echo $((t/(n>0?n:1))) > "$stop.busy" ) &
  out=$(python3 run.py fps "$secs" "$label $m")
  touch "$stop"; wait; rm -f "$stop"
  r1=$(awk '{print $14+$15}' /proc/$pid/task/$rsx/stat); s1=$(date +%s.%N)
  echo "ctl=$m $(echo "$out" | python3 -c 'import json,sys;d=json.loads(sys.stdin.read());print("fps",d["mean"],"min",d["min"],"W",d["W"],"gpuMHz",d["gpuMHz"],"cpuC",d["cpuC"])') gpuBusy $(cat "$stop.busy"; rm -f "$stop.busy") rsx $(echo "($r1-$r0)/100/($s1-$s0)" | bc -l | cut -c1-5)"
done; done

#!/bin/bash
# particles.sh <label> <reps> <seconds> "<live controls 13 14>" ...: boot the dock state uncapped with the launcher's
# settings and switch, inside one boot, the particle target's scale in percent (13) and the old depth buffer path (14 = 1).
# RES=<resolution scale>, FIRE=1 fires lightning during the arms, EXTRA='NAME=VALUE "CFG:setting=value"' is passed on.
cd "$(dirname "$0")" || exit 1
label="$1"; reps="$2"; secs="$3"; shift 3
# A build directory next to the checkout is used when launch.py finds no build inside it
[ -z "$LIVE_TEST_BINARY" ] && [ -x ../../../build/bin/rpcs3 ] && export LIVE_TEST_BINARY="$PWD/../../../build/bin/rpcs3"
export LIVE_TEST_FLAGS=play LIVE_TEST_BOOT="$PWD/states/${STATE:-dock}.SAVESTAT.zst" LIVE_TEST_CACHE="${LIVE_TEST_CACHE:-cache-geom}"
eval "extra=($EXTRA)"
python3 live_test.py launch "sessions/$label" "CFG:Frame limit='Off'" "CFG:Vblank Rate='120'" "CFG:Resolution Scale='${RES:-100}'" "${extra[@]}" >/dev/null || exit 1
t=0; until python3 live_test.py status 2>/dev/null | grep -q 'FPS: [1-9]' || [ $t -ge 600 ]; do python3 live_test.py accept; sleep 1; t=$((t+1)); done
python3 live_test.py status 2>/dev/null | grep -q 'FPS: [1-9]' || { echo 'game did not start'; python3 live_test.py stop; exit 1; }
sleep "${SETTLE:-40}"
[ -n "$PRE" ] && eval "$PRE"
pid=$(pgrep -x rpcs3); busy=$(ls /sys/class/drm/card[0-9]*/device/gpu_busy_percent 2>/dev/null | head -1); stop=$(mktemp -u)
rsx=$(for t in /proc/$pid/task/*; do grep -qx 'rsx::thread' $t/comm 2>/dev/null && basename $t; done | head -1)
for rep in $(seq 1 "$reps"); do for m in "$@"; do
  c=($m); echo "1000 65536 1 0 0 0 0 0 1 6 0 1 1 ${c[0]} ${c[1]:-0}" > live.ctl; sleep 3.5
  [ -n "$FIRE" ] && ( for i in $(seq 1 $((secs*10/11))); do python3 live_test.py key q,e 0.6; done ) &
  [ -n "$FIRE" ] && sleep 1.5
  [ "$rep" = 1 ] && python3 live_test.py shot "sessions/$label/scale-$m.png"
  r0=$(awk '{print $14+$15}' /proc/$pid/task/$rsx/stat); s0=$(date +%s.%N)
  ( n=0; t=0; while [ -e /proc/$pid ] && [ ! -e "$stop" ]; do t=$((t+$(cat "$busy" 2>/dev/null || echo 0))); n=$((n+1)); sleep 0.2; done; echo $((t/(n>0?n:1))) > "$stop.busy" ) &
  out=$(python3 run.py fps "$secs" "$label $m")
  touch "$stop"; wait; rm -f "$stop"
  r1=$(awk '{print $14+$15}' /proc/$pid/task/$rsx/stat); s1=$(date +%s.%N)
  echo "scale=$m $(echo "$out" | python3 -c 'import json,sys;d=json.loads(sys.stdin.read());print("fps",d["mean"],"min",d["min"],"W",d["W"],"gpuMHz",d["gpuMHz"])') gpuBusy $(cat "$stop.busy"; rm -f "$stop.busy") rsx $(echo "($r1-$r0)/100/($s1-$s0)" | bc -l | cut -c1-5)"
done; done | tee "sessions/$label/particles.txt"
[ -n "$KEEP" ] || python3 live_test.py stop >/dev/null

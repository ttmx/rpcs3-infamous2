#!/bin/bash
# lightning_shots.sh <label> <NAME=VALUE ...>: boot the dock state, fire lightning without moving the camera and take
# four screenshots while firing (<label>/fire-N.png) and one before (<label>/still.png).
cd "$(dirname "$0")" || exit 1
label="$1"; shift
export LIVE_TEST_CACHE=cache-bench; log=cache-bench/rpcs3/RPCS3.log
rm -f "$log"
python3 live_test.py launch "sessions/$label" "$@" >/dev/null || exit 1
t=0; until python3 live_test.py status 2>/dev/null | grep -q 'FPS: [1-9]' || [ $t -ge 240 ]; do sleep 0.5; t=$((t+1)); done
sleep 15
python3 live_test.py shot "sessions/$label/still.png"
( for i in $(seq 1 12); do python3 live_test.py key q,e 0.6; done ) &
for n in 1 2 3 4; do sleep 1.6; python3 live_test.py shot "sessions/$label/fire-$n.png"; done
wait
python3 live_test.py status | grep title
grep -a 'Native' "$log" | grep -v "Native UI" | tail -3 | cut -c1-200
python3 live_test.py stop >/dev/null

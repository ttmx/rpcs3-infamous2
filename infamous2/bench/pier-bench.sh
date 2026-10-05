#!/bin/bash
# pier-bench.sh <binary-dir> <cfg.yml> <label> "<ctl A>" "<ctl B>" ... : relaunch with keyboard input, load autosave
# (pier start), then measure each control-file setting twice, interleaved.
cd "$(dirname "$0")" || exit 1
bin="$1"; cfg="$2"; label="$3"; shift 3
python3 run.py stop >/dev/null; sleep 2
echo "$1" > live.ctl
python3 run.py launch "$bin/rpcs3" --kb --cfg "$cfg" RPCS3_VK_LIVE_CTL=$PWD/live.ctl $EXTRA_ENV >/dev/null
until python3 run.py win | grep -q FPS; do sleep 2; done
sleep 15
for i in 1 2 3 4; do python3 run.py key x; sleep 3; python3 run.py key Return; sleep 3; done
sleep 30
python3 ready.py || exit 1
sleep 5
python3 run.py shot "ready-$label.png"
for rep in 1 2; do for v in "$@"; do echo "$v" > live.ctl; sleep 4; python3 run.py fps 12 "$label ctl=$v" | cut -c1-110; done; done
echo "$1" > live.ctl

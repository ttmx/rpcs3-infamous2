#!/bin/bash
# dock-bench.sh <binary-dir> <cfg.yml> <label> "<ctl A>" ... : boot the dock savestate (keyboard input, fast config),
# wait for gameplay, then measure each live-control setting twice, interleaved. GPU=high|auto|floor-<MHz> sets the
# GPU clock policy first through the helper in ../hardware (GPU_HELPER, default /usr/local/bin/rpcs3-gpu-perf); unset leaves it alone.
# EXTRA_ENV adds/overrides environment flags. STATE picks another state file.
cd "$(dirname "$0")" || exit 1
bin="$1"; cfg="$2"; label="$3"; shift 3
python3 run.py stop >/dev/null; sleep 2
[ -n "$GPU" ] && sudo -n "${GPU_HELPER:-/usr/local/bin/rpcs3-gpu-perf}" "$GPU"
echo "$1" > live.ctl
python3 run.py launch "$bin/rpcs3" --kb --cfg "$cfg" --boot "${STATE:-states/dock.SAVESTAT.zst}" RPCS3_VK_LIVE_CTL=$PWD/live.ctl \
  RPCS3_EXPERIMENT_BLIT_COMPLEMENT=1 RPCS3_FIFO_INLINE_CACHE=1 RPCS3_EXPERIMENT_LAST_IMAGE_VIEW=1 $EXTRA_ENV >/dev/null
ok=0
for i in $(seq 1 40); do
  sleep 3
  python3 run.py shot ready-probe.png 2>/dev/null || continue
  rgb=$(magick ready-probe.png -crop 50x50+815+455 -resize '1x1!' -format '%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]' info:)
  set -- $rgb "$@"; r=$1; g=$2; b=$3; shift 3
  if [ "$b" -gt $((r+20)) ] && [ "$b" -gt 50 ] && [ "$r" -lt 60 ]; then ok=1; break; fi
done
[ $ok = 1 ] || { echo "NOT READY"; exit 1; }
sleep ${SETTLE:-12}
cp ready-probe.png "ready-$label.png"
for rep in 1 2; do for v in "$@"; do echo "$v" > live.ctl; sleep 4; python3 run.py fps ${SECS:-10} "$label gpu=${GPU:-unchanged} ctl=$v" | cut -c1-${CUT:-175}; done; done
echo "$1" > live.ctl

#!/bin/bash
# fight.sh go <session> [gw.py launch arguments]      boot the menu state, load the autosave (first fight on Gaia), hold block
# fight.sh run <session> <seconds> [launch arguments]  the same, then measure and stop: one result line
# FIGHT_STATE=menu6 with RPCS3_SPU_EXTRA_THREADS=1 RPCS3_SPURS_EXTRA_WORKLOADS=1c00 is the six-SPU boot (tools/sixboot.sh).
# The game cannot be savestated in gameplay (HLE video decoder contexts), so the fight is reached through the menu.
cd "$(dirname "$0")/.."
cmd=$1; s=$2; shift 2
if [ "$cmd" = run ]; then secs=$1; shift; fi
# a key holder left over from an earlier session would release block in this one
pgrep -x python3 -a | grep "[g]w.py key" | awk '{print $1}' | xargs -r kill 2>/dev/null
python3 tools/gw.py go "$s" "${FIGHT_STATE:-menu}" "$@" | tail -1 >/dev/null || exit 1
python3 tools/gw.py seq "wait:2 Down:0.04 wait:1.5 x:0.06 wait:5 x:0.06 wait:5 x:0.06 wait:22"
(python3 tools/gw.py key q 600 >/dev/null 2>&1 &)
python3 tools/gw.py seq "wait:3"
if [ "$cmd" = run ]; then
  python3 tools/gw.py fps "$secs" "$s" | python3 -I -c "import sys,json; r=json.loads(sys.stdin.read()); print(r['label'],'| fps',r['fps'],'min',r['min'],'W',r['W'],'gpuMHz',r['gpuMHz'],'rsx',r['rsx_cores'],'spu',r['spu_cores'],'ppu',r['ppu_cores'])"
  python3 tools/gw.py stop >/dev/null
  pgrep -x rpcs3 | xargs -r kill -9
fi

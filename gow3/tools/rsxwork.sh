#!/bin/bash
# rsxwork.sh <seconds> [launch arguments]: one six-SPU fight session with the sampler: frame rate and the render
# thread's working time per frame (it spins on the game's jump to self the rest of the time). For settings that
# cannot be switched live; the working time varies less between sessions than the frame rate.
cd "$(dirname "$0")/.."
secs=$1; shift
rm -f /tmp/rw /tmp/rw.*
FIGHT_STATE=${FIGHT_STATE:-menu6} timeout 500 tools/fight.sh go rw --defaults --cfg=test RPCS3_SPU_EXTRA_THREADS=1 RPCS3_SPURS_EXTRA_WORKLOADS=1c00 RPCS3_SPU_SAMPLER=/tmp/rw "$@" >/dev/null 2>&1
if grep -aq "·F" cache/rpcs3/RPCS3.log; then grep -a "·F" cache/rpcs3/RPCS3.log | head -2 | cut -c1-200; python3 tools/gw.py stop >/dev/null; pgrep -x rpcs3 | xargs -r kill -9; exit 1; fi
python3 tools/gw.py seq "wait:35"; python3 tools/gw.py revive >/dev/null; python3 tools/gw.py seq "wait:3"
a=$(cat /tmp/rw.rsx)
f=$(python3 tools/gw.py fps "$secs" rw | python3 -I -c "import sys,json; r=json.loads(sys.stdin.read()); print(r['fps'], r['rsx_cores'], r['spu_cores'], r['W'])")
b=$(cat /tmp/rw.rsx)
python3 tools/gw.py stop >/dev/null; pgrep -x rpcs3 | xargs -r kill -9; pgrep -x python3 -a | grep "[g]w.py key" | awk '{print $1}' | xargs -r kill
python3 -I -c "
a=list(map(int,'$a'.split())); b=list(map(int,'$b'.split())); f='$f'.split(); fps=float(f[0])
run=b[0]-a[0]; tot=sum(b)-sum(a)
print('$*', '| fps', fps, 'rsx cores', f[1], 'spu', f[2], 'W', f[3], '| render thread working %.2f ms per frame (%.0f%%)' % (1000/fps*run/tot, 100*run/tot))"

#!/bin/bash
# statepair.sh <pairs> <seconds> <state A> <state B>: alternate fight sessions of two six-SPU menu states
cd "$(dirname "$0")/.."
for i in $(seq "$1"); do
  for st in "$3" "$4"; do
    FIGHT_STATE=$st tools/fight.sh go pair --defaults --cfg=test RPCS3_SPU_EXTRA_THREADS=1 RPCS3_SPURS_EXTRA_WORKLOADS=1c00 $PAIR_ARGS >/dev/null 2>&1
    python3 tools/gw.py seq "wait:${PAIR_SETTLE:-40}"; python3 tools/gw.py revive >/dev/null
    python3 tools/gw.py fps "$2" $st | python3 -I -c "import sys,json; r=json.loads(sys.stdin.read()); print(r['label'],'fps',r['fps'],'min',r['min'],'W',r['W'],'rsx',r['rsx_cores'],'spu',r['spu_cores'],'ppu',r['ppu_cores'])"
    python3 tools/gw.py stop >/dev/null; pgrep -x rpcs3 | xargs -r kill -9; pgrep -x python3 -a | grep "[g]w.py key" | awk '{print $1}' | xargs -r kill
  done
done

#!/bin/bash
# cold.sh <session> [launch arguments]: boot the game itself (no savestate) and wait for the first fight, which the
# opening leads into after about five minutes; then hold block. For changes that act at start-up (game patches).
cd "$(dirname "$0")/.."
s=$1; shift
pgrep -x python3 -a | grep "[g]w.py key" | awk '{print $1}' | xargs -r kill 2>/dev/null
python3 tools/gw.py launch "$s" --defaults --cfg=test "$@" >/dev/null 2>&1
for i in $(seq 60); do
  # the fight: the SPU threads and the main thread busy (the opening's video alone can load the SPU threads)
  r=$(python3 tools/gw.py fps 8 cold 2>/dev/null | python3 -I -c "import sys,json; d=json.loads(sys.stdin.read()); print(d['spu_cores'] if d['ppu_cores'] > 0.7 else 0)" 2>/dev/null)
  python3 tools/gw.py seq "x:0.06" 2>/dev/null
  [ -n "$r" ] && python3 -I -c "import sys; sys.exit(0 if float('$r') > 4.6 else 1)" && break
done
(python3 tools/gw.py key q 600 >/dev/null 2>&1 &)
python3 tools/gw.py seq "wait:8"

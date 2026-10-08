#!/bin/bash
# abgeom.sh [pairs] [extra launch arguments]: the fight with the host geometry kernels switched off and on inside one
# session (live control 16), 3 seconds each; prints the pairs inside the fight and their means
cd "$(dirname "$0")/.."
pairs=${1:-8}; shift
tools/fight.sh go abg --defaults --cfg=test "$@" >/dev/null 2>&1
ON="1000 65536 1 0 0 0 0 0 1 2 0 1 1 0 0 0"
timeout 400 python3 tools/gw.py ab 3 "$pairs" "$ON 1" "$ON 0" | awk '{print $18, $20, $30}' | paste - - | awk '$2<59 && $5<59 && $3>4 {a+=$2; b+=$5; n++; print} END {print "pairs", n, "SPU code", a/n, "host kernels", b/n}'
python3 tools/gw.py stop >/dev/null; pgrep -x rpcs3 | xargs -r kill -9
grep -a "·F " cache/rpcs3/RPCS3.log | head -2 | cut -c1-200

#!/bin/bash
# abctl.sh <pairs> "<live control values A>" "<values B>" [launch arguments]: the fight with two sets of live controls
# switched inside one session, 3 seconds each; the values replace the tail of the defaults (index 0 first)
cd "$(dirname "$0")/.."
pairs=$1; a=$2; b=$3; shift 3
tools/fight.sh go abc --defaults --cfg=test "$@" >/dev/null 2>&1
timeout 400 python3 tools/gw.py ab 3 "$pairs" "$a" "$b" | awk '{n=split($0,f," "); for(i=1;i<=n;i++){if(f[i]=="fps")x=f[i+1]; if(f[i]=="spu")s=f[i+1]}; print x, s}' | paste - - | awk '$2>4 && $4>4 {a+=$1; b+=$3; n++; print} END {print "pairs", n, "A", a/n, "B", b/n}'
python3 tools/gw.py stop >/dev/null; pgrep -x rpcs3 | xargs -r kill -9
grep -a "·F " cache/rpcs3/RPCS3.log | head -2 | cut -c1-200

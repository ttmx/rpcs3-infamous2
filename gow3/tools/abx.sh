#!/bin/bash
# abx.sh <seconds> <rounds> <settle> <arm A> <arm B>: live control arms inside one fight session in the order
# A B B A B A A B, so that neither arm follows the fight's load cycle (about 20 s); prints mean, median and slow windows
cd "$(dirname "$0")/.."
secs=$1; rounds=$2; settle=$3; A=$4; B=$5
tools/fight.sh go abx --defaults --cfg=test >/dev/null 2>&1
python3 tools/gw.py seq "wait:$settle"
python3 tools/gw.py ab "$secs" "$rounds" "$A" "$B" "$B" "$A" "$B" "$A" "$A" "$B" > /tmp/abx.txt 2>&1
python3 tools/gw.py stop >/dev/null; pgrep -x rpcs3 | xargs -r kill -9
python3 -I - "$A" "$B" <<'P'
import re, sys, statistics as st
rows = []
for l in open('/tmp/abx.txt'):
    m = re.match(r'ctl=(.*?) \| fps ([\d.]+) min ([\d.]+) W ([\d.]+) .* spu ([\d.]+)', l)
    if m and float(m[5]) > 4.5: rows.append((m[1], float(m[2]), float(m[3]), float(m[4])))
for arm in sys.argv[1:3]:
    v = [r for r in rows if r[0] == arm]
    if v: print(arm, '| n', len(v), 'fps', round(st.mean(r[1] for r in v), 2), 'median', round(st.median(r[1] for r in v), 2), 'min', round(st.mean(r[2] for r in v), 2), 'below 50:', sum(r[1] < 50 for r in v), 'W', round(st.mean(r[3] for r in v), 1))
P

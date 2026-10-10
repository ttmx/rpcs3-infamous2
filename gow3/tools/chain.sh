#!/bin/bash
# chain.sh [seconds] [launch arguments]: the fight with the sampler: what the SPU thread of the second SPURS instance
# (0x200: noise task and its neighbours) and the five workers run, and the frame rate
cd "$(dirname "$0")/.."
secs=${1:-16}; shift
rm -f /tmp/ch /tmp/ch.*
tools/fight.sh go ch --defaults --cfg=test RPCS3_SPU_SAMPLER=/tmp/ch "$@" >/dev/null 2>&1
python3 tools/gw.py seq "wait:25"; python3 tools/gw.py revive >/dev/null
cp /tmp/ch.threads /tmp/ch.a
python3 tools/gw.py fps "$secs" ch | cut -c1-110
cp /tmp/ch.threads /tmp/ch.b
python3 tools/gw.py stop >/dev/null; pgrep -x rpcs3 | xargs -r kill -9; pgrep -x python3 -a | grep "[g]w.py key" | awk '{print $1}' | xargs -r kill
python3 -I - <<'P'
import collections
def load(p):
    d = {}
    for l in open(p):
        f = l.split(); d[tuple(f[:-1])] = int(f[-1])
    return d
a, b = load('/tmp/ch.a'), load('/tmp/ch.b'); d = {k: v - a.get(k, 0) for k, v in b.items() if v - a.get(k, 0) > 0 and k[0] == 'T'}
tot = collections.Counter()
for k, v in d.items(): tot[k[1]] += v
for th in sorted(tot):
    row = sorted(((v, k[2], k[3]) for k, v in d.items() if k[1] == th), reverse=True)[:7]
    print(th, ' '.join('%s%s:%.0f%%' % (h[:4], '(idle)' if w == '1' else '', v * 100 / tot[th]) for v, h, w in row))
P

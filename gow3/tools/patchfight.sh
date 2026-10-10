#!/bin/bash
# patchfight.sh <name> <windows> <patch lines...>: patchtry.sh, then (if the fight is reached) hold block and print
# 5-second windows, what the SPU threads ran, and errors from the log
cd "$(dirname "$0")/.."
name=$1; n=$2; shift 2
r=$(tools/patchtry.sh "$name" "$@"); echo "$r"
case "$r" in *fight*) ;; *) python3 tools/gw.py stop >/dev/null; pgrep -x rpcs3 | xargs -r kill -9; exit;; esac
(python3 tools/gw.py key q 600 >/dev/null 2>&1 &)
python3 tools/gw.py seq "wait:20"; python3 tools/gw.py revive >/dev/null; cp /tmp/pt.threads /tmp/pf.a
for i in $(seq "$n"); do python3 tools/gw.py fps 5 x | python3 -I -c "import sys,json; r=json.loads(sys.stdin.read()); print(r['fps'],r['spu_cores'],r['rsx_cores'])"; done > /tmp/pf.txt
cp /tmp/pt.threads /tmp/pf.b; python3 tools/gw.py shot /tmp/pf.png 640
python3 tools/gw.py stop >/dev/null; pgrep -x rpcs3 | xargs -r kill -9; pgrep -x python3 -a | grep "[g]w.py key" | awk '{print $1}' | xargs -r kill
awk '$2>4.2{s+=$1;c++; l=l" "int($1+.5)} END{print "windows",c,"mean fps",s/c,"|",l}' /tmp/pf.txt
python3 -I - <<'P'
import collections
def load(p):
    d = {}
    for l in open(p):
        f = l.split(); d[tuple(f[:-1])] = int(f[-1])
    return d
a, b = load('/tmp/pf.a'), load('/tmp/pf.b'); d = {k: v - a.get(k, 0) for k, v in b.items() if v - a.get(k, 0) > 0 and k[0] == 'T'}
tot = collections.Counter()
for k, v in d.items(): tot[k[1]] += v
for th in sorted(tot):
    row = sorted(((v, k[2], k[3]) for k, v in d.items() if k[1] == th), reverse=True)[:5]
    print(th, ' '.join('%s%s:%.0f%%' % (h[:4], '(idle)' if w == '1' else '', v * 100 / tot[th]) for v, h, w in row))
P
grep -a "Access violation\|·F" cache/rpcs3/RPCS3.log | head -3 | cut -c1-200

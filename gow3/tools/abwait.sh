#!/bin/bash
# abwait.sh <rounds> <arm A> <arm B>: two live control settings inside one fight session, 8 seconds each in the
# order A B B A, and for each the main PPU thread's time by wait site and the render thread's working time per frame,
# both from the sampler
cd "$(dirname "$0")/.."
rounds=$1; A=$2; B=$3
rm -f /tmp/aw /tmp/aw.* /tmp/aw-*
tools/fight.sh go aw --defaults --cfg=test RPCS3_SPU_SAMPLER=/tmp/aw $ABX_ARGS >/dev/null 2>&1
python3 tools/gw.py seq "wait:30"
n=0
for r in $(seq "$rounds"); do for arm in A B B A; do
  python3 tools/gw.py revive >/dev/null
  [ $arm = A ] && python3 tools/gw.py ctl "$A" || python3 tools/gw.py ctl "$B"
  python3 tools/gw.py seq "wait:4"; cp /tmp/aw.stack /tmp/aw-$n.a; cp /tmp/aw.ppu /tmp/aw-$n.pa; cp /tmp/aw.rsx /tmp/aw-$n.ra
  f=$(python3 tools/gw.py fps 8 aw | python3 -I -c "import sys,json; r=json.loads(sys.stdin.read()); print(r['fps'], r['spu_cores'])")
  cp /tmp/aw.stack /tmp/aw-$n.b; cp /tmp/aw.ppu /tmp/aw-$n.pb; cp /tmp/aw.rsx /tmp/aw-$n.rb; echo "$arm $f" > /tmp/aw-$n.info; n=$((n+1))
done; done
python3 tools/gw.py stop >/dev/null; pgrep -x rpcs3 | xargs -r kill -9
python3 -I - <<'P'
import collections, glob
def load(p):
    d = collections.Counter()
    for l in open(p):
        f = l.split(); d[f[1]] += int(f[0])
    return d
def total(p):
    t = 0
    for l in open(p):
        i, pc, w, c = l.split()
        if i == '01000000': t += int(c)
    return t
arms = collections.defaultdict(lambda: [collections.Counter(), 0, [], 0, 0, 0])
for info in sorted(glob.glob('/tmp/aw-*.info')):
    base = info[:-5]; arm, fps, spu = open(info).read().split()
    if float(spu) < 4.5: continue
    a, b = load(base + '.a'), load(base + '.b'); t = total(base + '.pb') - total(base + '.pa')
    for k, v in b.items(): arms[arm][0][k] += v - a.get(k, 0)
    arms[arm][1] += t; arms[arm][2].append(float(fps)); arms[arm][3] += 1
    ra = list(map(int, open(base + '.ra').read().split())); rb = list(map(int, open(base + '.rb').read().split()))
    arms[arm][4] += rb[0] - ra[0]; arms[arm][5] += sum(rb) - sum(ra)
for arm in sorted(arms):
    c, t, fps, n, run, rs = arms[arm]; ms = 1000 / (sum(fps) / len(fps))
    print(arm, 'windows', n, 'fps %.1f' % (sum(fps) / len(fps)), '| ms per frame at', ' '.join('%s: %.2f' % (k, ms * v / t) for k, v in c.most_common(6)), '| all %.2f' % (ms * sum(c.values()) / t), '| render thread working %.2f ms per frame (%.0f%%)' % (ms * run / max(rs, 1), run * 100 / max(rs, 1)))
P

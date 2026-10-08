#!/bin/bash
# ownwork.sh <seconds> [launch arguments]: the main PPU thread's time per frame in the fight, split into waiting,
# spinning (the two job waits, cia 0x2eafe8 and 0x22fb28) and the rest (its own work), from the sampler
cd "$(dirname "$0")/.."
secs=$1; shift
rm -f /tmp/ow /tmp/ow.*
tools/fight.sh go ow --defaults --cfg=test RPCS3_SPU_SAMPLER=/tmp/ow "$@" >/dev/null 2>&1
python3 tools/gw.py seq "wait:30"; python3 tools/gw.py revive >/dev/null
cp /tmp/ow.ppu /tmp/ow.a
fps=$(python3 tools/gw.py fps "$secs" ow | python3 -I -c "import sys,json; r=json.loads(sys.stdin.read()); print(r['fps'], r['spu_cores'])")
cp /tmp/ow.ppu /tmp/ow.b
python3 tools/gw.py stop >/dev/null; pgrep -x rpcs3 | xargs -r kill -9
python3 -I - $fps <<'P'
import collections, sys
def load(p):
    d = collections.Counter()
    for l in open(p):
        i, pc, w, c = l.split()
        if i == '01000000': d[(int(pc, 16), w)] = int(c)
    return d
a, b = load('/tmp/ow.a'), load('/tmp/ow.b')
d = {k: v - a.get(k, 0) for k, v in b.items() if v - a.get(k, 0) > 0}
tot = sum(d.values()); wait = sum(v for (pc, w), v in d.items() if w == '1')
spin = sum(v for (pc, w), v in d.items() if w == '0' and pc in (0x2eafe8, 0x22fb28))
fps = float(sys.argv[1]); ms = 1000 / fps
print('fps %.1f spu %s | frame %.1f ms: waiting %.1f, spinning %.1f, own work %.1f ms' % (fps, sys.argv[2], ms, ms * wait / tot, ms * spin / tot, ms * (tot - wait - spin) / tot))
P

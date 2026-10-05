#!/usr/bin/env python3
"""shadow_stats.py <trace.txt>: what changes between consecutive draws of the shadow-map pass (depth-only target)."""
import sys, collections
draws = []; cur = None; pend = {}
for line in open(sys.argv[1]):
    p = line.split()
    if p[0] == 'R': pend['R'] = p[1:]
    elif p[0] == 'C': pend['C'] = p[1:]
    elif p[0] == 'D':
        cur = dict(frame=int(p[1]), n=int(p[2]), cmd=int(p[3]), prim=int(p[4]), vcount=int(p[6]), dcount=int(p[7]), ibytes=int(p[11]), vp=p[13],
                   color=int(p[15], 16), depth=int(p[16], 16), nblocks=int(p[17]), blocks=[], R=pend.get('R', []), C=pend.get('C', []))
        pend = {}; draws.append(cur)
    elif p[0] == 'B':
        cur['blocks'].append((int(p[6]), p[12] if len(p) > 12 else ''))
frames = sorted(set(d['frame'] for d in draws)); f = frames[1]
fd = [d for d in draws if d['frame'] == f]
print('frame', f, 'draws', len(fd))
# pass segmentation by render target
seg = []
for i, d in enumerate(fd):
    k = (d['color'], d['depth'])
    if not seg or seg[-1][0] != k: seg.append([k, i, 0])
    seg[-1][2] += 1
print('target runs (color, depth, first draw, count):')
for k, i, n in seg:
    if n >= 20: print(f'  {k[0]:08x} {k[1]:08x} at {i:5d} x{n}')
names = {}
try:
    import re
    for m in re.finditer(r'^\s*(NV\w+)\s*=\s*(0x[0-9a-fA-F]+)', open(sys.argv[2]).read(), re.M): names.setdefault(int(m[2], 16), m[1])
except Exception: pass
def regname(r):
    best = max((k for k in names if k <= r), default=None)
    if best is None: return hex(r)
    return names[best] + (f'+{r-best:x}' if r != best else '')
sh = [d for d in fd if d['color'] == 0 and d['depth'] == 0xcf2f0000]
print('\nshadow draws', len(sh), 'triangles', sum(d['dcount'] for d in sh) // 3, 'vertices', sum(d['vcount'] for d in sh))
print('vertex programs:', collections.Counter(d['vp'] for d in sh).most_common(8))
print('layouts:', collections.Counter(' | '.join(f"s{s} " + ','.join(':'.join(a.split(':')[:3]) for a in at.split(';') if a) for s, at in d['blocks']) for d in sh).most_common(8))
print('prims', collections.Counter(d['prim'] for d in sh), 'cmd', collections.Counter(d['cmd'] for d in sh))
rc = collections.Counter(); nreg = collections.Counter(); cc = collections.Counter(); ncon = collections.Counter()
for d in sh[1:]:
    regs = [int(x.split(':')[0], 16) for x in d['R'] if ':' in x]
    for r in regs: rc[regname(r)] += 1
    nreg[len(regs)] += 1
    cons = [int(x.split(':')[0]) for x in d['C'] if ':' in x]
    for c in cons: cc[c] += 1
    ncon[len(cons)] += 1
print('\nregisters changed between consecutive shadow draws (count of draws):')
for r, n in rc.most_common(45): print(f'  {n:5d} {r}')
print('changed-register count histogram:', sorted(nreg.items())[:20])
print('transform constants changed:', sorted(cc.items()))
print('changed-constant count histogram:', sorted(ncon.items()))

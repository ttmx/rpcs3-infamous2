#!/usr/bin/env python3
"""blocked.py <before> <after>: two snapshots of <RPCS3_SPU_SAMPLER>.blocked: for every place the main PPU thread
waits or spins at, what the SPU threads were running meanwhile (program, pcs; 'waiting' = the SPU thread sleeps)."""
import collections, sys
def load(p):
    d = collections.Counter()
    for l in open(p):
        f = l.split(); d[(f[0], f[1], int(f[2], 16))] = int(f[3])
    return d
a, b = load(sys.argv[1]), load(sys.argv[2])
bl = collections.Counter({k: v - a.get(k, 0) for k, v in b.items() if v - a.get(k, 0) > 0})
sites = collections.Counter()
for (s, h, pc), v in bl.items(): sites[s] += v
for site, t in sites.most_common(7):
    print('main thread at', site, ': SPU thread samples', t)
    prog = collections.Counter()
    for (s, h, pc), v in bl.items():
        if s == site: prog[h + (' waiting' if pc & 1 else '')] += v
    for h, v in prog.most_common(7):
        pcs = collections.Counter({pc & ~1: v2 for (s2, h2, pc), v2 in bl.items() if s2 == site and h2 + (' waiting' if pc & 1 else '') == h})
        print('  %5.1f%% %s  ' % (v * 100 / t, h), ' '.join('%x:%d%%' % (p, c * 100 / v) for p, c in pcs.most_common(6)))

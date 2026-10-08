#!/usr/bin/env python3
"""samp.py <before> <after> [program hash]: SPU time by program between two snapshots of the RPCS3_SPU_SAMPLER file; with
a hash, that program's time by pc."""
import collections, sys
def load(p):
    d = collections.Counter()
    for l in open(p):
        h, pc, w, c = l.split(); d[(h, int(pc, 16), w)] = int(c)
    return d
a, b = load(sys.argv[1]), load(sys.argv[2])
d = collections.Counter({k: v - a.get(k, 0) for k, v in b.items() if v - a.get(k, 0) > 0})
run = collections.Counter(); wait = collections.Counter(); pcs = collections.defaultdict(collections.Counter)
for (h, pc, w), c in d.items():
    if w == '1': wait[h] += c
    else: run[h] += c; pcs[h][pc] += c
t = sum(run.values())
if len(sys.argv) > 3:
    h = sys.argv[3]; acc = 0
    for pc, n in pcs[h].most_common(45):
        acc += n; print('%05x %5.1f%% of program, %5.1f%% of all, cumulative %5.1f%%' % (pc, n * 100 / run[h], n * 100 / t, acc * 100 / run[h]))
else:
    print('running', t, 'waiting', sum(wait.values()))
    for h, c in run.most_common(16):
        print(h, '%5.1f%%' % (c * 100 / t), 'wait %5.1f%%' % (wait[h] * 100 / t), ' '.join('%x:%.0f%%' % (p, n * 100 / c) for p, n in pcs[h].most_common(8)))

#!/usr/bin/env python3
"""loops.py <capture dir> [max jobs]: the hot loops of the replayed jobs: for every backward branch taken, the loop
(target..branch), how many instructions ran inside it and how often it was entered and iterated."""
import sys, glob, collections, struct; sys.path.insert(0, __file__.rsplit('/', 1)[0])
import gj
pcs = collections.Counter(); back = collections.Counter(); total = 0; jobs = 0
for p in sorted(glob.glob(sys.argv[1] + '/job-*.bin'))[:int(sys.argv[2]) if len(sys.argv) > 2 else 10000]:
    j = gj.Job(p)
    if j.srr0 != 0x4bd8: continue
    st = {'prev': 0}
    def hook(s):
        pc = s.pc; pcs[pc] += 1; prev = st['prev']
        if pc <= prev and prev - pc < 0x1000 and pc != prev: back[(pc, prev)] += 1
        st['prev'] = pc
    j.run(hook); jobs += 1
total = sum(pcs.values())
rows = []
for (head, br), iters in back.items():
    inside = sum(c for a, c in pcs.items() if head <= a <= br)
    rows.append((inside, head, br, iters, pcs[head] - iters))
rows.sort(reverse=True)
print('jobs', jobs, 'instructions', total)
seen = []
for inside, head, br, iters, entered in rows[:40]:
    if any(h <= head and br <= b for h, b in seen): continue      # inner part of a loop already listed
    seen.append((head, br))
    print('loop %05x..%05x  %5.1f%% of all  %4d instructions long  iterations %7d  entered %6d (%.1f iterations per entry, %.2f entries per job)' % (head, br, inside * 100 / total, (br - head) // 4 + 1, iters, entered, iters / max(entered, 1) + 1, entered / jobs))

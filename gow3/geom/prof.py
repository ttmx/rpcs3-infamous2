#!/usr/bin/env python3
"""prof.py <capture dir> [max jobs]: replay the captured jobs and print SPU instructions by function, plus how the
replays compare with the captured outputs."""
import sys, glob, collections, bisect, struct, time, pathlib; sys.path.insert(0, __file__.rsplit('/', 1)[0])
import gj
root = pathlib.Path(__file__).resolve().parent.parent
d = (root / 'spu/ls-240-pcf1a0.bin').read_bytes()
W = lambda pc: struct.unpack_from('>I', d, pc)[0]
ent = set()
for pc in range(0x2800, 0x12000, 4):
    w = W(pc)
    if (w >> 23) == 0x66:
        off = (w >> 7) & 0xffff; off -= 0x10000 if off & 0x8000 else 0; t = (pc + off * 4) & 0x3fffc
        if t != pc + 4: ent.add(t)
    if w == 0x24004080: ent.add(pc)
ent = sorted(ent); entset = set(ent)
hist = collections.Counter(); calls = collections.Counter(); cls = collections.Counter(); sizes = []; ex = []; t0 = time.time()
for p in sorted(glob.glob(sys.argv[1] + '/job-*.bin'))[:int(sys.argv[2]) if len(sys.argv) > 2 else 10000]:
    j = gj.Job(p)
    if j.srr0 != 0x4bd8: cls['not in a job'] += 1; continue
    def hook(s):
        pc = s.pc; hist[ent[bisect.bisect_right(ent, pc) - 1]] += 1
        if pc in entset: calls[pc] += 1
    try: s = j.run(hook)
    except Exception as e: cls['error'] += 1; ex.append((p[-12:], str(e)[:90])); continue
    c = j.check(s); sizes.append(s.count)
    if s.phase != 1 or s.notes: cls['did not finish'] += 1; ex.append((p[-12:], s.notes[:2]))
    elif not c: cls['identical'] += 1
    elif any('header' in x or 'puts' in x for x in c): cls['sizes differ'] += 1; ex.append((p[-12:], c[0][:100]))
    else: cls['bytes differ'] += 1
n = len(sizes); t = sum(hist.values()); sizes.sort()
print(dict(cls), 'seconds %.0f' % (time.time() - t0)); print('jobs', n, 'instructions per job: mean', t // n, 'median', sizes[n // 2], 'p90', sizes[n * 9 // 10], 'max', sizes[-1])
acc = 0
for f, c in hist.most_common(34):
    acc += c; print('%05x %5.1f%%  cum %5.1f%%  per job %6d  entered per job %.2f' % (f, c * 100 / t, acc * 100 / t, c // n, calls[f] / n))
for e in ex[:10]: print(e)

#!/usr/bin/env python3
"""verify.py <capture dir>: compare the outputs the emulator produced (captured with the host kernels active) with
the reference replay of the same jobs. Checks the kernels as built into the emulator."""
import sys, glob, collections; sys.path.insert(0, __file__.rsplit('/', 1)[0])
import gj, kern
res = collections.Counter(); un = words = 0; bad = []
for p in sorted(glob.glob(sys.argv[1] + '/job-*.bin')):
    j = gj.Job(p)
    if j.srr0 != 0x4bd8: continue
    s = j.run(); real = j.puts[:len(s.out[0])]
    c = kern.compare(real, s.out[0]) if s.phase == 1 and not s.notes else 'replay did not finish %s' % s.notes[:1]
    if isinstance(c, str): res['differs'] += 1; bad.append((p[-12:], c))
    else:
        un += c[1]; words += c[2]; res['same' if c[0] == 0 else ('close' if c[1] == 0 else 'unexplained')] += 1
        if c[1]: bad.append((p[-12:], 'words differ %d, unexplained %d of %d' % c))
print(dict(res), 'unexplained words %d of %d' % (un, words))
for b in bad[:10]: print('  ', b)

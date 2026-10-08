#!/usr/bin/env python3
"""kdiff.py <kernel> <capture file> [call number]: run the reference to the kernel's entry, then compare the kernel's
effect on the local store and registers with the reference's at the exit."""
import sys, copy, struct, numpy as np; sys.path.insert(0, __file__.rsplit('/', 1)[0])
import gj, kern, live
name = sys.argv[1]; entry, exit_, fn = kern.KERNELS[name]; j = gj.Job(sys.argv[2]); want = int(sys.argv[3]) if len(sys.argv) > 3 else 0
s = j.spu(); s.step(); k = 0
while True:
    if s.pc == entry:
        if k == want: break
        k += 1
    s.step()
ls0 = bytes(s.ls); r0 = [x.copy() for x in s.r]
print('entry: ' + ' '.join('r%d=%x' % (r, int(s.r[r][0])) for r in sorted(live.live_in(entry, exit_)[0])))
while s.pc != exit_: s.step()
ref_ls = bytes(s.ls); ref_r = [x.copy() for x in s.r]
s.ls = bytearray(ls0); s.r = [x.copy() for x in r0]; fn(s); mine = bytes(s.ls)
def ranges(pred):
    out = []
    for a in range(0, 0x3f000, 16):
        if pred(a):
            if out and a == out[-1][1]: out[-1][1] = a + 16
            else: out.append([a, a + 16])
    return out
print('reference changed:', ' '.join('%x-%x' % tuple(x) for x in ranges(lambda a: ref_ls[a:a + 16] != ls0[a:a + 16])))
print('kernel changed   :', ' '.join('%x-%x' % tuple(x) for x in ranges(lambda a: mine[a:a + 16] != ls0[a:a + 16])))
bad = ranges(lambda a: mine[a:a + 16] != ref_ls[a:a + 16]); print('different        :', ' '.join('%x-%x' % tuple(x) for x in bad[:12]))
for a, b in bad[:3]:
    for o in range(a, min(b, a + 64), 16):
        f = lambda x: ' '.join('%11.5g' % v for v in struct.unpack('>4f', x))
        print('  %05x before %s\n        ref    %s\n        mine   %s' % (o, ls0[o:o + 16].hex(), f(ref_ls[o:o + 16]) + '  ' + ref_ls[o:o + 16].hex(), f(mine[o:o + 16]) + '  ' + mine[o:o + 16].hex()))
lo = live.live_in(exit_, None, 0x4000, 0x12000)[0]
for r in sorted(lo):
    if not np.array_equal(s.r[r], ref_r[r]) and not np.array_equal(ref_r[r], r0[r]): print('  register r%d ref %s mine %s' % (r, ' '.join('%x' % int(x) for x in ref_r[r]), ' '.join('%x' % int(x) for x in s.r[r])))

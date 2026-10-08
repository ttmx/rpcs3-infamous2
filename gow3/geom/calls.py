#!/usr/bin/env python3
"""calls.py <function address hex> <capture dir> [max calls]: replay the captures and save every call of the function
(local store and registers before and after, by the reference interpreter) to geom/calls-<address>.npz for
prototyping a native version. Keys: ls0, ls1 (n x 0x40000 uint8), r0, r1 (n x 128 x 4 uint32, word 0 preferred), src."""
import sys, glob, numpy as np; sys.path.insert(0, __file__.rsplit('/', 1)[0])
import gj
F = int(sys.argv[1], 16); limit = int(sys.argv[3]) if len(sys.argv) > 3 else 300
ls0, ls1, r0, r1, src = [], [], [], [], []
for p in sorted(glob.glob(sys.argv[2] + '/job-*.bin')):
    if len(ls0) >= limit: break
    j = gj.Job(p)
    if j.srr0 != 0x4bd8: continue
    st = {}
    def hook(s):
        if s.pc == F and not st:
            st.update(ret=int(s.r[0][0]) & 0x3fffc, sp=int(s.r[1][0]))
            ls0.append(np.frombuffer(bytes(s.ls), dtype=np.uint8)); r0.append(np.array(s.r, dtype=np.uint32)); src.append(p[-12:])
        elif st and 'done' not in st and s.pc == st['ret'] and int(s.r[1][0]) == st['sp']:
            st['done'] = True; ls1.append(np.frombuffer(bytes(s.ls), dtype=np.uint8)); r1.append(np.array(s.r, dtype=np.uint32))
    j.run(hook)
    if len(ls1) < len(ls0): ls0.pop(); r0.pop(); src.pop()
out = __file__.rsplit('/', 1)[0] + '/calls-%x.npz' % F
np.savez_compressed(out, ls0=np.array(ls0), ls1=np.array(ls1), r0=np.array(r0), r1=np.array(r1), src=np.array(src))
print(len(ls0), 'calls saved to', out)

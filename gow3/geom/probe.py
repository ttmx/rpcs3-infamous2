#!/usr/bin/env python3
"""probe.py <function address hex> <capture file>... : for each call of the function in the replays: the arguments, the
local store ranges it reads and writes (16-byte slots, merged), and the instruction count."""
import sys, struct; sys.path.insert(0, __file__.rsplit('/', 1)[0])
import gj, spuemu
F = int(sys.argv[1], 16)
def ranges(slots):
    out = []; 
    for a in sorted(slots):
        if out and a == out[-1][1]: out[-1][1] = a + 16
        else: out.append([a, a + 16])
    return ' '.join('%x-%x' % (a, b) for a, b in out)
for p in sys.argv[2:]:
    j = gj.Job(p)
    if j.srr0 != 0x4bd8: continue
    st = {'in': False}
    olq, osq = spuemu.SPU.lq, spuemu.SPU.sq
    def lq(self, a):
        if st['in'] and (a & 0x3fff0) not in st['w']: st['r'].add(a & 0x3fff0)
        return olq(self, a)
    def sq(self, a, v):
        if st['in']: st['w'].add(a & 0x3fff0)
        osq(self, a, v)
    spuemu.SPU.lq, spuemu.SPU.sq = lq, sq
    def hook(s):
        if s.pc == F and not st['in']:
            st.update({'in': True, 'r': set(), 'w': set(), 'n': s.count, 'ret': int(s.r[0][0]) & 0x3fffc, 'sp': int(s.r[1][0])})
            print(p[-12:], 'call: ' + ' '.join('r%d=%s' % (i, ' '.join('%x' % int(x) for x in s.r[i])) for i in range(3, 10)), 'sp %x' % st['sp'])
        elif st['in'] and s.pc == st['ret'] and int(s.r[1][0]) == st['sp']:
            st['in'] = False; sp = st['sp']
            print('   instructions', s.count - st['n'], 'returns r3=%x' % int(s.r[3][0]))
            print('   reads ', ranges(a for a in st['r'] if not sp - 0x400 <= a < sp + 0x40))
            print('   writes', ranges(a for a in st['w'] if not sp - 0x400 <= a < sp + 0x40))
    try: j.run(hook)
    finally: spuemu.SPU.lq, spuemu.SPU.sq = olq, osq

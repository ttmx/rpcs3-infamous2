#!/usr/bin/env python3
"""live.py <entry hex> <exit hex> [function end hex]: for a region of the geometry job that is to be replaced by host
code: the registers the region reads before writing them (live in at the entry), the registers it writes, and of those
the ones that code after the exit may read before writing (live out). Static, over the control flow inside the
function; indirect branches are taken as function returns (r0, r1, r3 and r80..r127 live)."""
import sys, struct, pathlib
sys.path.insert(0, '/home/tiago/Applications/rpcs3/profiling/native-investigation/build-prep/source/infamous2/reference/spu')
import spudis
ls = (pathlib.Path(__file__).resolve().parent.parent / 'spu/ls-240-pcf1a0.bin').read_bytes()
W = lambda pc: struct.unpack_from('>I', ls, pc)[0]
STORE = {'STQD', 'STQX', 'STQR', 'STQA'}; BR_COND = {'BRZ', 'BRNZ', 'BRHZ', 'BRHNZ'}; BI_COND = {'BIZ', 'BINZ', 'BIHZ', 'BIHNZ'}
NODEF = STORE | BR_COND | BI_COND | {'BR', 'BRA', 'BI', 'WRCH', 'NOP', 'LNOP', 'HBR', 'HBRR', 'HBRA', 'SYNC', 'DSYNC', 'STOP', 'STOPD', 'HEQ', 'HEQI', 'HGT', 'HGTI', 'HLGT', 'HLGTI', 'IRET', '.word'}
def info(pc):
    """(uses, defs, successors or None for a return)"""
    w = W(pc); name, f = spudis.decode(w); m = f.get('magn'); use = set(); de = set(); nxt = [pc + 4]
    if name in ('NOP', 'LNOP', 'HBR', 'HBRR', 'HBRA', 'SYNC', 'DSYNC', '.word', 'STOP', 'STOPD'): return use, de, nxt
    if m == 7: use = {f['ra'], f['rb'], f['rc']}; de = {f['rt4']}
    elif m == 4: de = {f['rt']}                                   # ILA
    elif m == 3: use = {f['ra']}; de = {f['rt']}
    elif m == 2:
        if name in ('IL', 'ILH', 'ILHU', 'LQA', 'LQR', 'FSMBI'): de = {f['rt']}
        elif name == 'IOHL': use = {f['rt']}; de = {f['rt']}
        elif name in ('STQA', 'STQR'): use = {f['rt']}
        elif name in BR_COND: use = {f['rt']}; nxt = [pc + 4, (pc + (spudis.sext(f['i16'], 16) << 2)) & 0x3fffc]
        elif name == 'BR': nxt = [(pc + (spudis.sext(f['i16'], 16) << 2)) & 0x3fffc]
        elif name == 'BRSL':
            t = (pc + (spudis.sext(f['i16'], 16) << 2)) & 0x3fffc; de = {f['rt']}
            if t != pc + 4: use = set(range(3, 11)) | {1}; de = set(range(0, 80)) - {1}      # a call: arguments in, volatile registers out
        else: de = {f['rt']}
    else:
        if name in ('BI', 'IRET'): return {f['ra'], 0, 1, 3} | set(range(80, 128)), set(), None
        if name in BI_COND: return {f['ra'], f['rt'], 0, 1, 3} | set(range(80, 128)), set(), [pc + 4]
        if name == 'BISL': use = {f['ra']} | set(range(3, 11)) | {1}; de = set(range(0, 80)) - {1}
        elif name == 'WRCH': use = {f['rt']}
        elif name in ('RDCH', 'RCHCNT'): de = {f['rt']}
        elif name in spudis.UNARY or name in spudis.I7 or name in spudis.I8 or m == 1: use = {f['ra']}; de = {f['rt']}
        else: use = {f['ra'], f['rb']}; de = {f['rt']}
    if name in STORE: use |= {f['rt']}; de = set()
    if name in NODEF: de = set() if name not in ('BRSL', 'BISL') else de
    return use, de, nxt
def live_in(start, stop=None, lo=0, hi=0x40000):
    """registers read before written starting at start; paths end at stop (if given) or at returns"""
    livein = {}; work = [start]; succ = {}
    order = []
    seen = set()
    while work:
        pc = work.pop()
        if pc in seen or pc == stop or not lo <= pc < hi: continue
        seen.add(pc); u, d, n = info(pc); succ[pc] = (u, d, n or [])
        work += n or []
    changed = True
    while changed:
        changed = False
        for pc in sorted(seen, reverse=True):
            u, d, n = succ[pc]; out = set()
            for t in n: out |= livein.get(t, set())
            new = u | (out - d)
            if new != livein.get(pc, set()): livein[pc] = new; changed = True
    return livein.get(start, set()), seen, succ
if __name__ == '__main__':
    entry, exit_ = int(sys.argv[1], 16), int(sys.argv[2], 16); hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x12000
    lin, seen, succ = live_in(entry, exit_)
    written = set().union(*[succ[p][1] for p in seen]) if seen else set()
    after, _, _ = live_in(exit_, None, 0x4000, hi)
    fmt = lambda s: ' '.join('r%d' % r for r in sorted(s))
    print('region %x..%x: %d instructions reached' % (entry, exit_, len(seen)))
    print('live in :', fmt(lin)); print('writes  :', fmt(written)); print('live out:', fmt(written & after))
    print('live after the exit but not written by the region:', fmt(after - written))

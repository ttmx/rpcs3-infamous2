#!/usr/bin/env python3
"""Dataflow tracer on top of spuemu: every 32-bit word carries a node id; nodes record op, inputs and value."""
import sys, struct, pickle, numpy as np
sys.path.insert(0, '../spu')
import spuemu, spudis
NODES = []          # (op, args, value, extra)
def node(op, args, value, extra=None):
    NODES.append((op, tuple(args), int(value) & 0xffffffff, extra)); return len(NODES) - 1
BYTE_OPS = {'SHUFB', 'ROTQBY', 'ROTQBYI', 'SHLQBYI', 'SHLQBY', 'ROTQMBYI', 'ROTQMBY'}
CONST_OPS = {'IL', 'ILH', 'ILHU', 'ILA', 'FSMBI', 'CWD', 'CHD', 'CBD', 'CDD', 'CWX', 'CDX', 'CBX', 'CHX'}
NOIO = {'BR', 'BRA', 'BRZ', 'BRNZ', 'BRHZ', 'BRHNZ', 'BI', 'BIZ', 'BINZ', 'BIHZ', 'BIHNZ', 'NOP', 'LNOP', 'HBR', 'HBRR', 'HBRA', 'SYNC', 'DSYNC', 'HEQI', 'WRCH', 'STOP', 'STOPD'}
class Tracer(spuemu.SPU):
    def __init__(self, ls, gpr, pc):
        super().__init__(ls, gpr, pc)
        self.rt = [[node('reg0', (), int(self.r[i][k]), (i, k)) for k in range(4)] for i in range(128)]
        self.lt = {}
        self.leaf_name = lambda addr: None
    def load_tags(self, a):
        a &= 0x3fff0; out = []
        for k in range(4):
            t = self.lt.get(a + 4 * k)
            if t is None:
                t = node('ls', (), struct.unpack_from('>I', self.ls, a + 4 * k)[0], a + 4 * k); self.lt[a + 4 * k] = t
            out.append(t)
        return out
    def step(self):
        pc = self.pc; w = struct.unpack_from('>I', self.ls, pc)[0]; name, q = spudis.decode(w)
        r = self.r; rt = self.rt
        if name in ('LQD', 'LQX', 'LQR', 'LQA'):
            a = {'LQD': lambda: int(r[q['ra']][0]) + q['si10'] * 16, 'LQX': lambda: int(r[q['ra']][0]) + int(r[q['rb']][0]),
                 'LQR': lambda: pc + (spudis.sext(q['i16'], 16) << 2), 'LQA': lambda: q['i16'] << 2}[name]()
            tags = self.load_tags(a); super().step(); rt[q['rt']] = tags; return
        if name in ('STQD', 'STQX', 'STQR', 'STQA'):
            a = {'STQD': lambda: int(r[q['ra']][0]) + q['si10'] * 16, 'STQX': lambda: int(r[q['ra']][0]) + int(r[q['rb']][0]),
                 'STQR': lambda: pc + (spudis.sext(q['i16'], 16) << 2), 'STQA': lambda: q['i16'] << 2}[name]() & 0x3fff0
            for k in range(4): self.lt[a + 4 * k] = rt[q['rt']][k]
            super().step(); return
        if name in NOIO: super().step(); return
        m = q.get('magn', 0)
        if name in ('BRSL', 'BISL'):
            super().step(); rt[q['rt']] = [node('const', (), int(r[q['rt']][k])) for k in range(4)]; return
        if name == 'RDCH' or name == 'RCHCNT':
            super().step(); rt[q['rt']] = [node('chan', (), int(r[q['rt']][k])) for k in range(4)]; return
        if m == 7: outr = q['rt4']; ins = [q['ra'], q['rb'], q['rc']]
        elif name in CONST_OPS: outr = q['rt']; ins = []
        elif name == 'IOHL': outr = q['rt']; ins = [q['rt']]
        elif m == 3: outr = q['rt']; ins = [q['ra']]
        elif name in spudis.I7 or name in spudis.I8 or name in spudis.UNARY or m == 1: outr = q['rt']; ins = [q['ra']]
        else: outr = q['rt']; ins = [q['ra'], q['rb']]
        in_tags = [list(rt[i]) for i in ins]; in_vals = [r[i].copy() for i in ins]
        super().step()
        val = r[outr]
        if name in BYTE_OPS:
            # byte source map
            if name == 'SHUFB':
                c = spuemu.tob(in_vals[2]); src = []
                for i in range(16):
                    x = c[i]
                    if x & 0x80: src.append(None)
                    else: src.append((1 if x & 0x10 else 0, x & 15))
            else:
                if name in ('ROTQBY', 'ROTQBYI'):
                    n = (int(in_vals[1][0]) if name == 'ROTQBY' else q['i7']) & 15; src = [(0, (i + n) % 16) for i in range(16)]
                elif name in ('SHLQBYI', 'SHLQBY'):
                    n = (int(in_vals[1][0]) if name == 'SHLQBY' else q['i7']) & 31; src = [(0, i + n) if i + n < 16 else None for i in range(16)]
                else:
                    n = (-(int(in_vals[1][0]) if name == 'ROTQMBY' else q['i7'])) & 31; src = [(0, i - n) if i - n >= 0 else None for i in range(16)]
            tags = []
            for k in range(4):
                s4 = src[4 * k:4 * k + 4]
                if all(x is not None for x in s4) and all(x[0] == s4[0][0] for x in s4) and s4[0][1] % 4 == 0 and [x[1] for x in s4] == list(range(s4[0][1], s4[0][1] + 4)):
                    tags.append(in_tags[s4[0][0]][s4[0][1] // 4])
                elif all(x is None for x in s4): tags.append(node('const', (), int(val[k])))
                else: tags.append(node('pack', [in_tags[x[0]][x[1] // 4] for x in s4 if x is not None], int(val[k]), [None if x is None else x[1] % 4 for x in s4]))
            rt[outr] = tags; return
        if not ins: rt[outr] = [node('const', (), int(val[k])) for k in range(4)]; return
        imm = q.get('si10') if m == 3 else (q.get('i8') if (name in spudis.I8 or m == 1) else (q.get('si7') if name in spudis.I7 else None))
        rt[outr] = [node(name, [t[k] for t in in_tags], int(val[k]), imm) for k in range(4)]
def fval(v): return float(np.array([v], dtype=np.uint32).view(np.float32)[0])

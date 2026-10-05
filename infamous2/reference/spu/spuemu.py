#!/usr/bin/env python3
"""Small SPU interpreter for offline study. Semantics follow RPCS3's SPUInterpreter (non-accurate paths).
Registers: numpy uint32[4], index 0 = preferred (most significant) word."""
import numpy as np, struct, json, pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import spudis
LUT = json.load(open(pathlib.Path(__file__).parent / 'spu_luts.json'))
U32 = np.uint32; F32 = np.float32
np.seterr(all='ignore')
def tob(r): return r.astype('>u4').tobytes()
def fromb(b): return np.frombuffer(bytes(b), dtype='>u4').astype(U32)
def f(r): return r.view(F32)
def fr(x): return np.asarray(x, dtype=F32).view(U32).copy()
def s32(r): return r.view(np.int32)
class Halt(Exception): pass
class SPU:
    def __init__(self, ls, gpr, pc):
        self.ls = bytearray(ls); self.r = [g.copy() for g in gpr]; self.pc = pc; self.count = 0
        self.ch = {}; self.on_wrch = None; self.on_rdch = None; self.cache = {}; self.trace = None
    def lq(self, a): a &= 0x3fff0; return fromb(self.ls[a:a + 16])
    def sq(self, a, v): a &= 0x3fff0; self.ls[a:a + 16] = tob(v); self.cache.pop(a, None); self.cache.pop(a + 4, None); self.cache.pop(a + 8, None); self.cache.pop(a + 12, None)
    def step(self):
        pc = self.pc
        d = self.cache.get(pc)
        if d is None:
            w = struct.unpack_from('>I', self.ls, pc)[0]; d = spudis.decode(w) + (w,); self.cache[pc] = d
        name, q, w = d
        self.count += 1
        h = OPS.get(name)
        if h is None: raise Halt(f'unimplemented {name} at 0x{pc:05x}: {spudis.dis(w, pc)}')
        nxt = h(self, q, pc)
        self.pc = (pc + 4) & 0x3fffc if nxt is None else nxt
    def run(self, limit=50_000_000, stop=None):
        while self.count < limit:
            if stop and stop(self): return True
            self.step()
        return False
R = lambda s, n: s.r[n]
def setr(s, n, v): s.r[n] = np.asarray(v, dtype=U32)
OPS = {}
def op(*names):
    def deco(fn):
        for n in names: OPS[n] = fn
        return fn
    return deco
def w0(r): return int(r[0])
# ---- loads / stores
@op('LQD')
def _(s, q, pc): setr(s, q['rt'], s.lq(w0(R(s, q['ra'])) + q['si10'] * 16))
@op('LQX')
def _(s, q, pc): setr(s, q['rt'], s.lq(w0(R(s, q['ra'])) + w0(R(s, q['rb']))))
@op('LQR')
def _(s, q, pc): setr(s, q['rt'], s.lq(pc + (spudis.sext(q['i16'], 16) << 2)))
@op('LQA')
def _(s, q, pc): setr(s, q['rt'], s.lq(q['i16'] << 2))
@op('STQD')
def _(s, q, pc): s.sq(w0(R(s, q['ra'])) + q['si10'] * 16, R(s, q['rt']))
@op('STQX')
def _(s, q, pc): s.sq(w0(R(s, q['ra'])) + w0(R(s, q['rb'])), R(s, q['rt']))
@op('STQR')
def _(s, q, pc): s.sq(pc + (spudis.sext(q['i16'], 16) << 2), R(s, q['rt']))
@op('STQA')
def _(s, q, pc): s.sq(q['i16'] << 2, R(s, q['rt']))
# ---- immediates
@op('IL')
def _(s, q, pc): setr(s, q['rt'], np.full(4, spudis.sext(q['i16'], 16) & 0xffffffff, dtype=U32))
@op('ILH')
def _(s, q, pc): setr(s, q['rt'], np.full(4, (q['i16'] << 16) | q['i16'], dtype=U32))
@op('ILHU')
def _(s, q, pc): setr(s, q['rt'], np.full(4, q['i16'] << 16, dtype=U32))
@op('ILA')
def _(s, q, pc): setr(s, q['rt'], np.full(4, q['i18'], dtype=U32))
@op('IOHL')
def _(s, q, pc): setr(s, q['rt'], R(s, q['rt']) | U32(q['i16']))
@op('FSMBI')
def _(s, q, pc): setr(s, q['rt'], fromb(bytes(0xff if (q['i16'] >> (15 - i)) & 1 else 0 for i in range(16))))
# ---- integer
def halves(r): return np.frombuffer(tob(r), dtype='>u2').astype(np.uint16)
def from_halves(h): return fromb(np.asarray(h, dtype='>u2').tobytes())
def bytes_(r): return np.frombuffer(tob(r), dtype=np.uint8)
@op('A')
def _(s, q, pc): setr(s, q['rt'], R(s, q['ra']) + R(s, q['rb']))
@op('AI')
def _(s, q, pc): setr(s, q['rt'], R(s, q['ra']) + U32(q['si10'] & 0xffffffff))
@op('AH')
def _(s, q, pc): setr(s, q['rt'], from_halves(halves(R(s, q['ra'])) + halves(R(s, q['rb']))))
@op('AHI')
def _(s, q, pc): setr(s, q['rt'], from_halves(halves(R(s, q['ra'])) + np.uint16(q['si10'] & 0xffff)))
@op('SF')
def _(s, q, pc): setr(s, q['rt'], R(s, q['rb']) - R(s, q['ra']))
@op('SFI')
def _(s, q, pc): setr(s, q['rt'], U32(q['si10'] & 0xffffffff) - R(s, q['ra']))
@op('SFH')
def _(s, q, pc): setr(s, q['rt'], from_halves(halves(R(s, q['rb'])) - halves(R(s, q['ra']))))
@op('CG')
def _(s, q, pc): setr(s, q['rt'], ((R(s, q['ra']).astype(np.uint64) + R(s, q['rb']).astype(np.uint64)) >> 32).astype(U32))
@op('ADDX')
def _(s, q, pc): setr(s, q['rt'], R(s, q['ra']) + R(s, q['rb']) + (R(s, q['rt']) & U32(1)))
@op('AND')
def _(s, q, pc): setr(s, q['rt'], R(s, q['ra']) & R(s, q['rb']))
@op('ANDC')
def _(s, q, pc): setr(s, q['rt'], R(s, q['ra']) & ~R(s, q['rb']))
@op('ANDI')
def _(s, q, pc): setr(s, q['rt'], R(s, q['ra']) & U32(q['si10'] & 0xffffffff))
@op('ANDHI')
def _(s, q, pc): setr(s, q['rt'], from_halves(halves(R(s, q['ra'])) & np.uint16(q['si10'] & 0xffff)))
@op('ANDBI')
def _(s, q, pc): setr(s, q['rt'], fromb(bytes(b & (q['si10'] & 0xff) for b in tob(R(s, q['ra'])))))
@op('OR')
def _(s, q, pc): setr(s, q['rt'], R(s, q['ra']) | R(s, q['rb']))
@op('ORC')
def _(s, q, pc): setr(s, q['rt'], R(s, q['ra']) | ~R(s, q['rb']))
@op('ORI')
def _(s, q, pc): setr(s, q['rt'], R(s, q['ra']) | U32(q['si10'] & 0xffffffff))
@op('ORBI')
def _(s, q, pc): setr(s, q['rt'], fromb(bytes(b | (q['si10'] & 0xff) for b in tob(R(s, q['ra'])))))
@op('XOR')
def _(s, q, pc): setr(s, q['rt'], R(s, q['ra']) ^ R(s, q['rb']))
@op('XORI')
def _(s, q, pc): setr(s, q['rt'], R(s, q['ra']) ^ U32(q['si10'] & 0xffffffff))
@op('NOR')
def _(s, q, pc): setr(s, q['rt'], ~(R(s, q['ra']) | R(s, q['rb'])))
@op('NAND')
def _(s, q, pc): setr(s, q['rt'], ~(R(s, q['ra']) & R(s, q['rb'])))
@op('SELB')
def _(s, q, pc): c = R(s, q['rc']); setr(s, q['rt4'], (c & R(s, q['rb'])) | (~c & R(s, q['ra'])))
@op('SHUFB')
def _(s, q, pc):
    a = tob(R(s, q['ra'])); b = tob(R(s, q['rb'])); c = tob(R(s, q['rc'])); o = bytearray(16)
    for i in range(16):
        x = c[i]
        if x & 0x80: o[i] = 0x00 if (x & 0xc0) == 0x80 else (0xff if (x & 0xe0) == 0xc0 else 0x80)
        else: o[i] = b[x & 15] if x & 0x10 else a[x & 15]
    setr(s, q['rt4'], fromb(o))
def cmp_w(fn):
    def h(s, q, pc): setr(s, q['rt'], np.where(fn(s, q), U32(0xffffffff), U32(0)))
    return h
OPS['CEQ'] = cmp_w(lambda s, q: R(s, q['ra']) == R(s, q['rb']))
OPS['CEQI'] = cmp_w(lambda s, q: R(s, q['ra']) == U32(q['si10'] & 0xffffffff))
OPS['CGT'] = cmp_w(lambda s, q: s32(R(s, q['ra'])) > s32(R(s, q['rb'])))
OPS['CGTI'] = cmp_w(lambda s, q: s32(R(s, q['ra'])) > q['si10'])
OPS['CLGT'] = cmp_w(lambda s, q: R(s, q['ra']) > R(s, q['rb']))
OPS['CLGTI'] = cmp_w(lambda s, q: R(s, q['ra']) > U32(q['si10'] & 0xffffffff))
OPS['FCGT'] = cmp_w(lambda s, q: f(R(s, q['ra'])) > f(R(s, q['rb'])))
OPS['FCEQ'] = cmp_w(lambda s, q: f(R(s, q['ra'])) == f(R(s, q['rb'])))
OPS['FCMGT'] = cmp_w(lambda s, q: np.abs(f(R(s, q['ra']))) > np.abs(f(R(s, q['rb']))))
def cmp_h(fn):
    def h(s, q, pc): setr(s, q['rt'], from_halves(np.where(fn(s, q), np.uint16(0xffff), np.uint16(0))))
    return h
OPS['CEQH'] = cmp_h(lambda s, q: halves(R(s, q['ra'])) == halves(R(s, q['rb'])))
OPS['CEQHI'] = cmp_h(lambda s, q: halves(R(s, q['ra'])) == np.uint16(q['si10'] & 0xffff))
OPS['CGTH'] = cmp_h(lambda s, q: halves(R(s, q['ra'])).view(np.int16) > halves(R(s, q['rb'])).view(np.int16))
OPS['CGTHI'] = cmp_h(lambda s, q: halves(R(s, q['ra'])).view(np.int16) > q['si10'])
OPS['CLGTH'] = cmp_h(lambda s, q: halves(R(s, q['ra'])) > halves(R(s, q['rb'])))
OPS['CLGTHI'] = cmp_h(lambda s, q: halves(R(s, q['ra'])) > np.uint16(q['si10'] & 0xffff))
@op('CEQB')
def _(s, q, pc): a = tob(R(s, q['ra'])); b = tob(R(s, q['rb'])); setr(s, q['rt'], fromb(bytes(0xff if x == y else 0 for x, y in zip(a, b))))
@op('CEQBI')
def _(s, q, pc): a = tob(R(s, q['ra'])); setr(s, q['rt'], fromb(bytes(0xff if x == (q['si10'] & 0xff) else 0 for x in a)))
@op('CLGTB')
def _(s, q, pc): a = tob(R(s, q['ra'])); b = tob(R(s, q['rb'])); setr(s, q['rt'], fromb(bytes(0xff if x > y else 0 for x, y in zip(a, b))))
@op('CLGTBI')
def _(s, q, pc): a = tob(R(s, q['ra'])); setr(s, q['rt'], fromb(bytes(0xff if x > (q['si10'] & 0xff) else 0 for x in a)))
# ---- quadword shifts / rotates
def qint(r): return int.from_bytes(tob(r), 'big')
def qfrom(v): return fromb((v & ((1 << 128) - 1)).to_bytes(16, 'big'))
def rotl128(v, n): n %= 128; return ((v << n) | (v >> (128 - n))) & ((1 << 128) - 1) if n else v
@op('ROTQBY')
def _(s, q, pc): setr(s, q['rt'], qfrom(rotl128(qint(R(s, q['ra'])), 8 * (w0(R(s, q['rb'])) & 15))))
@op('ROTQBYI')
def _(s, q, pc): setr(s, q['rt'], qfrom(rotl128(qint(R(s, q['ra'])), 8 * (q['i7'] & 15))))
@op('ROTQBYBI')
def _(s, q, pc): setr(s, q['rt'], qfrom(rotl128(qint(R(s, q['ra'])), 8 * ((w0(R(s, q['rb'])) >> 3) & 15))))
@op('ROTQBI')
def _(s, q, pc): setr(s, q['rt'], qfrom(rotl128(qint(R(s, q['ra'])), w0(R(s, q['rb'])) & 7)))
@op('ROTQBII')
def _(s, q, pc): setr(s, q['rt'], qfrom(rotl128(qint(R(s, q['ra'])), q['i7'] & 7)))
@op('SHLQBY')
def _(s, q, pc): n = w0(R(s, q['rb'])) & 31; setr(s, q['rt'], qfrom(qint(R(s, q['ra'])) << (8 * n)) if n < 16 else np.zeros(4, U32))
@op('SHLQBYI')
def _(s, q, pc): n = q['i7'] & 31; setr(s, q['rt'], qfrom(qint(R(s, q['ra'])) << (8 * n)) if n < 16 else np.zeros(4, U32))
@op('SHLQBYBI')
def _(s, q, pc): n = (w0(R(s, q['rb'])) >> 3) & 31; setr(s, q['rt'], qfrom(qint(R(s, q['ra'])) << (8 * n)) if n < 16 else np.zeros(4, U32))
@op('SHLQBI')
def _(s, q, pc): setr(s, q['rt'], qfrom(qint(R(s, q['ra'])) << (w0(R(s, q['rb'])) & 7)))
@op('SHLQBII')
def _(s, q, pc): setr(s, q['rt'], qfrom(qint(R(s, q['ra'])) << (q['i7'] & 7)))
@op('ROTQMBY')
def _(s, q, pc): n = (-w0(R(s, q['rb']))) & 31; setr(s, q['rt'], qfrom(qint(R(s, q['ra'])) >> (8 * n)) if n < 16 else np.zeros(4, U32))
@op('ROTQMBYI')
def _(s, q, pc): n = (-q['i7']) & 31; setr(s, q['rt'], qfrom(qint(R(s, q['ra'])) >> (8 * n)) if n < 16 else np.zeros(4, U32))
@op('ROTQMBYBI')
def _(s, q, pc): n = (-(w0(R(s, q['rb'])) >> 3)) & 31; setr(s, q['rt'], qfrom(qint(R(s, q['ra'])) >> (8 * n)) if n < 16 else np.zeros(4, U32))
@op('ROTQMBI')
def _(s, q, pc): setr(s, q['rt'], qfrom(qint(R(s, q['ra'])) >> ((-w0(R(s, q['rb']))) & 7)))
@op('ROTQMBII')
def _(s, q, pc): setr(s, q['rt'], qfrom(qint(R(s, q['ra'])) >> ((-q['i7']) & 7)))
# ---- word shifts / rotates
def shl_w(a, n): n = n & 63; return (a << U32(n)) if n < 32 else np.zeros(4, U32)
@op('SHL')
def _(s, q, pc): a = R(s, q['ra']); b = R(s, q['rb']); setr(s, q['rt'], np.array([(int(a[i]) << (int(b[i]) & 63)) & 0xffffffff if (int(b[i]) & 63) < 32 else 0 for i in range(4)], dtype=U32))
@op('SHLI')
def _(s, q, pc): setr(s, q['rt'], shl_w(R(s, q['ra']), q['i7']))
@op('ROT')
def _(s, q, pc): a = R(s, q['ra']); b = R(s, q['rb']); setr(s, q['rt'], np.array([((int(a[i]) << (int(b[i]) & 31)) | (int(a[i]) >> (32 - (int(b[i]) & 31)))) & 0xffffffff if int(b[i]) & 31 else int(a[i]) for i in range(4)], dtype=U32))
@op('ROTI')
def _(s, q, pc): n = q['i7'] & 31; a = R(s, q['ra']); setr(s, q['rt'], ((a << U32(n)) | (a >> U32(32 - n))) if n else a.copy())
@op('ROTM')
def _(s, q, pc): a = R(s, q['ra']); b = R(s, q['rb']); setr(s, q['rt'], np.array([int(a[i]) >> ((-int(b[i])) & 63) if ((-int(b[i])) & 63) < 32 else 0 for i in range(4)], dtype=U32))
@op('ROTMI')
def _(s, q, pc): n = (-q['i7']) & 63; setr(s, q['rt'], (R(s, q['ra']) >> U32(n)) if n < 32 else np.zeros(4, U32))
@op('ROTMAI')
def _(s, q, pc): n = min((-q['i7']) & 63, 31); setr(s, q['rt'], (s32(R(s, q['ra'])) >> np.int32(n)).view(U32))
@op('ROTMA')
def _(s, q, pc): a = s32(R(s, q['ra'])); b = R(s, q['rb']); setr(s, q['rt'], np.array([int(a[i]) >> min((-int(b[i])) & 63, 31) for i in range(4)], dtype=np.int32).view(U32))
@op('ROTHMI')
def _(s, q, pc): n = (-q['i7']) & 31; setr(s, q['rt'], from_halves((halves(R(s, q['ra'])) >> np.uint16(n)) if n < 16 else np.zeros(8, np.uint16)))
@op('SHLHI')
def _(s, q, pc): n = q['i7'] & 31; setr(s, q['rt'], from_halves((halves(R(s, q['ra'])) << np.uint16(n)) if n < 16 else np.zeros(8, np.uint16)))
@op('ROTHI')
def _(s, q, pc): n = q['i7'] & 15; h = halves(R(s, q['ra'])); setr(s, q['rt'], from_halves((h << np.uint16(n)) | (h >> np.uint16(16 - n)) if n else h))
# ---- multiplies
def lo16s(r): return (r & U32(0xffff)).astype(np.uint16).view(np.int16).astype(np.int64)
def lo16u(r): return (r & U32(0xffff)).astype(np.int64)
@op('MPY')
def _(s, q, pc): setr(s, q['rt'], (lo16s(R(s, q['ra'])) * lo16s(R(s, q['rb']))).astype(np.int32).view(U32))
@op('MPYU')
def _(s, q, pc): setr(s, q['rt'], (lo16u(R(s, q['ra'])) * lo16u(R(s, q['rb']))).astype(U32))
@op('MPYI')
def _(s, q, pc): setr(s, q['rt'], (lo16s(R(s, q['ra'])) * q['si10']).astype(np.int32).view(U32))
@op('MPYUI')
def _(s, q, pc): setr(s, q['rt'], (lo16u(R(s, q['ra'])) * (q['si10'] & 0xffff)).astype(U32))
@op('MPYH')
def _(s, q, pc): setr(s, q['rt'], ((((R(s, q['ra']) >> U32(16)).astype(np.int64) * lo16u(R(s, q['rb']))) << 16) & 0xffffffff).astype(U32))
@op('MPYA')
def _(s, q, pc): setr(s, q['rt4'], ((lo16s(R(s, q['ra'])) * lo16s(R(s, q['rb'])) + s32(R(s, q['rc'])).astype(np.int64)) & 0xffffffff).astype(U32))
@op('MPYS')
def _(s, q, pc): setr(s, q['rt'], ((lo16s(R(s, q['ra'])) * lo16s(R(s, q['rb']))) >> 16).astype(np.int32).view(U32))
@op('MPYHH')
def _(s, q, pc): a = (R(s, q['ra']) >> U32(16)).astype(np.uint16).view(np.int16).astype(np.int64); b = (R(s, q['rb']) >> U32(16)).astype(np.uint16).view(np.int16).astype(np.int64); setr(s, q['rt'], (a * b).astype(np.int32).view(U32))
# ---- masks, inserts, extends
@op('FSM')
def _(s, q, pc): v = w0(R(s, q['ra'])); setr(s, q['rt'], np.array([0xffffffff if (v >> (3 - i)) & 1 else 0 for i in range(4)], dtype=U32))
@op('FSMH')
def _(s, q, pc): v = w0(R(s, q['ra'])); setr(s, q['rt'], from_halves([0xffff if (v >> (7 - i)) & 1 else 0 for i in range(8)]))
@op('FSMB')
def _(s, q, pc): v = w0(R(s, q['ra'])); setr(s, q['rt'], fromb(bytes(0xff if (v >> (15 - i)) & 1 else 0 for i in range(16))))
@op('GB')
def _(s, q, pc): a = R(s, q['ra']); setr(s, q['rt'], np.array([sum((int(a[i]) & 1) << (3 - i) for i in range(4)), 0, 0, 0], dtype=U32))
def cins(s, q, pos, width):
    o = bytearray(range(0x10, 0x20)); pos &= {1: 0xf, 2: 0xe, 4: 0xc, 8: 0x8}[width]
    o[pos:pos + width] = bytes(range(4 - width, 4)) if width <= 4 else bytes(range(8))
    setr(s, q['rt'], fromb(o))
@op('CBD')
def _(s, q, pc): cins(s, q, w0(R(s, q['ra'])) + q['si7'], 1)
@op('CHD')
def _(s, q, pc): cins(s, q, w0(R(s, q['ra'])) + q['si7'], 2)
@op('CWD')
def _(s, q, pc): cins(s, q, w0(R(s, q['ra'])) + q['si7'], 4)
@op('CDD')
def _(s, q, pc): cins(s, q, w0(R(s, q['ra'])) + q['si7'], 8)
@op('CBX')
def _(s, q, pc): cins(s, q, w0(R(s, q['ra'])) + w0(R(s, q['rb'])), 1)
@op('CHX')
def _(s, q, pc): cins(s, q, w0(R(s, q['ra'])) + w0(R(s, q['rb'])), 2)
@op('CWX')
def _(s, q, pc): cins(s, q, w0(R(s, q['ra'])) + w0(R(s, q['rb'])), 4)
@op('CDX')
def _(s, q, pc): cins(s, q, w0(R(s, q['ra'])) + w0(R(s, q['rb'])), 8)
@op('XSBH')
def _(s, q, pc): setr(s, q['rt'], from_halves((halves(R(s, q['ra'])) & np.uint16(0xff)).astype(np.uint8).view(np.int8).astype(np.int16).view(np.uint16)))
@op('XSHW')
def _(s, q, pc): setr(s, q['rt'], (R(s, q['ra']) & U32(0xffff)).astype(np.uint16).view(np.int16).astype(np.int32).view(U32))
@op('CLZ')
def _(s, q, pc): setr(s, q['rt'], np.array([32 - int(x).bit_length() for x in R(s, q['ra'])], dtype=U32))
@op('CNTB')
def _(s, q, pc): setr(s, q['rt'], fromb(bytes(bin(b).count('1') for b in tob(R(s, q['ra'])))))
# ---- float
@op('FA')
def _(s, q, pc): setr(s, q['rt'], fr(f(R(s, q['ra'])) + f(R(s, q['rb']))))
@op('FS')
def _(s, q, pc): setr(s, q['rt'], fr(f(R(s, q['ra'])) - f(R(s, q['rb']))))
@op('FM')
def _(s, q, pc): setr(s, q['rt'], fr(f(R(s, q['ra'])) * f(R(s, q['rb']))))
FUSED = True   # RPCS3 uses FMA3 instructions when the host has them
def d(r): return r.view(F32).astype(np.float64)
@op('FMA')
def _(s, q, pc): setr(s, q['rt4'], fr(d(R(s, q['ra'])) * d(R(s, q['rb'])) + d(R(s, q['rc']))) if FUSED else fr(f(R(s, q['ra'])) * f(R(s, q['rb'])) + f(R(s, q['rc']))))
@op('FMS')
def _(s, q, pc): setr(s, q['rt4'], fr(d(R(s, q['ra'])) * d(R(s, q['rb'])) - d(R(s, q['rc']))) if FUSED else fr(f(R(s, q['ra'])) * f(R(s, q['rb'])) - f(R(s, q['rc']))))
@op('FNMS')
def _(s, q, pc): setr(s, q['rt4'], fr(d(R(s, q['rc'])) - d(R(s, q['ra'])) * d(R(s, q['rb']))) if FUSED else fr(f(R(s, q['rc'])) - f(R(s, q['ra'])) * f(R(s, q['rb']))))
@op('FREST')
def _(s, q, pc):
    a = R(s, q['ra']); setr(s, q['rt'], np.array([LUT['spu_frest_fraction_lut'][(int(x) >> 18) & 0x1f] | LUT['spu_frest_exponent_lut'][(int(x) >> 23) & 0xff] | (int(x) & 0x80000000) for x in a], dtype=U32))
@op('FRSQEST')
def _(s, q, pc):
    a = R(s, q['ra']); setr(s, q['rt'], np.array([LUT['spu_frsqest_fraction_lut'][(int(x) >> 18) & 0x3f] | LUT['spu_frsqest_exponent_lut'][(int(x) >> 23) & 0xff] for x in a], dtype=U32))
@op('FI')
def _(s, q, pc):
    a = R(s, q['ra']); b = R(s, q['rb'])
    base = ((b & U32(0x007ffc00)) | U32(0x3f800000)).view(F32)
    step = (b & U32(0x3ff)).astype(np.int32).astype(F32) * F32(2.0 ** -13)
    y = (a & U32(0x7ffff)).astype(np.int32).astype(F32) * F32(2.0 ** -19)
    res = fr(base - step * y)
    setr(s, q['rt'], (b & U32(0xff800000)) | (res & U32(0x007fffff)))
def scale(n): return F32(2.0 ** n)
@op('CSFLT')
def _(s, q, pc): setr(s, q['rt'], fr(s32(R(s, q['ra'])).astype(F32) * scale(q['i8'] - 155)))
@op('CUFLT')
def _(s, q, pc):
    a = R(s, q['ra']); v = (a & U32(0x7fffffff)).astype(np.int32).astype(F32) + np.where(a >> U32(31), F32(2147483648.0), F32(0))
    setr(s, q['rt'], fr(v.astype(F32) * scale(q['i8'] - 155)))
@op('CFLTS')
def _(s, q, pc):
    x = (f(R(s, q['ra'])) * scale(173 - q['i8'])).astype(np.float64)
    setr(s, q['rt'], np.array([0x7fffffff if v >= 2147483648.0 else (0x80000000 if (v < -2147483648.0 or v != v) else int(v) & 0xffffffff) for v in x], dtype=U32))
@op('CFLTU')
def _(s, q, pc):
    x = np.maximum((f(R(s, q['ra'])) * scale(173 - q['i8'])).astype(np.float64), 0.0)
    setr(s, q['rt'], np.array([0xffffffff if v >= 4294967296.0 else (0 if v != v else int(v)) for v in x], dtype=U32))
# ---- branches
def target(pc, i16): return (pc + (spudis.sext(i16, 16) << 2)) & 0x3fffc
@op('BR')
def _(s, q, pc): return target(pc, q['i16'])
@op('BRA')
def _(s, q, pc): return (q['i16'] << 2) & 0x3fffc
@op('BRSL')
def _(s, q, pc): setr(s, q['rt'], np.array([pc + 4, 0, 0, 0], dtype=U32)); return target(pc, q['i16'])
@op('BRZ')
def _(s, q, pc): return target(pc, q['i16']) if w0(R(s, q['rt'])) == 0 else None
@op('BRNZ')
def _(s, q, pc): return target(pc, q['i16']) if w0(R(s, q['rt'])) != 0 else None
@op('BRHZ')
def _(s, q, pc): return target(pc, q['i16']) if (w0(R(s, q['rt'])) & 0xffff) == 0 else None
@op('BRHNZ')
def _(s, q, pc): return target(pc, q['i16']) if (w0(R(s, q['rt'])) & 0xffff) != 0 else None
@op('BI')
def _(s, q, pc): return w0(R(s, q['ra'])) & 0x3fffc
@op('BISL')
def _(s, q, pc): t = w0(R(s, q['ra'])) & 0x3fffc; setr(s, q['rt'], np.array([pc + 4, 0, 0, 0], dtype=U32)); return t
@op('BIZ')
def _(s, q, pc): return (w0(R(s, q['ra'])) & 0x3fffc) if w0(R(s, q['rt'])) == 0 else None
@op('BINZ')
def _(s, q, pc): return (w0(R(s, q['ra'])) & 0x3fffc) if w0(R(s, q['rt'])) != 0 else None
@op('BIHZ')
def _(s, q, pc): return (w0(R(s, q['ra'])) & 0x3fffc) if (w0(R(s, q['rt'])) & 0xffff) == 0 else None
@op('BIHNZ')
def _(s, q, pc): return (w0(R(s, q['ra'])) & 0x3fffc) if (w0(R(s, q['rt'])) & 0xffff) != 0 else None
@op('NOP', 'LNOP', 'HBR', 'HBRR', 'HBRA', 'SYNC', 'DSYNC')
def _(s, q, pc): return None
@op('HEQI')
def _(s, q, pc):
    if s32(R(s, q['ra']))[0] == q['si10']: raise Halt(f'HEQI halt at 0x{pc:05x}')
@op('STOP', 'STOPD')
def _(s, q, pc): raise Halt(f'STOP at 0x{pc:05x}')
# ---- channels
@op('WRCH')
def _(s, q, pc):
    v = w0(R(s, q['rt'])); s.ch[q['ra']] = v
    if s.on_wrch: return s.on_wrch(s, q['ra'], v, pc)
@op('RDCH')
def _(s, q, pc):
    v = s.on_rdch(s, q['ra'], pc) if s.on_rdch else 0
    setr(s, q['rt'], np.array([v, 0, 0, 0], dtype=U32))
@op('RCHCNT')
def _(s, q, pc): setr(s, q['rt'], np.array([1, 0, 0, 0], dtype=U32))

@op('ORX')
def _(s, q, pc): a = R(s, q['ra']); setr(s, q['rt'], np.array([a[0] | a[1] | a[2] | a[3], 0, 0, 0], dtype=U32))
@op('ROTH')
def _(s, q, pc):
    a = halves(R(s, q['ra'])).astype(np.uint32); n = halves(R(s, q['rb'])).astype(np.uint32) & 15
    setr(s, q['rt'], from_halves((((a << n) | (a >> (16 - n))) & 0xffff).astype(np.uint16)))
@op('BRASL')
def _(s, q, pc): setr(s, q['rt'], np.array([pc + 4, 0, 0, 0], dtype=U32)); return (q['i16'] << 2) & 0x3fffc
@op('IRET')
def _(s, q, pc): return getattr(s, 'srr0', 0) & 0x3fffc
@op('ORHI')
def _(s, q, pc): setr(s, q['rt'], from_halves(halves(R(s, q['ra'])) | np.uint16(q['si10'] & 0xffff)))

# ---- SPU float corner cases, following RPCS3's interpreter (exponent 255 is an ordinary large number, denormals are zero)
EXP = U32(0x7f800000)
def _ext(r): return (r & EXP) == EXP
def _den(r): return (r & EXP) == 0
def _lowered(r):
    r = np.where(_ext(r), r & U32(0xff7fffff), r); return np.where(_den(r), U32(0), r).astype(U32)
def _fcgt(s, q): return f(_lowered(R(s, q['ra']))) > f(_lowered(R(s, q['rb'])))
def _fcmgt(s, q):
    a = R(s, q['ra']); b = R(s, q['rb']); ea = _ext(a); eb = _ext(b)
    fa = np.where(_den(a), U32(0), a).astype(U32) & U32(0x7fffffff); fb = np.where(_den(b), U32(0), b).astype(U32) & U32(0x7fffffff)
    return (f(fa) > f(fb)) | (ea & ~eb)
OPS['FCGT'] = cmp_w(_fcgt)
OPS['FCMGT'] = cmp_w(_fcmgt)
def _mask_ext(r): return np.where(_ext(r), U32(0), r).astype(U32)
@op('FM')
def _(s, q, pc):
    a = R(s, q['ra']); b = R(s, q['rb']); p = fr(f(a) * f(b)); dop = _den(a) | _den(b)
    flushed = np.where(_den(p) | dop, U32(0), p); ext = np.where(dop, U32(0), ((a ^ b) & U32(0x80000000)) | (p & U32(0x7fffffff)))
    setr(s, q['rt'], np.where(_ext(p), ext, flushed).astype(U32))
def _fma3(s, q, kind):
    a = _mask_ext(R(s, q['ra'])); b = _mask_ext(R(s, q['rb'])); c = R(s, q['rc'])
    if FUSED:
        m = d(a) * d(b); cc = d(c)
        return fr(m + cc) if kind == 'FMA' else fr(m - cc) if kind == 'FMS' else fr(cc - m)
    m = f(a) * f(b); cc = f(c)
    return fr(m + cc) if kind == 'FMA' else fr(m - cc) if kind == 'FMS' else fr(cc - m)
@op('FMA')
def _(s, q, pc): setr(s, q['rt4'], _fma3(s, q, 'FMA'))
@op('FMS')
def _(s, q, pc): setr(s, q['rt4'], _fma3(s, q, 'FMS'))
@op('FNMS')
def _(s, q, pc): setr(s, q['rt4'], _fma3(s, q, 'FNMS'))

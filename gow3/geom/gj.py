#!/usr/bin/env python3
"""gj.py: replay of captured geometry jobs (RPCS3_SPU_JOB_CAPTURE) in the Python SPU interpreter.
   load(path) -> Job; Job.run() executes from the job manager's input GET to its next job fetch and checks every PUT
   against the capture. As a script: gj.py <capture file> [...]: run and report."""
import sys, struct, pathlib, collections
sys.path.insert(0, '/home/tiago/Applications/rpcs3/profiling/native-investigation/build-prep/source/infamous2/reference/spu')
import numpy as np, spuemu, spudis
U32 = np.uint32
def _rchcnt(s, q, pc): spuemu.setr(s, q['rt'], np.array([0 if q['ra'] in (0, 25, 27, 29) else 1, 0, 0, 0], dtype=U32))
spuemu.OPS['RCHCNT'] = _rchcnt
# ---- instructions the inFamous work did not need
_R, _set, _tob, _fromb, _halves, _fh = spuemu.R, spuemu.setr, spuemu.tob, spuemu.fromb, spuemu.halves, spuemu.from_halves
def _op(name):
    def deco(fn): spuemu.OPS[name] = fn; return fn
    return deco
@_op('GBB')
def _(s, q, pc): b = _tob(_R(s, q['ra'])); _set(s, q['rt'], [sum((b[i] & 1) << (15 - i) for i in range(16)), 0, 0, 0])
@_op('GBH')
def _(s, q, pc): h = _halves(_R(s, q['ra'])); _set(s, q['rt'], [sum((int(h[i]) & 1) << (7 - i) for i in range(8)), 0, 0, 0])
@_op('ABSDB')
def _(s, q, pc): a = _tob(_R(s, q['ra'])); b = _tob(_R(s, q['rb'])); _set(s, q['rt'], _fromb(bytes(abs(x - y) for x, y in zip(a, b))))
@_op('XSWD')
def _(s, q, pc): a = _R(s, q['ra']); _set(s, q['rt'], [0xffffffff if a[1] & 0x80000000 else 0, a[1], 0xffffffff if a[3] & 0x80000000 else 0, a[3]])
@_op('SFHI')
def _(s, q, pc): _set(s, q['rt'], _fh(np.uint16(q['si10'] & 0xffff) - _halves(_R(s, q['ra']))))
@_op('SUMB')
def _(s, q, pc):
    a = _tob(_R(s, q['ra'])); b = _tob(_R(s, q['rb'])); _set(s, q['rt'], [(sum(b[i * 4:i * 4 + 4]) << 16) | sum(a[i * 4:i * 4 + 4]) for i in range(4)])
@_op('CGTBI')
def _(s, q, pc):
    i = q['si10'] & 0xff; i = i - 256 if i & 0x80 else i
    _set(s, q['rt'], _fromb(bytes(0xff if (x - 256 if x & 0x80 else x) > i else 0 for x in _tob(_R(s, q['ra'])))))
@_op('XORBI')
def _(s, q, pc): _set(s, q['rt'], _fromb(bytes(x ^ (q['si10'] & 0xff) for x in _tob(_R(s, q['ra'])))))
def _hshift(a, n, arith):
    out = []
    for x, k in zip(a, n):
        x = int(x); k = (0 - int(k)) & 0x1f
        if arith: x = x - 0x10000 if x & 0x8000 else x; out.append((x >> min(k, 15)) & 0xffff)
        else: out.append((x >> k) & 0xffff if k < 16 else 0)
    return _fh(np.array(out, dtype=np.uint16))
@_op('ROTHM')
def _(s, q, pc): _set(s, q['rt'], _hshift(_halves(_R(s, q['ra'])), _halves(_R(s, q['rb'])), False))
@_op('ROTMAH')
def _(s, q, pc): _set(s, q['rt'], _hshift(_halves(_R(s, q['ra'])), _halves(_R(s, q['rb'])), True))
@_op('ROTMAHI')
def _(s, q, pc): _set(s, q['rt'], _hshift(_halves(_R(s, q['ra'])), [q['i7']] * 8, True))
@_op('MPYHHU')
def _(s, q, pc): _set(s, q['rt'], (_R(s, q['ra']) >> U32(16)) * (_R(s, q['rb']) >> U32(16)))
@_op('FCMEQ')
def _(s, q, pc):
    a = (_R(s, q['ra']) & U32(0x7fffffff)).view(np.float32); b = (_R(s, q['rb']) & U32(0x7fffffff)).view(np.float32)
    _set(s, q['rt'], np.where(a == b, U32(0xffffffff), U32(0)))
# ---- float arithmetic with the SPU's number range. An exponent of 255 is not infinity or NaN but a number up to
# 2^129, and results saturate at the largest magnitude; the job uses multiply-subtract on all-ones masks and relies
# on that. Operands in the host's range take the interpreter's usual path.
_EXP = U32(0x7f800000)
def _xd(r):
    r = np.asarray(r, dtype=U32); e = ((r >> U32(23)) & U32(255)).astype(np.int64); m = (r & U32(0x7fffff)).astype(np.float64)
    v = (1.0 + m / 8388608.0) * np.exp2((e - 127).astype(np.float64)); v = np.where(e == 0, 0.0, v)
    return np.where(r >> U32(31), -v, v)
def _xf(v):
    out = np.zeros(4, dtype=U32)
    for i, x in enumerate(v):
        a = abs(float(x))
        if a < 2.0 ** -126: continue
        if a >= 2.0 ** 129: out[i] = 0x7fffffff | (0x80000000 if x < 0 else 0); continue
        import math
        m, e = math.frexp(a); e -= 1; frac = int((m * 2 - 1) * 8388608)
        out[i] = ((e + 127) << 23) | frac | (0x80000000 if x < 0 else 0)
    return out
def _ext(*rs): return any(bool(((np.asarray(r, dtype=U32) & _EXP) == _EXP).any()) for r in rs)
def _wrap(name, fn, regs):
    plain = spuemu.OPS[name]
    def h(s, q, pc):
        ops = [_R(s, q[k]) for k in regs]
        if _ext(*ops): _set(s, q['rt4' if 'rc' in regs else 'rt'], _xf(fn(*[_xd(o) for o in ops])))
        else:
            plain(s, q, pc)
            t = q['rt4' if 'rc' in regs else 'rt']
            if _ext(_R(s, t)): _set(s, t, _xf(fn(*[_xd(o) for o in ops])))
    spuemu.OPS[name] = h
_wrap('FA', lambda a, b: a + b, ('ra', 'rb'))
_wrap('FS', lambda a, b: a - b, ('ra', 'rb'))
_wrap('FM', lambda a, b: a * b, ('ra', 'rb'))
_wrap('FMA', lambda a, b, c: a * b + c, ('ra', 'rb', 'rc'))
_wrap('FMS', lambda a, b, c: a * b - c, ('ra', 'rb', 'rc'))
_wrap('FNMS', lambda a, b, c: c - a * b, ('ra', 'rb', 'rc'))
class Job:
    def __init__(self, path):
        d = pathlib.Path(path).read_bytes(); assert d[:4] == b'SPUK'
        self.path = str(path)
        self.pc, self.eal, self.lsa, self.size, self.srr0, self.ie = struct.unpack_from('<6I', d, 4); o = 28
        g = np.frombuffer(d, dtype='<u4', count=512, offset=o).reshape(128, 4); o += 2048
        self.gpr = [g[i][::-1].astype(U32).copy() for i in range(128)]
        self.ls = bytearray(d[o:o + 0x40000]); o += 0x40000
        self.elems = []; self.puts = []; self.gets = []; self.atoms = []; self.lsd = []
        while o < len(d):
            tag = d[o:o + 4]; o += 4
            if tag == b'ELEM':
                ea, size = struct.unpack_from('<2I', d, o); o += 8; self.elems.append((ea, size, d[o:o + size])); o += size
            elif tag in (b'PUT ', b'GET '):
                pc, ea, lsa, size = struct.unpack_from('<4I', d, o); o += 16
                (self.puts if tag == b'PUT ' else self.gets).append((pc, ea, lsa, size, d[o:o + size])); o += size
            elif tag == b'LSD ':
                self.lsd.append((len(self.puts) - 1, d[o:o + 2048], d[o + 2048:o + 2048 + 0x40000])); o += 2048 + 0x40000
            elif tag == b'ATOM':
                pc, cmd, ea, lsa, status = struct.unpack_from('<5I', d, o); o += 20; self.atoms.append((pc, cmd, ea, lsa, status, d[o:o + 128])); o += 128
            else: raise ValueError(tag)
    def following(self, n=3):
        p = pathlib.Path(self.path); k = int(p.stem.split('-')[1]); out = []
        for i in range(1, n + 1):
            q = p.with_name('job-%04d.bin' % (k + i))
            if q.exists(): out.append(Job(q))
        return out
    def spu(self):
        """The interpreter at the capture point, inside the job manager's fetch of the next job's inputs while a job
        runs. phase 0: the rest of the running job; 1: the next job, from its entry to its return; 2: over.
        s.out[phase] collects the PUTs. GETs and atomic commands are answered from the capture stream (this file and
        the following ones) by pc; interrupts are not delivered."""
        s = spuemu.SPU(self.ls, self.gpr, self.pc); s.out = [[], [], []]; s.phase = 0; s.srr0 = self.srr0; s.notes = []
        jobs = [self] + self.following()
        s.lists = [j.elems for j in jobs]; s.getq = [g for j in jobs for g in j.gets]; s.atomq = [a for j in jobs for a in j.atoms]
        s.refputs = [x for j in jobs for x in j.puts]; s.status = 0
        def take(q, match, what):
            # the pc in the capture is the last one the recompiled code stored, so records are matched by address
            for i, r in enumerate(q):
                if match(r):
                    if i: s.notes.append('%s: skipped %d records' % (what, i))
                    del q[:i + 1]; return r
            s.notes.append('%s: no record' % what); return None
        def wrch(s, ch, v, pc):
            if ch == 14: s.srr0 = v; return
            if ch != 21: return
            cmd = v & ~3 if v not in (0xd0, 0xb4, 0xb0) else v
            lsa, eal, size = s.ch.get(16, 0), s.ch.get(18, 0), s.ch.get(19, 0)
            if cmd == 0x44:                       # list GET
                if not s.lists: s.phase = 2; return pc
                elems = iter(s.lists.pop(0))
                for i in range(0, size, 8):
                    e = s.ls[(eal + i) & 0x3fff8:((eal + i) & 0x3fff8) + 8]; esz = (e[2] << 8) | e[3]; ea = struct.unpack('>I', e[4:8])[0]
                    cea, csz, data = next(elems)
                    if (cea, csz) != (ea, esz): s.notes.append('list element %x %x, capture has %x %x' % (ea, esz, cea, csz)); s.phase = 2; return pc
                    a = lsa | (ea & 15); s.ls[a:a + esz] = data; lsa += (esz + 15) & ~15
                    if e[0] & 0x80: break
                s.cache.clear()
            elif cmd == 0x20: s.out[s.phase].append((pc, eal, lsa, size, bytes(s.ls[lsa:lsa + size])))
            elif cmd == 0x40 and size == 0: pass
            elif cmd == 0x40:
                r = take(s.getq, lambda r: (r[1], r[3]) == (eal, size), 'GET %x %x at %x' % (eal, size, pc))
                if r is None: s.notes.append('GET %x %x does not match the capture' % (eal, size)); s.phase = 2; return pc
                s.ls[lsa:lsa + size] = r[4]; s.cache.clear()
            elif cmd in (0xd0, 0xb4, 0xb0):
                r = take(s.atomq, lambda r: r[1] == cmd and r[2] & ~127 == eal & ~127, 'atomic %x %x at %x' % (cmd, eal, pc))
                if r is None: s.notes.append('atomic %x at %x does not match the capture' % (cmd, pc)); s.phase = 2; return pc
                if cmd == 0xd0: a = lsa & 0x3ff80; s.ls[a:a + 128] = r[5]; s.cache.clear()
                s.status = r[4]
            else: raise spuemu.Halt('MFC command 0x%x at 0x%x' % (v, pc))
        s.on_wrch = wrch
        s.on_rdch = lambda s, ch, pc: {24: s.ch.get(22, 0), 27: s.status, 15: s.srr0}.get(ch, 0)
        return s
    def run(self, hook=None, limit=30_000_000):
        """Run the job that was interrupted by the capture to its return to the job manager. The interrupt comes
        right after the job's first call (0x4bc4), so this is all of the job's work. hook(s) is called before every
        instruction of the job."""
        if self.srr0 != 0x4bd8: raise ValueError('this capture was not taken inside a job (srr0 %x)' % self.srr0)
        s = self.spu(); s.step()
        while s.count < limit and s.phase == 0:
            pc = s.pc
            if pc == 0x32b0: s.phase = 1; break
            if hook and pc >= 0x4000 and pc < 0x3f000: hook(s)
            s.step()
        return s
    def check(self, s):
        """Differences between the replay's PUTs and the capture's"""
        return Job.diff(s.out[0], self.puts[:len(s.out[0])])
    @staticmethod
    def diff(mine, ref):
        bad = []
        for i, (a, b) in enumerate(zip(mine, ref)):
            if a[1:4] != b[1:4]: bad.append('put %d header %s vs %s' % (i, [hex(x) for x in a[:4]], [hex(x) for x in b[:4]]))
            elif a[4] != b[4]:
                n = sum(x != y for x, y in zip(a[4], b[4])); bad.append('put %d pc %x ea %x size %x: %d bytes differ' % (i, a[0], a[1], a[3], n))
        if len(mine) != len(ref): bad.append('puts %d vs %d' % (len(mine), len(ref)))
        return bad
def _iret(s, q, pc): return s.srr0 & 0x3fffc
spuemu.OPS['IRET'] = _iret
spuemu.FUSED = True
if __name__ == '__main__':
    import time
    for p in sys.argv[1:]:
        j = Job(p); t = time.time()
        s = j.run(); c = j.check(s)
        print(p.rsplit('/', 1)[-1], 'instructions', s.count, 'seconds %.1f' % (time.time() - t), 'returned' if s.phase == 1 else 'did not return', s.notes, 'puts', [hex(x[3]) for x in s.out[0]], 'check:', c or 'identical')

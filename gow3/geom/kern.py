#!/usr/bin/env python3
"""kern.py <capture dir> [max jobs] [kernel names...]: prototypes of host replacements for hot regions of the geometry
job, tested in the replay. A kernel takes over at its entry pc, changes the local store and the registers the code
after it needs, and continues at its exit pc; every other register the region writes is filled with a poison value,
so that a wrong assumption about what is needed afterwards shows. The job's PUTs are then compared with the
reference replay (same interpreter without kernels)."""
import sys, glob, struct, collections, time, numpy as np; sys.path.insert(0, __file__.rsplit('/', 1)[0])
import gj, live
U32 = np.uint32; F32 = np.float32
def w0(s, r): return int(s.r[r][0])
def ptr(s, r): return int(s.r[r][0]) & 0x3fff0
def setw(s, r, v): s.r[r] = np.array([v & 0xffffffff, 0, 0, 0], dtype=U32)
def rdf(s, a, n): return np.frombuffer(bytes(s.ls[a:a + 16 * n]), dtype='>f4').reshape(n, 4).astype(F32)
def wrf(s, a, v): s.ls[a:a + v.size * 4] = np.ascontiguousarray(v, dtype='>f4').tobytes(); s.cache.clear()
def fma(a, b, c): return (a.astype(np.float64) * b.astype(np.float64) + c.astype(np.float64)).astype(F32)
KERNELS = {}
def kernel(name, entry, exit_):
    def deco(fn): KERNELS[name] = (entry, exit_, fn); return fn
    return deco

@kernel('skin4', 0x10d74, 0x10ff8)
def skin4(s):
    """Weighted skinning of positions, up to four bones per vertex. Per vertex an 8-byte record of four halfwords
    (weight byte, bone index byte); bones are 48 bytes, three vec4 rows in a rotated layout (see blend below). Writes
    the blended matrix of every vertex (48 bytes) to a scratch array and the transformed position in place."""
    n = w0(s, 63); rec = ptr(s, 83); pos = ptr(s, 86); out = ptr(s, 67); base = ptr(s, 89)
    scale = s.r[69].view(F32)[0]
    r = np.frombuffer(bytes(s.ls[rec:rec + 8 * n]), dtype=np.uint8).reshape(n, 4, 2)
    wgt = (r[:, :, 0].astype(F32) * scale).astype(F32); idx = r[:, :, 1].astype(np.int64)
    bones = np.frombuffer(bytes(s.ls[base:base + 48 * 256 + 48]), dtype='>f4').reshape(-1, 3, 4).astype(F32)
    m = bones[idx]                                            # n, 4 influences, 3 rows, 4
    acc = (wgt[:, 0, None, None] * m[:, 0]).astype(F32)
    for k in (1, 2, 3): acc = fma(wgt[:, k, None, None], m[:, k], acc)
    p = rdf(s, pos, n)
    t = acc[:, :, 3].copy(); t3 = np.concatenate([t, np.zeros((n, 1), F32)], axis=1)   # translation: the three w components
    r0 = acc[:, 0].copy(); r0[:, 3] = 1.0; r1 = acc[:, 1].copy(); r1[:, 3] = 0.0; r2 = acc[:, 2].copy(); r2[:, 3] = 0.0
    o = fma(p, r0, t3); o = fma(p[:, [1, 2, 0, 3]], r1, o); o = fma(p[:, [2, 0, 1, 3]], r2, o)
    wrf(s, out, np.stack([r0, r1, r2], axis=1)); wrf(s, pos, o)
    s.r[73] = s.r[83] + (s.r[63] << U32(3)); s.r[86] = s.r[86] + U32(16 * n)

@kernel('skin_streams', 0x111bc, 0x1160c)
def skin_streams(s):
    """After the positions: one, two or three more vec4 streams (normal, tangent, ...) of the same vertices are
    transformed by the blended matrix that skin4 stored per vertex, without the translation; w stays (the stored rows
    have w = 1, 0, 0). r66 = number of streams, r62, r65, r68 the streams, r39 the matrices, r63 the count."""
    mode = w0(s, 66); n = w0(s, 63)
    if mode not in (1, 2, 3): return
    m = np.frombuffer(bytes(s.ls[ptr(s, 39):ptr(s, 39) + 48 * n]), dtype='>f4').reshape(n, 3, 4).astype(F32)
    for reg in (62, 65, 68)[:mode]:
        a = ptr(s, reg); v = rdf(s, a, n)
        o = (v * m[:, 0]).astype(F32); o = fma(v[:, [1, 2, 0, 3]], m[:, 1], o); o = fma(v[:, [2, 0, 1, 3]], m[:, 2], o)
        wrf(s, a, o); s.r[reg] = s.r[reg] + U32(16 * n)

@kernel('transform4', 0x11740, None)
def transform4(s):
    """11740(r3 = count, r4 = source, r5 = destination, r6 = 4x4 matrix): destination[i] = matrix * source[i] for
    count vec4 (rounded up to 8), rows of the matrix dotted with the vector. Source and destination may be the same."""
    n = (w0(s, 3) + 7) & ~7; m = rdf(s, ptr(s, 6), 4); v = rdf(s, ptr(s, 4), n); k = np.arange(4)
    o = (v * m[k, k]).astype(F32)
    for j in (1, 2, 3): o = fma(v[:, (k + j) % 4], m[k, (k + j) % 4][None, :], o)
    wrf(s, ptr(s, 5), o)

@kernel('binormals', 0xc168, 0xc520)
def binormals(s):
    """Third basis vector of every vertex: destination = normalize(cross(B, A)) * B.w, w = 0, for r83 vertices
    (rounded up to 8); r81 = A, r82 = B, r45 = destination. A zero cross product gives zero."""
    n = np.int32(w0(s, 83))
    if n <= 0: return
    n = (int(n) + 7) & ~7; a = rdf(s, ptr(s, 81), n); b = rdf(s, ptr(s, 82), n); yzx = [1, 2, 0]; zxy = [2, 0, 1]
    # single precision in the SPU code's order, so that vertices with parallel inputs get the same rounding residue
    t = (b[:, yzx] * a[:, zxy]).astype(F32); c = fma(-b[:, zxy], a[:, yzx], t); c = (c * b[:, 3:4]).astype(F32)
    d = (c.astype(np.float64) ** 2).sum(axis=1)
    with np.errstate(all='ignore'): rs = np.where(d > 1e-37, 1.0 / np.sqrt(d), 0.0)
    o = np.zeros((n, 4), F32); o[:, :3] = c * rs[:, None]; wrf(s, ptr(s, 45), o)

@kernel('pack_xyz', 0x5cb0, 0x5de8)
def pack_xyz(s):
    """Output format 0 of the vertex packer: the xyz of r60 positions (vec4 at the second word of r5) as 12 bytes each
    at the first word of r5. The SPU loop leaves 16 bytes of leftovers after them; zeros here."""
    n = w0(s, 60); dst = w0(s, 5) & 0x3ffff; src = int(s.r[5][1]) & 0x3fff0
    v = np.frombuffer(bytes(s.ls[src:src + 16 * n]), dtype=np.uint8).reshape(n, 16)[:, :12]
    s.ls[dst:dst + 12 * n] = v.tobytes(); s.ls[dst + 12 * n:dst + 12 * n + 16] = bytes(16); s.cache.clear()

def stream_of(s, attribute):
    """Address of the vec4 stream that holds a vertex attribute: the job context (r12) lists the attribute ids at
    +352 and the stream addresses as words from +16 (r39)."""
    ctx = ptr(s, 12); ids = bytes(s.ls[ctx + 352:ctx + 368]); i = ids.index(attribute); t = ptr(s, 39) + 4 * i
    return struct.unpack('>I', s.ls[t:t + 4])[0] & 0x3fff0
def q16(v):
    """float to signed 16 bits as the packer does: floor(v * 32767.5 - 0.5), saturated"""
    t = fma(v, np.float32(32767.5), np.float32(-0.5)).astype(np.float64) * 65536.0
    return (np.clip(np.trunc(t), -2 ** 31, 2 ** 31 - 1).astype(np.int64) >> 16).astype('>i2')
def pack_11_11_10(v):
    """xyz to 32 bits: x and y in 11 bits (low and middle), z in the top 10"""
    def field(c, bits):
        m = float(2 ** bits); t = fma(c, np.float32((m - 1) / m), np.float32(-1 / m)).astype(np.float64) * 2.0 ** 31
        return (np.clip(np.trunc(t), -2 ** 31, 2 ** 31 - 1).astype(np.int64) >> (32 - bits)) & (2 ** bits - 1)
    return (field(v[:, 2], 10) << 22 | field(v[:, 1], 11) << 11 | field(v[:, 0], 11)).astype('>u4')

@kernel('pack36', 0x5ea8, 0x5de8)
def pack36(s):
    """Output format 7 of the vertex packer, 36 bytes a vertex: position (attribute 1) as three floats, attribute 3
    (normal) as four signed 16-bit values, attribute 4 (tangent) in 11:11:10 bits, attribute 0x21 as three floats."""
    n = w0(s, 60); dst = w0(s, 5) & 0x3ffff
    p = rdf(s, stream_of(s, 1), n); nrm = rdf(s, stream_of(s, 3), n); tan = rdf(s, stream_of(s, 4), n); p2 = rdf(s, stream_of(s, 0x21), n)
    rec = np.zeros((n, 36), np.uint8)
    rec[:, 0:12] = np.frombuffer(p[:, :3].astype('>f4').tobytes(), np.uint8).reshape(n, 12)
    rec[:, 12:20] = np.frombuffer(q16(nrm).tobytes(), np.uint8).reshape(n, 8)
    rec[:, 20:24] = np.frombuffer(pack_11_11_10(tan).tobytes(), np.uint8).reshape(n, 4)
    rec[:, 24:36] = np.frombuffer(p2[:, :3].astype('>f4').tobytes(), np.uint8).reshape(n, 12)
    s.ls[dst:dst + 36 * n] = rec.tobytes(); s.cache.clear()

def half(v):
    """float to half as the packer does: mantissa truncated, too small gives 0 without sign, too large the largest"""
    u = np.ascontiguousarray(v, dtype=F32).view(U32).astype(np.int64); sign = (u >> 16) & 0x8000; e = ((u >> 23) & 255) - 112; m = (u >> 13) & 0x3ff
    return np.where(e < 0, 0, sign | np.where(e >= 31, 0x7bff, (e << 10) | m)).astype('>u2')

@kernel('pack54', 0x66f0, 0x5de8)
def pack54(s):
    """Output format 4 of the vertex packer, 54 bytes a vertex: as format 7 with attribute 0x1d in place of 0x21,
    then attributes 0x1e, 0x1f and 0x20 as three half floats each."""
    n = w0(s, 60); dst = w0(s, 5) & 0x3ffff
    get = lambda a: rdf(s, stream_of(s, a), n)
    rec = np.zeros((n, 54), np.uint8); b = lambda x, w: np.frombuffer(x.tobytes(), np.uint8).reshape(n, w)
    rec[:, 0:12] = b(get(1)[:, :3].astype('>f4'), 12); rec[:, 12:20] = b(q16(get(3)), 8); rec[:, 20:24] = b(pack_11_11_10(get(4)), 4)
    rec[:, 24:36] = b(get(0x1d)[:, :3].astype('>f4'), 12)
    for k, a in enumerate((0x1e, 0x1f, 0x20)): rec[:, 36 + 6 * k:42 + 6 * k] = b(half(get(a)[:, :3]), 6)
    s.ls[dst:dst + 54 * n] = rec.tobytes(); s.cache.clear()

@kernel('point_lights', 0x8fd8, None)
def point_lights(s):
    """8fd8(r3 = destination, r4 = positions, r5 = vertex count, r6 = light table): per vertex the sum over a range
    of point lights of colour * (colour.w * f * f), f = smoothstep(1 - min(distance * light.w, 1)). The table's
    first words give the offsets of the light positions and colours (vec4 arrays), its second line the range:
    first = word 0 + word 1, count = word 2. Vertices are done in eights; no lights gives zeros."""
    n = np.int32(w0(s, 5))
    if n <= 0: return
    n = (int(n) + 7) & ~7; t = ptr(s, 6); a, b = struct.unpack('>2I', s.ls[t:t + 8]); i0, i1, cnt = struct.unpack('>3I', s.ls[t + 16:t + 28]); first = i0 + i1
    out = np.zeros((n, 4), F32)
    if cnt:
        x = rdf(s, (t + a + 16 * first) & 0x3fff0, cnt).astype(np.float64); c = rdf(s, (t + b + 16 * first) & 0x3fff0, cnt).astype(np.float64); p = rdf(s, ptr(s, 4), n).astype(np.float64)
        d = np.sqrt(((p[:, None, :3] - x[None, :, :3]) ** 2).sum(axis=2)); sv = 1 - np.clip(d * x[None, :, 3], 0, 1); f = sv * sv * (3 - 2 * sv)
        out = (c[None, :, :] * (c[None, :, 3:4] * (f * f)[:, :, None])).sum(axis=1).astype(F32)
    wrf(s, ptr(s, 3), out)

@kernel('normalize', 0xdd50, None)
def normalize(s):
    """dd50(r3 = vec4 array, r4 = count): normalises the xyz of count vectors (rounded up to 8), w stays. The SPU code
    uses the reciprocal square root estimate alone (about 12 bits); a zero vector stays zero."""
    n = (w0(s, 4) + 7) & ~7; a = ptr(s, 3); v = rdf(s, a, n)
    d = (v[:, 0].astype(np.float64) ** 2 + v[:, 1].astype(np.float64) ** 2 + v[:, 2].astype(np.float64) ** 2)
    with np.errstate(all='ignore'): rs = np.where(d > 1e-37, 1.0 / np.sqrt(d), 0.0)
    o = v.copy(); o[:, :3] = (v[:, :3] * rs[:, None]).astype(F32); wrf(s, a, o)

def run(job, names, poison=True):
    s = job.spu(); s.step(); table = {KERNELS[k][0]: KERNELS[k] for k in names}; used = collections.Counter()
    written = {k: None for k in names}
    while s.count < 30_000_000 and s.phase == 0:
        pc = s.pc
        if pc == 0x32b0: s.phase = 1; break
        if pc in table:
            entry, exit_, fn = table[pc]
            if written.get(fn.__name__) is None:
                if exit_ is None: written[fn.__name__] = set(range(2, 80))       # a whole function: the volatile registers
                else: _, seen, succ = live.live_in(entry, exit_); written[fn.__name__] = set().union(*[succ[p][1] for p in seen])
            if exit_ is None: exit_ = int(s.r[0][0]) & 0x3fffc
            before = [x.copy() for x in s.r]; fn(s)
            for r in written[fn.__name__]:
                if poison and np.array_equal(s.r[r], before[r]): s.r[r] = np.full(4, 0xdeadbee0 + (r & 15), dtype=U32)
            s.pc = exit_; used[fn.__name__] += 1
            continue
        s.step()
    return s, used
def compare(a, b, tol=3e-3):
    """How two PUT lists differ: a text if their shape differs, else (words that differ, of them unexplained, total
    words). A differing word is explained when the floats are within tol, when no byte differs by more than 2
    (packed normals and the like), or when the reference value is junk from a padding vertex."""
    if len(a) != len(b): return 'puts %d vs %d' % (len(a), len(b))
    differ = unexplained = total = 0
    for x, y in zip(a, b):
        if x[1:4] != y[1:4]: return 'header %s vs %s' % ([hex(v) for v in x[1:4]], [hex(v) for v in y[1:4]])
        n = len(x[4]) // 4 * 4; total += n // 4
        if x[4] == y[4]: continue
        fx = np.frombuffer(x[4][:n], dtype='>f4').astype(np.float64); fy = np.frombuffer(y[4][:n], dtype='>f4').astype(np.float64)
        ix = np.frombuffer(x[4][:n], dtype='>u4'); iy = np.frombuffer(y[4][:n], dtype='>u4')
        bx = np.frombuffer(x[4][:n], dtype=np.uint8).reshape(-1, 4).astype(np.int16); by = np.frombuffer(y[4][:n], dtype=np.uint8).reshape(-1, 4).astype(np.int16)
        with np.errstate(all='ignore'):
            rel = np.abs(fx - fy) / np.maximum(np.maximum(np.abs(fx), np.abs(fy)), 1e-3)
            close = np.isfinite(rel) & (rel <= tol)
        packed = (np.minimum(np.abs(bx - by), 256 - np.abs(bx - by)) <= 2).all(axis=1)
        hx = np.frombuffer(x[4][:n], dtype='>u2').reshape(-1, 2).astype(np.int32); hy = np.frombuffer(y[4][:n], dtype='>u2').reshape(-1, 2).astype(np.int32)
        packed |= (np.minimum(np.abs(hx - hy), 65536 - np.abs(hx - hy)) <= 32).all(axis=1)      # 16-bit components
        junk = ~np.isfinite(fy) | (np.abs(fy) > 1e15)
        d = ix != iy; differ += int(d.sum()); unexplained += int((d & ~close & ~packed & ~junk).sum())
    return differ, unexplained, total
if __name__ == '__main__':
    files = sorted(glob.glob(sys.argv[1] + '/job-*.bin'))[:int(sys.argv[2]) if len(sys.argv) > 2 else 10000]
    names = sys.argv[3:] or list(KERNELS); t0 = time.time(); res = collections.Counter(); used = collections.Counter(); saved = 0; total = 0; bad = []; un = 0; words = 0
    for p in files:
        j = gj.Job(p)
        if j.srr0 != 0x4bd8: continue
        ref = j.run(); s, u = run(j, names); used.update(u); total += ref.count; saved += ref.count - s.count
        c = compare(s.out[0], ref.out[0])
        if isinstance(c, str) or s.phase != 1: res['differs'] += 1; bad.append((p[-12:], c, s.notes[:1]))
        else:
            un += c[1]; words += c[2]
            res['same' if c[0] == 0 else ('close' if c[1] == 0 else 'unexplained')] += 1
            if c[1]: bad.append((p[-12:], 'words differ %d, unexplained %d of %d' % c))
    print(dict(res), 'unexplained words %d of %d' % (un, words), 'kernel calls', dict(used), 'reference instructions replaced %.1f%%' % (saved * 100 / max(total, 1)), 'seconds %.0f' % (time.time() - t0))
    for b in bad[:8]: print('  ', b)

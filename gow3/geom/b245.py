#!/usr/bin/env python3
"""b245.py <capture dir>: prototypes of host kernels for the job b245f318 (bounding spheres), checked against the
interpreter on region captures. 59a0: groups of spheres moved by their bone matrices and one sphere around each group."""
import sys, glob, struct, numpy as np; sys.path.insert(0, __file__.rsplit('/', 1)[0])
import region
f32 = np.float32
def be_f(ls, a, n): return np.frombuffer(ls, dtype='>f4', count=n, offset=a).astype(f32)
def spheres_59a0(ls, groups, out, counts, tmp, items, mats):
    """returns {address: 4 floats} writes and the last r3 value (None: the SPU code's value is not reproduced)"""
    w = {}; last = None
    for g in range(groups):
        n = ls[counts + g]; lo = np.full(4, np.finfo(f32).max, f32); hi = -lo; t = []
        for i in range(n):
            p = be_f(ls, items, 3); b12, b13 = ls[items + 12], ls[items + 13]; h = struct.unpack_from('>H', ls, items + 14)[0]
            m = be_f(ls, mats + 64 * (b12 | (h >> 14) << 8), 16).reshape(4, 4)
            c = ((m[0] * p[0] + m[1] * p[1]) + m[2] * p[2]) + m[3]
            scale = np.sqrt((m[:3, :3] ** 2).sum(axis=1, dtype=f32)).max()
            radius = np.frombuffer(struct.pack('<I', b13 << 23 | (h & 0x3fff) << 9), dtype='<f4')[0]
            s = np.array([c[0], c[1], c[2], scale * radius], f32); t.append(s); w[tmp + 16 * i] = s
            lo = np.minimum(lo, s); hi = np.maximum(hi, s); items += 16
        centre = ((lo + hi) * f32(0.5)).astype(f32) if n else np.zeros(4, f32)
        far = f32(0)
        for s in t:
            d = (s - centre)[:3]; far = max(far, f32(np.sqrt((d * d).sum(dtype=f32))) + s[3])
        w[out + 16 * g] = np.array([centre[0], centre[1], centre[2], far], f32); last = far if n else None
    return w, last
if __name__ == '__main__':
    worst = 0; bad = 0; nolast = 0; calls = 0
    for p in sorted(glob.glob(sys.argv[1] + '/region-*.bin')):
        s = region.load(p); ls0 = bytes(s.ls); a = [int(s.r[i][0]) for i in range(3, 9)]
        w, last = spheres_59a0(ls0, *a); region.run_function(s); ls1 = bytes(s.ls); calls += 1
        for addr, v in w.items():
            ref = be_f(ls1, addr, 4); err = np.abs(ref - v) / np.maximum(np.abs(ref), 1e-3); worst = max(worst, float(err.max())); bad += int((err > 1e-3).sum())
        changed = {i for i in range(0, 0x40000, 16) if ls0[i:i + 16] != ls1[i:i + 16]}
        extra = changed - set(w); bad += len(extra)
        if last is None: nolast += 1
        else:
            r3 = np.frombuffer(struct.pack('>I', int(s.r[3][0])), dtype='>f4')[0]
            if abs(r3 - last) > 1e-3 * max(abs(r3), 1e-3): bad += 1
    print('calls', calls, 'worst relative error %.2e' % worst, 'mismatches', bad, 'calls ending in an empty group', nolast)

def pairs_4ef0(ls, na, nb, a, bits, b):
    """4ef0: bit i*nb+j (least significant first in each byte) is set where sphere i of A and sphere j of B overlap"""
    na &= 0xffff; nb &= 0xffff; nbytes = (na * nb + 7) >> 3
    out = bytearray(nbytes)
    if na and nb:
        A = np.frombuffer(ls, dtype='>f4', count=4 * na, offset=a).astype(f32).reshape(na, 4)
        B = np.frombuffer(ls, dtype='>f4', count=4 * nb, offset=b).astype(f32).reshape(nb, 4)
        d = A[:, None, :3] - B[None, :, :3]; d = d * d
        d2 = (d[..., 0] + d[..., 1]) + d[..., 2]
        r = B[None, :, 3] + A[:, None, 3]
        hit = ((r * r) > d2).reshape(-1)
        packed = np.packbits(hit, bitorder='little')
        out[:len(packed)] = packed.tobytes()
    return bytes(out)
def check_4ef0(d):
    import collections
    bad = 0; calls = 0; sizes = collections.Counter(); instr = 0
    for p in sorted(glob.glob(d + '/region-*.bin')):
        s = region.load(p); ls0 = bytes(s.ls); a = [int(s.r[i][0]) for i in range(3, 8)]
        out = pairs_4ef0(ls0, *a); region.run_function(s); ls1 = bytearray(s.ls); calls += 1; instr += s.count
        want = bytearray(ls0); want[a[3]:a[3] + len(out)] = out
        sizes[(a[0] & 0xffff, a[1] & 0xffff)] += 1
        if want != ls1 or int(s.r[3][0]) != (a[0] & 0xffff) * (a[1] & 0xffff): bad += 1
    print('calls', calls, 'not identical', bad, 'instructions per call', instr // calls, 'common sizes', sizes.most_common(6))
if __name__ == '__main__' and len(sys.argv) > 2: check_4ef0(sys.argv[2])

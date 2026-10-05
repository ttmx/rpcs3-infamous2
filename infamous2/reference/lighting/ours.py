"""Offline lighting reconstruction. Capture byte order is A,R,G,B.

This is an investigation reference, not an enabled emulator replacement.
Point/spot shading and tile-volume selection use direct native formulas.
Estimate/interpolation LUTs provide close numerical behavior without an SPU
instruction executor. Isolated differential fuzzing allows small float drift;
live replacement additionally requires correct guest job/frame ownership.
"""
import json
from pathlib import Path
import struct
import numpy as np

F = np.float32
U = np.uint32
ROOT = Path(__file__).resolve().parent
LUT = json.loads((ROOT.parent / 'spu/spu_luts.json').read_text())

def bits(v):
    return np.asarray(v, dtype=F).view(U)

def const(v):
    return np.array(v, dtype=U).view(F)

def estimate(a, rsqrt=False):
    a = bits(a)
    prefix = 'spu_frsqest' if rsqrt else 'spu_frest'
    frac = np.asarray(LUT[prefix + '_fraction_lut'], dtype=U)
    exp = np.asarray(LUT[prefix + '_exponent_lut'], dtype=U)
    b = frac[(a >> 18) & (63 if rsqrt else 31)] | exp[(a >> 23) & 255]
    if not rsqrt:
        b |= a & U(0x80000000)
    base = ((b & U(0x007ffc00)) | U(0x3f800000)).view(F)
    step = (b & U(0x3ff)).astype(F) * F(2 ** -13)
    y = (a & U(0x7ffff)).astype(F) * F(2 ** -19)
    res = bits(base - step * y)
    return ((b & U(0xff800000)) | (res & U(0x007fffff))).view(F)

def dot(a, b):
    return (a[..., 0]*b[..., 0] + a[..., 1]*b[..., 1]) + a[..., 2]*b[..., 2]

def normalize(a):
    return a * estimate(dot(a, a), True)[..., None]

def positions(B, M, origin=(0, 0), screen=None):
    """Tile loop starts at the right edge and walks left in four-pixel quads.
    Preserve incremental float32 rounding instead of evaluating M at each pixel.
    """
    H, W = B.shape[:2]
    pos = np.empty((H, W, 4), dtype=F)
    sw, sh = screen or (W,H)
    ox, oy = origin
    dx = M[0] * F(1/sw)
    dy = M[1] * F(1/sh)
    for ty in range(0, H, 40):
        for tx in range(0, W, 32):
            start = M[0] * F((ox+tx+32)/sw) + M[3]
            start = M[1] * F((oy+ty)/sh) + start
            for ry in range(40):
                row = F(ry)*dy + start
                # First SIMD lane is rightmost; subsequent quads add -4*dx.
                for q in range(8):
                    for lane in range(4):
                        x = tx + 31 - 4*q - lane
                        pos[ty+ry, x] = F((1+lane)/4)*(-4*dx) + row
                    row = row + (-4*dx)
    d = (B[..., 0].astype(U) << 16) | (B[..., 1].astype(U) << 8) | B[..., 2]
    pos = d.astype(F)[..., None] * (M[2]*F(2**-24)) + pos
    return pos[..., :3] * estimate(pos[..., 3])[..., None]

def gloss(a):
    # SPU's polynomial approximation to exp2(material*gain+bias).
    v = a.astype(F)*F(1/256)*const(0x41008081) + const(0xbf6392e2)
    k = np.floor(v).astype(np.int32)
    t = (k.astype(F)-v)*const(0x3f317218)
    lo = t * const(0xbe2aaa4f) + const(0x3efffffd)
    hi = t * const(0xb9142e40) + const(0x3aae4f6f)
    lo = t * lo - F(1)
    hi = t * hi + const(0xbc08026d)
    lo = t * lo + F(1)
    hi = t * hi + const(0x3d2aa0e5)
    t2 = t*t
    return ((t2*t2)*hi + lo) * np.exp2(k.astype(F))

def tile_bounds(p, B):
    """The job excludes d24=0xffffff from its world-space tile bounds.

    A tile with a collapsed extent on any axis takes the zero-fill path.
    This includes a tile containing just one non-sky pixel.
    """
    h, w = B.shape[:2]
    shape = (h//40, 40, w//32, 32, 3)
    valid = ~(B[..., :3] == 255).all(axis=2)
    lo = np.where(valid[..., None], p, F(np.inf)).reshape(shape).min(axis=(1, 3))
    hi = np.where(valid[..., None], p, F(-np.inf)).reshape(shape).max(axis=(1, 3))
    return lo, hi, (hi > lo).all(axis=2)


def refined_reciprocal(a):
    r = estimate(a)
    return (F(1)-a*r)*r+r

def refined_sqrt(a):
    r=estimate(a,True)
    r=(F(1)-a*(r*r))*(r*F(.5))+r
    return np.where(a>np.finfo(F).tiny,r*a,F(0))

def refined_normalize(a):
    d=dot(a,a);r=estimate(d,True)
    return a*((F(1)-d*(r*r))*(r*F(.5))+r)[...,None]

def tile_planes(params, shape, origin=(0,0), screen=None):
    h,w=shape; sw,sh=screen or (w,h);ox,oy=origin
    M=np.frombuffer(params[80:144],'>f4').astype(F).reshape(4,4)
    tx=np.arange(w//32,dtype=F)+F(ox/32);ty=np.arange(h//40,dtype=F)+F(oy/40)
    left=tx*F(2/(sw/32))-F(1);right=(tx+F(1))*F(2/(sw/32))-F(1)
    top=F(1)-ty*F(2/(sh/40));bottom=F(1)-(ty+F(1))*F(2/(sh/40))
    corners=[]
    for x,y in ((right,top),(left,top),(left,bottom),(right,bottom)):
        v=M[0]*x[None,:,None]+M[1]*y[:,None,None]+M[3]
        corners.append(v[...,:3]*refined_reciprocal(v[...,3])[...,None])
    return np.stack([refined_normalize(np.cross(corners[(i+1)%4],corners[i])) for i in range(4)],axis=2)

def cone_sphere(centre,radius,lp,outer,direction,cosine):
    delta=centre-lp;d2=dot(delta,delta);r2=radius*radius
    dist=refined_sqrt(d2);inv=refined_reciprocal(dist)
    with np.errstate(invalid='ignore',divide='ignore'):
        s=radius*inv
        tangent=refined_sqrt(F(1)-s*s)
        u=np.where(tangent*dist>outer,(d2+outer*outer-r2)*refined_reciprocal((F(2)*dist)*outer),tangent)
        threshold=u*cosine-refined_sqrt((F(1)-u*u)*(F(1)-cosine*cosine))
        angle=dot(direction,delta*inv[...,None])
        return (d2<=(radius+outer)*(radius+outer)) & ((r2>d2)|(angle>threshold))

def expand_tiles(a):
    return a.repeat(40, axis=0).repeat(32, axis=1)

def lighting(A, B, params, records, point_only=False, origin=(0,0), screen=None):
    M = np.frombuffer(params[16:80], '>f4').astype(F).reshape(4,4)
    scale = F(struct.unpack_from('>f', params, 0x98)[0])
    p = positions(B, M, origin, screen)
    n = normalize(A[..., 1:4].astype(F)*F(1/256)*const(0x40008081) - F(1))
    view = normalize(p)
    g = gloss(A[..., 0])
    spec_gain = g*const(0x3f6d097b) + F(1)
    rgb = np.zeros_like(p)
    spec = np.zeros(A.shape[:2], dtype=F)
    lo, hi, valid_tiles = tile_bounds(p, B)
    active_tiles = np.zeros_like(valid_tiles)
    planes = tile_planes(params, B.shape[:2], origin, screen)
    centre = (lo+hi)*F(.5)
    extent = hi-lo
    sphere_radius = refined_sqrt(dot(extent,extent))*F(.5)
    for record in records:
        kind = struct.unpack_from('>I', record, 0x14)[0]
        if kind not in (1, 2):
            raise ValueError(f'Unsupported light kind {kind}')
        if kind == 2 and point_only:
            continue
        lp = np.frombuffer(record[:12], '>f4').astype(F)
        colour = np.frombuffer(record[12:18], '>f2').astype(F)*scale
        inner, outer = np.frombuffer(record[32:36], '>f2').astype(F)
        delta = lp-p
        dsq = dot(delta, delta)
        closest = np.clip(lp, lo, hi)
        bounds_hit = dot(lp-closest, lp-closest) < outer*outer
        inv = estimate(dsq, True)
        dist = inv*dsq
        denom = estimate(outer-inner)
        att = np.clip(dist*(-denom) + outer*denom, 0, 1)
        pixel_hit = dsq < outer*outer
        if kind == 2:
            cone_gain, cone_cos = np.frombuffer(record[36:40], '>f2').astype(F)
            direction = np.frombuffer(record[40:46], '>f2').astype(F)
            min_dist = F(np.frombuffer(record[46:48], '>f2')[0])
            angle = dot(delta, -direction)*inv
            cone_att = np.clip(angle*(-cone_gain) + cone_gain*cone_cos, 0, 1)
            att = np.where(dist > min_dist, att*cone_att, F(0))
            pixel_hit = (dist < outer) & (angle > cone_cos)
        pixel_hit = pixel_hit.reshape(
            A.shape[0]//40, 40, A.shape[1]//32, 32).any(axis=(1, 3))
        plane_hit = (dot(planes, lp) >= -outer).all(axis=2)
        volume_hit = cone_sphere(centre,sphere_radius,lp,outer,direction,cone_cos) if kind==2 else True
        eligible = valid_tiles & bounds_hit & plane_hit & volume_hit & pixel_hit
        active_tiles |= eligible
        ndl = np.maximum(dot(n, delta)*inv, 0)
        intensity = np.where(expand_tiles(eligible), ndl*(att*att), F(0))
        rgb = colour*intensity[..., None] + rgb
        half = delta*inv[..., None] - view
        ndh = np.maximum(dot(n, half)*estimate(dot(half, half), True), 0)
        shape = (F(1)-ndh)*g + F(1)
        shape = shape*shape; shape = shape*shape; shape = shape*shape
        magnitude = dot(colour, colour)
        magnitude = magnitude*estimate(magnitude, True)
        spec = (estimate(shape)*spec_gain)*(intensity*magnitude) + spec
    with np.errstate(invalid='ignore'):
        v = rgb*estimate(rgb, True)
    v = np.where(v > np.finfo(F).tiny, v, F(0))
    m = np.maximum(v.max(axis=2)*const(0x3eb504f3), const(0x3eb504f3))
    out = np.zeros_like(A)
    out[..., 0] = np.minimum(np.floor(m*F(255)), 255).astype(np.uint8)
    invm = estimate(out[..., 0].astype(F)*F(1/256)*const(0x3f808081))*const(0x3eb504f3)
    out[..., 1:] = np.minimum(np.floor(v*invm[..., None]*F(255)), 255).astype(np.uint8)
    with np.errstate(invalid='ignore'):
        v = spec*estimate(spec, True)
    v = np.where(v > np.finfo(F).tiny, v, F(0))
    outB = np.zeros_like(B)
    outB[..., 0] = np.minimum(np.floor(v*const(0x3eb504f3)*F(255)), 255).astype(np.uint8)
    out[~expand_tiles(active_tiles)] = 0
    return out, outB

def inputs(run=50):
    def image(name):
        return np.fromfile(ROOT / f'cap/pre-mem-{name}.bin', np.uint8).reshape(720,1280,4)
    small = (ROOT / f'cap/run{run:03d}-mem-00a93000.bin').read_bytes()
    params = small[0xc80:0xd80]
    count, ea = struct.unpack_from('>II', params, 0x90)
    base = ea & ~4095
    table = (ROOT / f'cap/run{run:03d}-mem-{base:08x}.bin').read_bytes()
    records = [table[ea-base+48*i:ea-base+48*(i+1)] for i in range(count)]
    return image('37400b80'), image('37784b80'), params, records

if __name__ == '__main__':
    A, B = lighting(*inputs())
    np.savez_compressed(ROOT / 'ours-point.npz', A=A, B=B)
    ref = np.load(ROOT / 'frame-emu.npz')
    for name, out in [('A', A), ('B', B)]:
        target = ref[name].reshape(out.shape)
        d = np.abs(out.astype(int)-target.astype(int))
        print(name, 'identical bytes', (d==0).mean(), 'pixels <=1', (d.max(axis=2)<=1).mean(),
              'pixels <=4', (d.max(axis=2)<=4).mean(), 'max', d.max())

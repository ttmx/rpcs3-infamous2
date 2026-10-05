"""Our reimplementation of the SSAO stages (numpy prototype of the future GPU pass)."""
import numpy as np
G = np.array([1, 6, 10, 6, 1], dtype=np.float32)
def blur(ao, z, axis, k=30.0, mode='center'):
    """5-tap depth-aware blur along one axis. ao uint8 [H,W], z float32 [H,W]."""
    a = ao.astype(np.float32); z = z.astype(np.float32)
    num = np.zeros_like(a); den = np.zeros_like(a)
    for t, g in zip(range(-2, 3), G):
        an = np.roll(a, -t, axis); zn = np.roll(z, -t, axis)
        # clamp at the borders instead of wrapping
        idx = np.arange(a.shape[axis]) + t
        valid = (idx >= 0) & (idx < a.shape[axis])
        ref = {'center': z, 'neighbor': zn, 'max': np.maximum(z, zn), 'min': np.minimum(z, zn)}[mode]
        w = g * np.clip(1.0 - k * np.abs(zn - z) / ref, 0.0, 1.0)
        w = w * (valid if axis == 1 else valid[:, None])
        num += w * an; den += w
    return np.floor(num / den).astype(np.uint8)

P_NEAR = 0.100000347; Q_DEPTH = 5.96042646e-09
def linear_depth(d24):
    return (1.0 / np.maximum(P_NEAR - Q_DEPTH * d24.astype(np.float64), 1e-12)).astype(np.float32)
def downsample(d24):
    """Stage 1: half-res linear depth from the top-left pixel of each 2x2 block."""
    return linear_depth(d24[0::2, 0::2])
def upsample(aoh, zh, d24, k=40.0, ref='full'):
    """Stage 5: 2x bilinear upsample (weights 9/16, 3/16, 1/16) with each half-res sample weighted by depth agreement."""
    H, W = d24.shape; zf = linear_depth(d24).astype(np.float64); zh = zh.astype(np.float64); a = aoh.astype(np.float64)
    yy, xx = np.mgrid[0:H, 0:W]
    u = (yy - 0.5) / 2.0; v = (xx - 0.5) / 2.0
    i0 = np.floor(u).astype(int); j0 = np.floor(v).astype(int); fy = u - i0; fx = v - j0
    num = np.zeros((H, W)); den = np.zeros((H, W)); best = np.zeros((H, W)); bestw = np.full((H, W), -1.0)
    for di, dj, wb in ((0, 0, (1 - fy) * (1 - fx)), (0, 1, (1 - fy) * fx), (1, 0, fy * (1 - fx)), (1, 1, fy * fx)):
        ii = np.clip(i0 + di, 0, zh.shape[0] - 1); jj = np.clip(j0 + dj, 0, zh.shape[1] - 1)
        zr = {'full': zf, 'half': zh[ii, jj], 'max': np.maximum(zf, zh[ii, jj])}[ref]
        w = wb * np.clip(1.0 - k * np.abs(zh[ii, jj] - zf) / zr, 0.0, 1.0)
        num += w * a[ii, jj]; den += w
        upd = wb > bestw; best = np.where(upd, a[ii, jj], best); bestw = np.where(upd, wb, bestw)
    return np.floor(np.where(den > 1e-9, num / np.maximum(den, 1e-9), best)).astype(np.uint8)

# ---- stage 2: raw occlusion ----
SX = np.array([0, 0.360393, 0.767721, 0.0606153, -0.347511, -0.37089, 0.381673, -0.488981, -0.779765, -0.339852, 0.021904, 0.393308], np.float64)
SY = np.array([0, -0.221962, -0.0020895, -0.561965, 0.688986, 0.221229, 0.682595, -0.294946, 0.0797645, -0.691509, 0.448074, 0.250064], np.float64)
SH = np.array([1, 0.906008, 0.640781, 0.824937, 0.63603, 0.901942, 0.623209, 0.820917, 0.620971, 0.637428, 0.893728, 0.884747], np.float64)
def raw_occlusion(z, A, M, S=5589.43, R=15.0, zlo=1050.52, zhi=2101.04, k_far=-1.0 / 60, k_near=-0.75, gain=1.2, bias=-0.2, masked=100):
    """z: half-res linear depth [h,w]; A: full-res G-buffer bytes [2h,2w,4] (byte 0 mask, bytes 1-3 world normal);
       M: 3x3 with columns (screen x, screen y, depth) as passed by the game each frame."""
    z = z.astype(np.float64); h, w = z.shape
    n = A[0::2, 0::2, 1:4].astype(np.float64) * (2.007843137 / 256.0) - 1.0
    c0 = n @ M[:, 0]; c1 = n @ M[:, 1]; c2 = n @ M[:, 2]
    zcl = np.clip(z, zlo, zhi); s = z / zcl; Rs = R * s; zref = z + s * c2
    yy, xx = np.mgrid[0:h, 0:w]
    total = np.zeros((h, w)); 
    for i in range(12):
        xs = np.clip(np.floor(xx + 0.5 + (S * SX[i] + c0) / zcl).astype(int), 0, w - 1)
        ys = np.clip(np.floor(yy + 0.5 + (S * SY[i] + c1) / zcl).astype(int), 0, h - 1)
        hi = SH[i] * Rs
        d = 0.5 * (z[ys, xs] - zref + hi)
        total += np.clip(d, 0, hi) + hi * np.clip(d * (k_far / s) + k_near * s, 0, 1)
    vis = total / (R * SH.sum() * s)
    out = np.floor(np.clip(gain * vis + bias, 0, 1) * 255.0).astype(np.uint8)
    if masked is not None: out[A[0::2, 0::2, 0] == 0] = masked
    return out

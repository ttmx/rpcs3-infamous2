"""Compare a validation dump of the GPU pass with the SPU result of the same frame and with ours.py."""
import numpy as np, sys, ours
from PIL import Image, ImageDraw
def load(i, d='gpu-dump'):
    raw = np.fromfile(f'{d}/gpu-{i}-all.bin', dtype=np.uint8); o = 0
    def take(n): 
        global_o = take.o; take.o += n; return raw[global_o:global_o + n]
    take.o = 0
    F, Hf = 1280 * 720, 640 * 360
    depth = take(F * 4).reshape(720, 1280, 4); normals = take(F * 4).reshape(720, 1280, 4)
    z = take(Hf * 4).view('<f4').reshape(360, 640); r = take(Hf).reshape(360, 640); h = take(Hf).reshape(360, 640); v = take(Hf).reshape(360, 640)
    fin = take(F).reshape(720, 1280)
    M = np.fromfile(f'{d}/gpu-{i}-matrix.bin', dtype='<f4').reshape(4, 4)[:3, :3]
    spu = np.fromfile(f'{d}/spu-{i}-final.bin', dtype=np.uint8).reshape(720, 1280)
    return dict(depth=depth, normals=normals, z=z, raw=r, h=h, v=v, final=fin, M=M, spu=spu)
def st(a, b):
    d = np.abs(a.astype(np.int64) - b.astype(np.int64)); return f'{100*(d==0).mean():5.1f}% identical, {100*(d<=1).mean():6.2f}% within 1, {100*(d<=4).mean():6.2f}% within 4, max {d.max()}'
if __name__ == '__main__':
    for i in range(4):
        D = load(i); print(f'--- dump {i}:  GPU final vs SPU final:', st(D['final'], D['spu']))
        dp = D['depth'].astype(np.int64); d24 = (dp[..., 3] << 16) | (dp[..., 2] << 8) | dp[..., 1]   # B,G,R,A in memory -> A,R,G
        A = D['normals'][..., [3, 2, 1, 0]]                                                           # -> A,R,G,B = guest byte order
        z = ours.downsample(d24); print('   z      :', 'max rel err %.2e' % np.max(np.abs(z - D['z']) / z))
        raw = ours.raw_occlusion(D['z'], A, D['M'], masked=None); print('   raw    :', st(raw, D['raw']))
        bh = ours.blur(D['raw'], D['z'], 1); print('   blur h :', st(bh, D['h']))
        bv = ours.blur(D['h'], D['z'], 0); print('   blur v :', st(bv, D['v']))
        up = ours.upsample(D['v'], D['z'], d24); print('   upsamp :', st(up, D['final']))
        print('   python chain vs SPU:', st(ours.upsample(ours.blur(ours.blur(ours.raw_occlusion(z, A, D['M'], masked=None), z, 1), z, 0), z, d24), D['spu']))

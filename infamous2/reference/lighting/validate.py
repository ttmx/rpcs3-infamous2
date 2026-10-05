"""Offline differential checks against the captured frame and original SPU job.

Synthetic cases replace the light table and selected pixels, preserving the
captured dispatcher. No emulator configuration or game saves are modified.
"""
import json
from pathlib import Path
import struct
import sys
import numpy as np

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / 'spu'))
import ours
import light_emu


def compare(actual, target):
    d = np.abs(actual.astype(np.int16) - target.astype(np.int16))
    return dict(identical_bytes=float((d == 0).mean()),
                pixels_within_1=float((d.max(axis=2) <= 1).mean()),
                pixels_within_4=float((d.max(axis=2) <= 4).mean()),
                max_error=int(d.max()), mean_error=float(d.mean()))


def oracle(A, B, params, records, tile, observer=None, budget=2_000_000):
    def before(mem):
        mem[light_emu.A_EA][:] = A.tobytes()
        mem[light_emu.B_EA][:] = B.tobytes()
        mem[0xa93000][0xa00:0xa06] = struct.pack('>HHH', 0, tile, tile)
        mem[0xa93000][0xc80:0xd80] = params
        mem[0xa93000][0xd10:0xd14] = struct.pack('>I', len(records))
        mem[0x37d6d000][0x690:0x690 + 48*len(records)] = b''.join(records)
        # The job finishing the last tile PUTs its RSX label at 0x40300870.
        mem.setdefault(0x40300000, bytearray(0x1000))
    class TileSPU(light_emu.spuemu.SPU):
        def __init__(self, ls, gpr, pc):
            # The saved DMA list contains the original eight-light byte count.
            # Resize that transfer when replaying a different table.
            ls=bytearray(ls)
            struct.pack_into('>H',ls,0xfffa,48*len(records))
            super().__init__(ls,gpr,pc)
    s, mem, stats = light_emu.make(50, cls=TileSPU, before=before)
    while stats['puts'] < 4:
        if observer: observer(s,mem,stats)
        if s.pc == 0x1760:
            s.pc = int(s.r[0][0]) & 0x3fffc
        try:
            s.step()
        except light_emu.Done:
            # After the last tile the job signals its label and returns to
            # SPURS state this harness does not map. Its pixels are complete.
            if not any(mem[0x40300000]): raise
            break
        if s.count > budget:
            raise RuntimeError('Bounded tile oracle did not finish')
    return [np.frombuffer(mem[ea], np.uint8).reshape(A.shape).copy()
            for ea in (light_emu.A_EA, light_emu.B_EA)]


def main():
    import os
    os.chdir(ROOT)  # legacy light_emu uses relative capture paths
    A, B, params, records = ours.inputs()
    result = dict(capture='cap/run050', oracle='SPU interpreter, unfused arithmetic', cases={})
    actual = ours.lighting(A, B, params, records)
    frame = np.load(ROOT / 'frame-emu.npz')
    result['frame_vs_interpreter'] = {
        name: compare(out, frame[name].reshape(out.shape))
        for name, out in zip(('A', 'B'), actual)}
    result['frame_vs_game'] = {
        name: compare(out, np.fromfile(ROOT / f'cap/post-mem-{ea:08x}.bin', np.uint8).reshape(out.shape))
        for name, ea, out in zip(('A', 'B'), (light_emu.A_EA, light_emu.B_EA), actual)}
    M = np.frombuffer(params[16:80], '>f4').astype('f4').reshape(4, 4)
    p = ours.positions(B, M)
    lp = np.frombuffer(records[5][:12], '>f4').astype('f4')
    direction = p[205, 859] - lp
    direction /= np.linalg.norm(direction)
    spot = bytearray(records[6])
    spot[:12] = records[5][:12]
    spot[32:36] = records[5][32:36]
    spot[40:46] = direction.astype('>f2').tobytes()
    cases = [('point', [records[5]], 226, None),
             ('overlapping_points', [records[0], records[5]], 226, None),
             ('spot_centre', [bytes(spot)], 226, None)]
    for label, amount in [('spot_falloff', 0.55), ('spot_outside', 1.4)]:
        r = bytearray(spot)
        v = direction + np.array([0, amount, 0], dtype='f4')
        v /= np.linalg.norm(v)
        r[40:46] = v.astype('>f2').tobytes()
        cases.append((label, [bytes(r)], 226, None))
    near = bytearray(spot)
    near[46:48] = np.array([1000], dtype='>f2').tobytes()
    cases += [('spot_near_cutoff', [bytes(near)], 226, None),
              ('point_and_spot', [records[5], bytes(spot)], 226, None),
              ('no_lights', [], 226, None),
              ('collapsed_tile', records, 334, None),
              ('sky_tile', records, 0, None),
              ('depth_byte3', [records[5]], 226, 'depth_byte3'),
              ('gloss_extremes', [records[5]], 226, 'gloss_extremes')]
    for label, lights, tile, tweak in cases:
        a, b = A.copy(), B.copy()
        y, x = (tile // 40) * 40, (tile % 40) * 32
        area = np.s_[y:y+40, x:x+32]
        if tweak == 'depth_byte3':
            b[area][..., 3] = np.arange(32, dtype=np.uint8)[None, :] * 8
        if tweak == 'gloss_extremes':
            a[area][..., 0] = np.where(np.arange(32)[None, :] < 16, 0, 255)
        target = oracle(a, b, params, lights, tile)
        out = ours.lighting(a, b, params, lights)
        result['cases'][label] = {name: compare(o[area], t[area])
                                 for name, o, t in zip(('A', 'B'), out, target)}
        print(label, result['cases'][label], flush=True)
    result['passed'] = all(v['max_error'] <= 1 for group in
                           [result['frame_vs_interpreter'], *result['cases'].values()]
                           for v in group.values())
    (ROOT / 'validation.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS' if result['passed'] else 'FAIL', flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

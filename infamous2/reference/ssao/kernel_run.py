#!/usr/bin/env python3
"""Run the captured SSAO stage-2 kernel (as loaded at LS 0x6c00) on arbitrary input bands with the offline interpreter."""
import struct, sys, numpy as np, pickle, pathlib
sys.path.insert(0, '../spu')
import spuemu
STATE = pathlib.Path('kernel-entry-state.pkl')
def capture_entry_state():
    """Re-run the dispatcher from the captured pre-state up to the second kernel entry and save that state."""
    import importlib.util
    b = open('capture/stage2-pre.bin', 'rb').read(); hdr = struct.unpack_from('<8I', b, 8); g = b[40:40 + 2048]; ls = bytearray(b[40 + 2048:])
    gpr = [np.array(struct.unpack_from('<4I', g, i * 16)[::-1], dtype=np.uint32) for i in range(128)]
    mem = {0x37b08000: bytearray(open('capture/mem-37b08000.bin', 'rb').read()), 0x00a94000: bytearray(open('capture/mem-00a94000.bin', 'rb').read())}
    def region(ea, size):
        for base, buf in mem.items():
            if base <= ea and ea + size <= base + len(buf): return buf, ea - base
        raise spuemu.Halt('unmapped 0x%x' % ea)
    s = spuemu.SPU(ls, gpr, hdr[0] + 4)
    def lst(lsa, eal, size, get):
        lsa &= 0x3fff0
        for i in range(size // 8):
            fl, sz, ea = struct.unpack_from('>HHI', s.ls, (eal & 0x3fff8) + 8 * i); buf, off = region(ea, sz); a = lsa + (ea & 15)
            if get: s.ls[a:a + sz] = buf[off:off + sz]
            lsa += (sz + 15) & ~15
    lst(hdr[4], hdr[3], hdr[5], True)
    def wrch(s_, ch, v, pc):
        if ch != 21: return
        cmd = v & 0xff; lsa = s_.ch.get(16, 0); eal = s_.ch.get(18, 0); size = s_.ch.get(19, 0)
        if cmd == 0xd0: buf, off = region(eal & ~127, 128); s_.ls[lsa & 0x3ff80:(lsa & 0x3ff80) + 128] = buf[off:off + 128]; s_.atomic = 4
        elif cmd == 0xb4: s_.atomic = 0
        elif cmd in (0x44, 0x45, 0x46): lst(lsa, eal, size, True)
    s.on_wrch = wrch; s.on_rdch = lambda s_, ch, pc: s_.ch.get(22, 0) if ch == 24 else (getattr(s_, 'atomic', 0) if ch == 27 else 0)
    entries = 0; last = 0
    while True:
        if s.pc == 0x6c30 and not (0x6c00 <= last < 0x7400):
            entries += 1
            if entries == 2: break
        last = s.pc; s.step()
    pickle.dump(dict(ls=bytes(s.ls), gpr=[r.copy() for r in s.r], pc=s.pc), open(STATE, 'wb'))
def run_band(inp):
    """inp: float32 array [49, 128] (two margin rows above and below). Returns uint8 [45, 128]."""
    st = pickle.load(open(STATE, 'rb'))
    s = spuemu.SPU(bytearray(st['ls']), st['gpr'], st['pc'])
    in_ls = int(s.r[8][0]) - 2 * 512; out_ls = int(s.r[5][0])
    s.ls[in_ls:in_ls + 49 * 512] = np.asarray(inp, dtype='>f4').tobytes()
    ret = int(s.r[0][0]) & 0x3fffc
    while s.pc != ret: s.step()
    return np.frombuffer(bytes(s.ls[out_ls:out_ls + 45 * 128]), dtype=np.uint8).reshape(45, 128).copy(), s.count
if __name__ == '__main__':
    if not STATE.exists(): capture_entry_state()
    st = pickle.load(open(STATE, 'rb'))
    print('entry state: r3=0x%x r5(out)=0x%x r8(in)=0x%x lr=0x%x' % (int(st['gpr'][3][0]), int(st['gpr'][5][0]), int(st['gpr'][8][0]), int(st['gpr'][0][0])))
    z = np.load('f3-zhalf.npy').astype(np.float32); game = np.load('f3-aohalf.npy')
    out = np.zeros((360, 640), dtype=np.uint8); total = 0
    zp = np.pad(z, ((2, 2), (0, 0)), mode='edge')
    for by in range(8):
        for bx in range(5):
            band, n = run_band(zp[by * 45:by * 45 + 49, bx * 128:bx * 128 + 128]); total += n
            out[by * 45:by * 45 + 45, bx * 128:bx * 128 + 128] = band
        print('  band row %d done' % by, flush=True)
    np.save('stage2-kernel-full.npy', out)
    d = np.abs(out.astype(int) - game.astype(int))
    print('instructions: %d;  whole half-res image vs game: %.1f%% identical, %.1f%% within 2, mean abs diff %.2f' % (total, 100 * (d == 0).mean(), 100 * (d <= 2).mean(), d.mean()))

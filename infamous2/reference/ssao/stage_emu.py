#!/usr/bin/env python3
"""Emulate one whole SSAO stage single-threaded from a stage-start capture.
   stage_emu.py <stage index>   ->  writes stage<N>-emu-mem.npz with the resulting C region and AO output."""
import struct, sys, time, glob, pathlib, numpy as np
sys.path.insert(0, '../spu')
import spuemu
ELF = pathlib.Path('../spu/EBOOT-104.elf').read_bytes()
def load_stage(i):
    f = glob.glob(f'stages/stage{i}-*-state.bin')[0]; b = open(f, 'rb').read(); assert b[:8] == b'SPUSTATE'
    hdr = struct.unpack_from('<8I', b, 8); g = b[40:40 + 2048]
    gpr = [np.array(struct.unpack_from('<4I', g, k * 16)[::-1], dtype=np.uint32) for k in range(128)]
    mem = {}
    for m in glob.glob(f'stages/stage{i}-*-mem-*.bin'):
        mem[int(m.rsplit('-', 1)[1][:8], 16)] = bytearray(open(m, 'rb').read())
    return dict(pc=hdr[0], cmd=hdr[1], eal=hdr[3], lsa=hdr[4], size=hdr[5], gpr=gpr, ls=bytearray(b[40 + 2048:]), mem=mem)
def emulate(i, limit=2_000_000_000, verbose=True, override=None):
    st = load_stage(i); mem = st['mem']; kernel_ea = st['eal']
    if override: override(mem)
    def region(ea, size):
        for base, buf in mem.items():
            if base <= ea and ea + size <= base + len(buf): return buf, ea - base
        if 0x0087d500 <= ea < 0x00880d90: return ELF, ea - 0x10000
        return None, 0
    s = spuemu.SPU(st['ls'], st['gpr'], st['pc'] + 4)
    s.ls[st['lsa']:st['lsa'] + st['size']] = ELF[st['eal'] - 0x10000:st['eal'] - 0x10000 + st['size']]
    stats = dict(lists=0, kernel_loads=0, ignored=0, units=0, last=0, llar=0)
    class Done(Exception): pass
    def wrch(s_, ch, v, pc):
        if ch != 21: return
        cmd = v & 0xff; lsa = s_.ch.get(16, 0); eal = s_.ch.get(18, 0); size = s_.ch.get(19, 0)
        if cmd == 0xd0:
            buf, off = region(eal & ~127, 128)
            if buf is None: raise spuemu.Halt('GETLLAR unmapped 0x%x' % eal)
            a = lsa & 0x3ff80; s_.ls[a:a + 128] = buf[off:off + 128]; s_.atomic = 4; stats['llar'] += 1
        elif cmd == 0xb4:
            buf, off = region(eal & ~127, 128); a = lsa & 0x3ff80
            if buf is not None and buf is not ELF: buf[off:off + 128] = s_.ls[a:a + 128]
            s_.atomic = 0
        elif cmd in (0x44, 0x45, 0x46, 0x24, 0x25, 0x26):
            get = cmd & 0x40; a = lsa & 0x3fff0; stats['lists'] += 1; stats['last'] = s_.count
            for k in range(size // 8):
                fl, sz, ea = struct.unpack_from('>HHI', s_.ls, (eal & 0x3fff8) + 8 * k); buf, off = region(ea, sz); d = a + (ea & 15)
                if buf is None: raise spuemu.Halt('list transfer to unmapped 0x%x' % ea)
                if get: s_.ls[d:d + sz] = buf[off:off + sz]
                else: buf[off:off + sz] = s_.ls[d:d + sz]
                a += (sz + 15) & ~15
            if not get: stats['units'] += 1
        elif cmd in (0x40, 0x41, 0x42):
            if 0x0087d500 <= eal < 0x00880d90 and size >= 784:
                if eal != kernel_ea: raise Done('next kernel 0x%x requested' % eal)
                stats['kernel_loads'] += 1
            buf, off = region(eal, size)
            if buf is None: raise spuemu.Halt('GET unmapped 0x%x size %d at pc 0x%x' % (eal, size, pc))
            s_.ls[lsa:lsa + size] = buf[off:off + size]
            for k in range(lsa & ~3, lsa + size, 4): s_.cache.pop(k, None)
        elif cmd in (0x20, 0x21, 0x22):
            buf, off = region(eal, size)
            if buf is not None and buf is not ELF: buf[off:off + size] = s_.ls[lsa:lsa + size]
            else: stats['ignored'] += 1
        else: raise spuemu.Halt('unexpected MFC command 0x%02x at 0x%05x' % (cmd, pc))
    def rdch(s_, ch, pc):
        if ch == 24: return s_.ch.get(22, 0)
        if ch == 27: return getattr(s_, 'atomic', 0)
        if ch == 13: return 0
        raise Done('RDCH ch%d at 0x%05x (job leaves the dispatcher)' % (ch, pc))
    s.on_wrch = wrch; s.on_rdch = rdch
    t = time.time(); why = 'limit'
    try:
        while s.count < limit:
            s.step()
            if s.count - stats['last'] > 1_500_000: raise Done('no transfers for 1.5M instructions (idle / waiting), pc 0x%05x' % s.pc)
            if verbose and s.count % 5_000_000 == 0: print('   %dM instructions, %d output bands, %.0f s' % (s.count // 1_000_000, stats['units'], time.time() - t), flush=True)
    except Done as e: why = str(e)
    except spuemu.Halt as e: why = 'HALT ' + str(e)
    if verbose: print('stage %d (kernel 0x%x): stopped: %s; %d instructions, %s, %.0f s' % (i, kernel_ea, why, s.count, stats, time.time() - t))
    return mem, s, why
if __name__ == '__main__':
    i = int(sys.argv[1]); mem, s, why = emulate(i)
    np.savez_compressed(f'stage{i}-emu-mem.npz', C=np.frombuffer(bytes(mem[0x37b08000]), dtype=np.uint8), AO=np.frombuffer(bytes(mem[0xcf800000]), dtype=np.uint8))
    nxt = load_stage(i + 1)['mem']
    C = 0x37b08000
    for name, lo, n in (('C1 half depth', 0xb80, 921600), ('C2 half occlusion', 0x37be9b80 - C, 230400)):
        a = np.frombuffer(bytes(mem[C][lo:lo + n]), dtype=np.uint8); b = np.frombuffer(bytes(nxt[C][lo:lo + n]), dtype=np.uint8)
        print('   %-18s vs game buffer at next stage start: %.3f%% bytes identical' % (name, 100 * (a == b).mean()))
    a = np.frombuffer(bytes(mem[0xcf800000][:921600]), dtype=np.uint8); b = np.frombuffer(bytes(nxt[0xcf800000][:921600]), dtype=np.uint8)
    print('   %-18s vs game buffer at next stage start: %.3f%% bytes identical' % ('AO output', 100 * (a == b).mean()))

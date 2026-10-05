#!/usr/bin/env python3
"""Run the game's SSAO stage-2 code (dispatcher + kernel) in the offline SPU interpreter from the captured
pre-state through to the output PUT of the band whose input was being fetched, and compare with the capture."""
import struct, sys, time, numpy as np
sys.path.insert(0, '../spu')
import spuemu, spudis
def load(name):
    b = open(f'capture/{name}.bin', 'rb').read(); assert b[:8] == b'SPUSTATE'
    hdr = struct.unpack_from('<8I', b, 8); g = b[40:40 + 128 * 16]; ls = b[40 + 128 * 16:]
    gpr = [np.array(struct.unpack_from('<4I', g, i * 16)[::-1], dtype=np.uint32) for i in range(128)]
    return dict(pc=hdr[0], cmd=hdr[1], tag=hdr[2], eal=hdr[3], lsa=hdr[4], size=hdr[5], gpr=gpr, ls=bytearray(ls))
pre = load('stage2-pre'); post = [load(f'stage2-post{i}') for i in range(3)]
mem = {0x37b08000: bytearray(open('capture/mem-37b08000.bin', 'rb').read()), 0x00a94000: bytearray(open('capture/mem-00a94000.bin', 'rb').read())}
def mem_region(ea, size):
    for base, buf in mem.items():
        if base <= ea and ea + size <= base + len(buf): return buf, ea - base
    return None, 0
spu = spuemu.SPU(pre['ls'], pre['gpr'], pre['pc'] + 4)
log = []; puts = []; kernel_calls = []
def do_list(s, lsa, eal, size, get):
    lsa &= 0x3fff0; out = []
    for i in range(size // 8):
        flags, sz, ea = struct.unpack_from('>HHI', s.ls, (eal & 0x3fff8) + 8 * i)
        buf, off = mem_region(ea, sz)
        a = lsa + (ea & 15)
        if get:
            if buf is None: raise spuemu.Halt(f'list GET from unmapped 0x{ea:x}')
            s.ls[a:a + sz] = buf[off:off + sz]
            for k in range(a & ~3, a + sz, 4): s.cache.pop(k, None)
        else:
            out.append((ea, bytes(s.ls[a:a + sz])))
            if buf is not None: buf[off:off + sz] = s.ls[a:a + sz]
        lsa += (sz + 15) & ~15
    return out
class Done(Exception): pass
def wrch(s, ch, v, pc):
    if ch != 21: return
    cmd = v & 0xff; lsa = s.ch.get(16, 0); eal = s.ch.get(18, 0); size = s.ch.get(19, 0)
    base = cmd & ~0x0f if (cmd & 0x0f) in (1, 2, 3) and cmd not in (0xd0, 0xb4, 0xb0, 0xb8) else cmd
    log.append((s.count, pc, 'MFC cmd 0x%02x lsa=0x%x eal=0x%x size=0x%x' % (cmd, lsa, eal, size)))
    if cmd == 0xd0:      # GETLLAR
        buf, off = mem_region(eal & ~127, 128)
        if buf is None: raise spuemu.Halt('GETLLAR from unmapped 0x%x' % eal)
        s.ls[lsa & 0x3ff80:(lsa & 0x3ff80) + 128] = buf[off:off + 128]; s.atomic = 4
    elif cmd == 0xb4:    # PUTLLC: always succeeds here (single thread)
        buf, off = mem_region(eal & ~127, 128); buf[off:off + 128] = s.ls[lsa & 0x3ff80:(lsa & 0x3ff80) + 128]; s.atomic = 0
    elif cmd in (0x44, 0x45, 0x46): do_list(s, lsa, eal, size, True)
    elif cmd in (0x24, 0x25, 0x26):
        puts.append(do_list(s, lsa, eal, size, False))
        if len(puts) == 2: raise Done()
    elif cmd in (0x20, 0x21, 0x22):
        buf, off = mem_region(eal, size)
        if buf is not None: buf[off:off + size] = s.ls[lsa:lsa + size]
    elif cmd in (0x40, 0x41, 0x42):
        buf, off = mem_region(eal, size)
        if buf is None: raise spuemu.Halt('GET from unmapped 0x%x size %d' % (eal, size))
        s.ls[lsa:lsa + size] = buf[off:off + size]
    else: raise spuemu.Halt('unexpected MFC command 0x%02x at 0x%05x' % (cmd, pc))
def rdch(s, ch, pc):
    if ch == 24: return s.ch.get(22, 0)
    if ch == 27: return getattr(s, 'atomic', 0)
    log.append((s.count, pc, f'RDCH ch{ch}')); return 0
spu.on_wrch = wrch; spu.on_rdch = rdch
t = time.time(); last = 0
try:
    while True:
        pc = spu.pc
        if 0x6c00 <= pc < 0x7400 and not (0x6c00 <= last < 0x7400):
            kernel_calls.append((spu.count, last, pc, [int(spu.r[i][0]) for i in range(3, 12)]))
        last = pc; spu.step()
        if spu.count % 2_000_000 == 0: print('  ...%dM instructions, pc 0x%05x, %.0f s' % (spu.count // 1_000_000, spu.pc, time.time() - t), flush=True)
        if spu.count > 300_000_000: print('limit'); break
except Done: print('reached the second output PUT at pc 0x%05x after %d instructions (%.0f s)' % (spu.pc, spu.count, time.time() - t))
except spuemu.Halt as e: print('HALT:', e, 'after', spu.count, 'instructions')
for c, pc, m in log: print('   %9d 0x%05x %s' % (c, pc, m))
print('kernel entries from outside:', len(kernel_calls))
for c, frm, to, args in kernel_calls[:6]: print('   at %d: 0x%05x -> 0x%05x  r3..r11 = %s' % (c, frm, to, ' '.join('%x' % a for a in args)))
if len(puts) >= 2:
    mine = np.frombuffer(b''.join(d for ea, d in puts[1]), dtype=np.uint8).reshape(-1, len(puts[1][0][1]))
    p = post[1]; lsa = p['lsa'] & 0x3fff0; n = p['size'] // 8
    game = np.frombuffer(bytes(p['ls'][lsa:lsa + n * 128]), dtype=np.uint8).reshape(n, 128)
    print('second PUT: first EA mine 0x%x; shape mine %s game %s' % (puts[1][0][0], mine.shape, game.shape))
    if mine.shape == game.shape:
        d = np.abs(mine.astype(int) - game.astype(int)); print('band output vs game: %.2f%% bytes identical, max abs diff %d, mean %.3f' % (100 * (d == 0).mean(), d.max(), d.mean()))
    np.save('stage2-emu-out.npy', mine); np.save('stage2-game-out.npy', game)
if len(puts) >= 1:
    mine0 = np.frombuffer(b''.join(d for ea, d in puts[0]), dtype=np.uint8).reshape(-1, 128)
    p = post[0]; lsa = p['lsa'] & 0x3fff0; game0 = np.frombuffer(bytes(p['ls'][lsa:lsa + 45 * 128]), dtype=np.uint8).reshape(45, 128)
    d0 = np.abs(mine0.astype(int) - game0.astype(int)); print('first PUT (band computed before the capture point): %.2f%% identical' % (100 * (d0 == 0).mean()))
    d = np.abs(mine.astype(int) - game.astype(int))
    print('second band: differing bytes per row :', ' '.join(str(int(x)) for x in (d > 0).sum(1)))
    print('second band: differing bytes per col/8:', ' '.join(str(int(x)) for x in (d > 0).reshape(45, 16, 8).sum((0, 2))))
    print('sample row 20 mine:', mine[20, 40:56].tolist()); print('sample row 20 game:', game[20, 40:56].tolist())

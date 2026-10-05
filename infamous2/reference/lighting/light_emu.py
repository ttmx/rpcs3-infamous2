#!/usr/bin/env python3
"""Emulate lighting job runs offline from the one-frame capture in cap/.
   emulate(run) -> (mem, spu, why, stats): starts from the captured SPU state of that job run, with guest memory =
   pre-lighting G-buffers + that run's small regions; the run keeps taking tiles until the shared counter is exhausted."""
import struct, sys, time, glob, numpy as np
sys.path.insert(0, '../spu')
import spuemu
spuemu.FUSED = False   # the capture was taken with the interpreter (separate multiply and add)
A_EA, B_EA = 0x37400b80, 0x37784b80
def load(i, cls=spuemu.SPU):
    b = open(f'cap/run{i:03d}-state.bin', 'rb').read(); assert b[:8] == b'SPUSTATE'
    hdr = struct.unpack_from('<8I', b, 8); g = b[40:40 + 2048]
    gpr = [np.array(struct.unpack_from('<4I', g, k * 16)[::-1], dtype=np.uint32) for k in range(128)]
    mem = {int(m.rsplit('-', 1)[1][:8], 16): bytearray(open(m, 'rb').read()) for m in glob.glob(f'cap/run{i:03d}-mem-*.bin')}
    mem[A_EA] = bytearray(open('cap/pre-mem-37400b80.bin', 'rb').read()); mem[B_EA] = bytearray(open('cap/pre-mem-37784b80.bin', 'rb').read())
    return dict(pc=hdr[0], srr0=hdr[1], list=hdr[3], lsa=hdr[4], size=hdr[5], gpr=gpr, ls=bytearray(b[40 + 2048:]), mem=mem)
class Done(Exception): pass
def make(i, cls=spuemu.SPU, before=None, log=None, mem=None):
    st = load(i); mem = st['mem'] if mem is None else mem
    if before: before(mem)
    def region(ea, size):
        for base, buf in mem.items():
            if base <= ea and ea + size <= base + len(buf): return buf, ea - base
        return None, 0
    s = cls(st['ls'], st['gpr'], st['pc'] + 4)
    s.srr0 = st['srr0']
    s.src = {}; s.out = {}
    stats = dict(gets=0, puts=0, llar=0, llc=0, last=0)
    def do_list(s_, get, lst, lsa, size):
        a = lsa & 0x3fff0
        for k in range(size // 8):
            fl, sz, ea = struct.unpack_from('>HHI', s_.ls, (lst & 0x3fff8) + 8 * k); buf, off = region(ea, sz); d = a + (ea & 15)
            if buf is None: raise spuemu.Halt('list transfer to unmapped 0x%x' % ea)
            if log is not None: log.append(('GET' if get else 'PUT', ea, d, sz, s_.pc))
            if get:
                s_.ls[d:d + sz] = buf[off:off + sz]
                for q in range(d & ~3, d + sz, 4): s_.cache.pop(q, None)
                if hasattr(s_, 'lt'):                       # dataflow tracer: fresh leaves, remember where each word came from
                    for q in range(d & ~3, d + sz, 4): s_.lt.pop(q, None); s_.src[q] = ea + (q - d)
            else:
                buf[off:off + sz] = s_.ls[d:d + sz]
                if hasattr(s_, 'lt'):
                    for q in range(d & ~3, d + sz, 4): s_.out[ea + (q - d)] = s_.lt.get(q)
            a += (sz + 15) & ~15
        stats['gets' if get else 'puts'] += 1; stats['last'] = s_.count
    do_list(s, True, st['list'], st['lsa'], st['size'])
    def wrch(s_, ch, v, pc):
        if ch != 21: return
        cmd = v & 0xff; lsa = s_.ch.get(16, 0); eal = s_.ch.get(18, 0); size = s_.ch.get(19, 0)
        if cmd == 0xd0:
            buf, off = region(eal & ~127, 128)
            if buf is None: raise Done('GETLLAR unmapped 0x%x at pc 0x%x' % (eal, pc))
            a = lsa & 0x3ff80; s_.ls[a:a + 128] = buf[off:off + 128]; s_.atomic = 4; stats['llar'] += 1
            if log is not None: log.append(('GETLLAR', eal, a, 128, pc))
        elif cmd == 0xb4:
            buf, off = region(eal & ~127, 128); a = lsa & 0x3ff80
            buf[off:off + 128] = s_.ls[a:a + 128]; s_.atomic = 0; stats['llc'] += 1
            if log is not None: log.append(('PUTLLC', eal, a, 128, pc))
        elif cmd in (0x44, 0x45, 0x46): do_list(s_, True, eal, lsa, size)
        elif cmd in (0x24, 0x25, 0x26): do_list(s_, False, eal, lsa, size)
        elif cmd in (0x40, 0x41, 0x42):
            buf, off = region(eal, size)
            if buf is None: raise Done('GET unmapped 0x%x size %d at pc 0x%x' % (eal, size, pc))
            s_.ls[lsa:lsa + size] = buf[off:off + size]
            for k in range(lsa & ~3, lsa + size, 4): s_.cache.pop(k, None)
            if log is not None: log.append(('GETd', eal, lsa, size, pc))
        elif cmd in (0x20, 0x21, 0x22):
            buf, off = region(eal, size)
            if buf is None: raise Done('PUT unmapped 0x%x size %d at pc 0x%x' % (eal, size, pc))
            buf[off:off + size] = s_.ls[lsa:lsa + size]
            if log is not None: log.append(('PUTd', eal, lsa, size, pc))
        else: raise spuemu.Halt('unexpected MFC command 0x%02x at 0x%05x' % (cmd, pc))
    def rdch(s_, ch, pc):
        if ch == 24: return s_.ch.get(22, 0)
        if ch == 27: return getattr(s_, 'atomic', 0)
        if ch == 13: return 0
        if ch == 8: return (0xffffffff - s_.count * 4) & 0xffffffff   # decrementer
        if ch == 0: return s_.ch.get(1, 0) & 0x1                        # event status: tag-group completion only
        raise Done('RDCH ch%d at 0x%05x' % (ch, pc))
    s.on_wrch = wrch; s.on_rdch = rdch
    return s, mem, stats
def emulate(i, limit=4_000_000_000, verbose=True, **kw):
    s, mem, stats = make(i, **kw); t = time.time(); why = 'limit'
    try:
        while s.count < limit:
            if s.pc == 0x1760:      # SPURS service called by the job at 0x44bc (yields to the kernel): return straight away
                s.pc = int(s.r[0][0]) & 0x3fffc; stats['yields'] = stats.get('yields', 0) + 1
            s.step()
            if verbose and s.count % 5_000_000 == 0: print('   %dM instructions, %s, %.0f s' % (s.count // 1_000_000, stats, time.time() - t), flush=True)
    except Done as e: why = str(e)
    except spuemu.Halt as e: why = 'HALT ' + str(e)
    if verbose: print('run %d stopped: %s; %d instructions, pc 0x%05x, %s, %.0f s' % (i, why, s.count, s.pc, stats, time.time() - t))
    return mem, s, why, stats
def full_frame(i=50, verbose=True):
    """Run the job repeatedly from run i's state, sharing guest memory, until all 720 tiles have been taken."""
    mem = load(i)['mem']; mem[0xa93000][0xa00:0xa06] = bytes(6); t = time.time(); n = 0; total = 0
    while True:
        flag, taken, done = struct.unpack_from('>HHH', mem[0xa93000], 0xa00)
        if done >= 720 or n > 800: break
        s, _, stats = make(i, mem=mem)
        try:
            while True:
                if s.pc == 0x1760: s.pc = int(s.r[0][0]) & 0x3fffc
                s.step()
        except Done: pass
        n += 1; total += s.count
        if verbose and n % 20 == 0: print('   %d runs, %d tiles taken, %dM instructions, %.0f s' % (n, struct.unpack_from('>H', mem[0xa93000], 0xa02)[0], total // 1000000, time.time() - t), flush=True)
    print('full frame: %d runs, %d instructions, %.0f s' % (n, total, time.time() - t))
    return mem
if __name__ == '__main__' and sys.argv[1] == 'frame':
    mem = full_frame()
    np.savez_compressed('frame-emu.npz', A=np.frombuffer(bytes(mem[A_EA]), dtype=np.uint8), B=np.frombuffer(bytes(mem[B_EA]), dtype=np.uint8))
    for name, ea, f in (('A', A_EA, 'cap/post-mem-37400b80.bin'), ('B', B_EA, 'cap/post-mem-37784b80.bin')):
        a = np.frombuffer(bytes(mem[ea]), dtype=np.uint8).reshape(720, 1280, 4).astype(int); b = np.fromfile(f, dtype=np.uint8).reshape(720, 1280, 4).astype(int)
        d = np.abs(a - b); print(f'{name}: identical bytes {100*(d==0).mean():.2f}%, pixels within 1: {100*(d.max(axis=2)<=1).mean():.2f}%, within 4: {100*(d.max(axis=2)<=4).mean():.2f}%, max {d.max()}')
elif __name__ == '__main__':
    i = int(sys.argv[1]); log = []
    mem, s, why, stats = emulate(i, limit=int(sys.argv[2]) if len(sys.argv) > 2 else 4_000_000_000, log=log)
    np.savez_compressed(f'run{i}-emu.npz', A=np.frombuffer(bytes(mem[A_EA]), dtype=np.uint8), B=np.frombuffer(bytes(mem[B_EA]), dtype=np.uint8))
    import pickle; pickle.dump(log, open(f'run{i}-emu-log.pkl', 'wb'))
    for name, ea, f in (('A', A_EA, 'cap/post-mem-37400b80.bin'), ('B', B_EA, 'cap/post-mem-37784b80.bin')):
        a = np.frombuffer(bytes(mem[ea]), dtype=np.uint8).reshape(720, 1280, 4); b = np.fromfile(f, dtype=np.uint8).reshape(720, 1280, 4)
        p = np.fromfile(f.replace('post', 'pre'), dtype=np.uint8).reshape(720, 1280, 4)
        touched = (a != p).any(axis=2); print(f'{name}: emulation wrote {100*touched.mean():.1f}% of pixels; identical to game output on {100*(a==b).all(axis=2).mean():.2f}% of all pixels, within 1 on {100*(np.abs(a.astype(int)-b.astype(int)).max(axis=2)<=1).mean():.2f}%')

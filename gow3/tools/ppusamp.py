#!/usr/bin/env python3
"""ppusamp.py <before> <after> [thread id hex]: PPU time by function between two snapshots of <RPCS3_SPU_SAMPLER>.ppu.
Functions are found in spu/EBOOT.elf by their stack frame set-up (stdu r1)."""
import collections, struct, sys, bisect
def load(p):
    d = collections.Counter()
    for l in open(p):
        i, pc, w, c = l.split(); d[(i, int(pc, 16), w)] = int(c)
    return d
a, b = load(sys.argv[1]), load(sys.argv[2])
tid = sys.argv[3] if len(sys.argv) > 3 else '01000000'
d = {k: v - a.get(k, 0) for k, v in b.items() if v - a.get(k, 0) > 0 and k[0] == tid}
elf = open(__file__.rsplit('/', 2)[0] + '/spu/EBOOT.elf', 'rb').read()
# program headers (ELF64 big endian)
phoff, = struct.unpack_from('>Q', elf, 0x20); phentsize, phnum = struct.unpack_from('>HH', elf, 0x36)
segs = []
for i in range(phnum):
    t, fl, off, va, pa, fsz, msz = struct.unpack_from('>IIQQQQQ', elf, phoff + i * phentsize)
    if t == 1 and fsz: segs.append((va, off, fsz))
starts = []
for va, off, fsz in segs:
    for o in range(0, fsz - 4, 4):
        w, = struct.unpack_from('>I', elf, off + o)
        if (w & 0xffff8003) == 0xf8218001: starts.append(va + o)   # stdu r1, -N(r1)
starts.sort()
def fn(pc):
    i = bisect.bisect_right(starts, pc) - 1
    return starts[i] if i >= 0 and pc - starts[i] < 0x4000 else pc
tot = sum(d.values()); run = collections.Counter(); wait = collections.Counter()
for (i, pc, w), v in d.items(): (wait if w == '1' else run)[fn(pc)] += v
print('samples', tot, 'running %.1f%% waiting %.1f%%' % (sum(run.values()) * 100 / tot, sum(wait.values()) * 100 / tot), 'functions seen', len(run))
acc = 0
for f, v in run.most_common(40):
    acc += v; print('  %06x %5.1f%%  cumulative %5.1f%%' % (f, v * 100 / tot, acc * 100 / tot))
print('waiting:', ' '.join('%x:%.1f%%' % (f, v * 100 / tot) for f, v in wait.most_common(6)))

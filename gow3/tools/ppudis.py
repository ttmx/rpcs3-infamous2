#!/usr/bin/env python3
"""ppudis.py <address hex> [bytes] | callers <address hex>: disassemble the game's PPU code at an address (the
executable's segments, so stripped parts work too), or list the bl instructions that target an address."""
import struct, subprocess, sys, tempfile, os
ELF = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'spu', 'EBOOT.elf')
d = open(ELF, 'rb').read()
phoff, = struct.unpack_from('>Q', d, 0x20); phentsize, phnum = struct.unpack_from('>HH', d, 0x36)
segs = []
for i in range(phnum):
    t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('>IIQQQQQQ', d, phoff + i * phentsize)
    if t == 1: segs.append((va, off, fsz))
def read(addr, n):
    for va, off, fsz in segs:
        if va <= addr < va + fsz: return d[off + addr - va: off + addr - va + n]
    sys.exit('address outside of the file')
if sys.argv[1] == 'callers':
    target = int(sys.argv[2], 16)
    for va, off, fsz in segs:
        for o in range(0, fsz - 3, 4):
            w, = struct.unpack_from('>I', d, off + o)
            if w >> 26 == 18 and w & 3 == 1:
                disp = w & 0x03fffffc
                if disp & 0x02000000: disp -= 0x04000000
                if va + o + disp == target: print('%x' % (va + o))
else:
    addr = int(sys.argv[1], 16); n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 128
    with tempfile.NamedTemporaryFile(suffix='.bin') as f:
        f.write(read(addr, n)); f.flush()
        out = subprocess.run(['llvm-objdump', '-D', '-b', 'binary', '--triple=powerpc64', '--adjust-vma=' + hex(addr), f.name], capture_output=True, text=True).stdout
    print('\n'.join(out.splitlines()[6:]))

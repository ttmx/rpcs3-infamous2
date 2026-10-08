#!/usr/bin/env python3
"""spufn.py <spu-cache.dat> <entry address hex> [index] [max instructions]: list the cached SPU functions that start
at an address; with an index, disassemble that one."""
import struct, sys, pathlib
sys.path.insert(0, '/home/tiago/Applications/rpcs3/profiling/native-investigation/build-prep/source/infamous2/reference/spu')
import spudis
cache = pathlib.Path(sys.argv[1]).read_bytes(); want = int(sys.argv[2], 16)
entries = []; off = 0
while off + 8 <= len(cache):
    crc, count, addr = struct.unpack_from('>HHI', cache, off); off += 8
    if not count or off + 4 * count > len(cache): break
    entries.append((addr, struct.unpack_from(f'>{count}I', cache, off))); off += 4 * count
sel = [e for e in entries if e[0] == want]
print(len(entries), 'functions in cache;', len(sel), 'start at', hex(want), 'sizes', [len(e[1]) for e in sel])
if len(sys.argv) > 3:
    addr, words = sel[int(sys.argv[3])]
    for i, w in enumerate(words[:int(sys.argv[4]) if len(sys.argv) > 4 else 400]):
        print(f'{addr + 4 * i:05x}: {w:08x}  {spudis.dis(w, addr + 4 * i)}')

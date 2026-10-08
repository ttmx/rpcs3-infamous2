#!/usr/bin/env python3
"""region.py: load a capture of RPCS3_SPU_REGION_CAPTURE (registers and local store at a region's entry) as an
interpreter state. load(path) -> spuemu.SPU at the region's pc; run_function(s) runs to the return address in r0."""
import sys, struct, numpy as np; sys.path.insert(0, __file__.rsplit('/', 1)[0])
import gj, spuemu
def load(path):
    d = open(path, 'rb').read(); assert d[:4] == b'SPUR'
    pc, = struct.unpack_from('<I', d, 4); g = np.frombuffer(d, dtype='<u4', count=512, offset=8).reshape(128, 4)
    return spuemu.SPU(d[8 + 2048:8 + 2048 + 0x40000], [g[i][::-1].astype(np.uint32).copy() for i in range(128)], pc)
def run_function(s, limit=5_000_000):
    ret = int(s.r[0][0]) & 0x3fffc; sp = int(s.r[1][0]); s.step()
    while not (s.pc == ret and int(s.r[1][0]) == sp) and s.count < limit: s.step()
    return s

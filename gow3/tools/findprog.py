#!/usr/bin/env python3
"""findprog.py <hash>...: find SPU program images in spu/EBOOT.elf by the sampler's program hash (FNV-1a of local store
0x4040..0x4140), for images loaded at 0x3000 or 0x4000 (and SPURS job binaries at other 16-byte offsets). Writes
spu/prog-<hash>.bin as a local store image."""
import numpy as np, sys, pathlib
root = pathlib.Path(__file__).resolve().parent.parent
d = np.frombuffer((root / 'spu/EBOOT.elf').read_bytes(), dtype=np.uint8)
want = {int(h, 16): h for h in sys.argv[1:]}
n = (len(d) - 0x200) // 16
pos = np.arange(n, dtype=np.int64) * 16
h = np.full(n, 0x811c9dc5, dtype=np.uint64)
for i in range(256):
    h = ((h ^ d[pos + i].astype(np.uint64)) * np.uint64(0x01000193)) & np.uint64(0xffffffff)
for hv, name in want.items():
    hits = pos[h == np.uint64(hv)]
    print(name, 'hash region at file offsets', [hex(int(x)) for x in hits[:6]])
    for x in hits[:1]:
        for base in (0x3000, 0x4000, 0x3800, 0x2800):
            start = int(x) - (0x4040 - base)
            img = bytes(d[start:start + 0x30000])
            ls = bytearray(0x40000); ls[base:base + len(img)] = img[:0x40000 - base]
            (root / f'spu/prog-{name}-base{base:x}.bin').write_bytes(ls)

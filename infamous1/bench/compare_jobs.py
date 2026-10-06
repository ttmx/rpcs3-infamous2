#!/usr/bin/env python3
"""Look for the inFamous 2 occlusion and lighting SPU code inside the inFamous 1 executable.
usage: compare_jobs.py <infamous2 elf> <infamous1 elf>"""
import sys, struct, pathlib
a = pathlib.Path(sys.argv[1]).read_bytes()
b = pathlib.Path(sys.argv[2]).read_bytes()
for name, e in (('inFamous 2', a), ('inFamous 1', b)):
    d = [p for p in range(0, len(e) - 4, 4) if e[p:p + 4] == b'\xda\x7a\xba\x5e']
    c = [p for p in range(0, len(e) - 4, 4) if e[p:p + 4] == b'\xc0\xde\xc0\xde']
    print(f'{name}: {len(e)} bytes, da7aba5e at {[hex(x) for x in d]}, c0dec0de at {[hex(x) for x in c]}')
N = 8  # words per shingle
index = {}
for p in range(0, len(b) - 4 * N, 4):
    index.setdefault(b[p:p + 4 * N], p)
regions = [('lighting job P071', 0x865540, 5918), ('occlusion dispatcher', 0x86b1b8, 2258),
           ('kernel-03 downsample', 0x86d500, 196), ('kernel-04 row blur', 0x86d880, 740), ('kernel-05 column blur', 0x86e480, 484),
           ('kernel-06 upsample', 0x86ec80, 740), ('kernel-07 raw occlusion', 0x86f880, 1348)]
for name, off, words in regions:
    hits = []
    for w in range(words - N):
        q = index.get(a[off + 4 * w: off + 4 * (w + N)])
        if q is not None: hits.append((w, q))
    print(f'{name}: {len(hits)} of {words - N} {N}-word windows also occur in inFamous 1', end='')
    if hits:
        deltas = {}
        for w, q in hits: deltas[q - 4 * w] = deltas.get(q - 4 * w, 0) + 1
        best = sorted(deltas.items(), key=lambda t: -t[1])[:3]
        print('; best alignments (file offset in inFamous 1: windows):', [(hex(k), v) for k, v in best])
    else: print()

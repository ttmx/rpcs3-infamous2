#!/usr/bin/env python3
"""kcheck.py <directory of RPCS3_SPU_KERNEL_CHECK> [relative tolerance]: every run of the built kernel (region-N.bin
before, after-N.bin after) against the SPU code run in the interpreter from the same state: local store quadwords
that differ as floats beyond the tolerance (0 = any difference), and the return register."""
import sys, glob, struct, numpy as np; sys.path.insert(0, __file__.rsplit('/', 1)[0])
import region
tol = float(sys.argv[2]) if len(sys.argv) > 2 else 0
runs = bad = words = 0; worst = 0.0; r3bad = 0
for p in sorted(glob.glob(sys.argv[1] + '/region-*.bin')):
    q = p.replace('region-', 'after-')
    try: after = open(q, 'rb').read()
    except FileNotFoundError: continue
    if len(after) < 8 + 2048 + 0x40000: continue
    s = region.load(p); region.run_function(s); runs += 1
    ref = np.frombuffer(bytes(s.ls), dtype='>u4'); got = np.frombuffer(after, dtype='>u4', count=0x10000, offset=8 + 2048)
    # the interpreter's stack use below the stack pointer is not the kernel's business
    sp = int(s.r[1][0]) & 0x3fff0
    diff = np.nonzero(ref != got)[0]; diff = diff[(diff * 4 >= sp) | (diff * 4 < sp - 0x400)]
    words += len(diff)
    if len(diff):
        a = ref[diff].view('>f4').astype(np.float64); b = got[diff].view('>f4').astype(np.float64)
        with np.errstate(all='ignore'): err = np.abs(a - b) / np.maximum(np.abs(a), 1e-3)
        err = np.nan_to_num(err, nan=1e9); worst = max(worst, float(err.max())); bad += int((err > tol).sum())
    g = np.frombuffer(after, dtype='<u4', count=512, offset=8).reshape(128, 4)
    if int(g[3][3]) != int(s.r[3][0]):
        a = struct.unpack('>f', struct.pack('>I', int(s.r[3][0])))[0]; b = struct.unpack('>f', struct.pack('>I', int(g[3][3])))[0]
        if tol == 0 or abs(a - b) > tol * max(abs(a), 1e-3): r3bad += 1
print('runs', runs, 'words that differ', words, 'beyond tolerance', bad, 'worst relative %.2e' % worst, 'return register differs', r3bad)

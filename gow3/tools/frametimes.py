#!/usr/bin/env python3
"""frametimes.py <trace file> <seconds>: arm the fork's frame interval trace (RPCS3_VK_FRAME_INTERVAL_TRACE=1,
RPCS3_VK_FRAME_INTERVAL_TRACE_PATH=<file>, which must not exist before the start), wait, and print the distribution
of the intervals between emulated flips."""
import mmap, struct, sys, time, collections
f = open(sys.argv[1], 'r+b'); m = mmap.mmap(f.fileno(), 0)
used0, = struct.unpack_from('<Q', m, 56)
struct.pack_into('<QQ', m, 40, 0, 2**63)      # start_ns, end_ns
struct.pack_into('<Q', m, 32, 2)              # arm_seq
time.sleep(float(sys.argv[2]))
struct.pack_into('<Q', m, 32, 0)
used, = struct.unpack_from('<Q', m, 56)
ts = []
for i in range(used0, min(used, 65536)):
    seq, ns, flip, payload, kind, flags, buf, _ = struct.unpack_from('<QQQQIIII', m, 128 + i * 48)
    if kind == 1 and flags & 1 and not flags & 2: ts.append(ns)
d = [(b - a) / 1e6 for a, b in zip(ts, ts[1:])]
print('flips', len(ts), 'fps %.1f' % (len(d) / (sum(d) / 1000)), 'interval ms mean %.1f median %.1f' % (sum(d) / len(d), sorted(d)[len(d) // 2]))
h = collections.Counter(int(x // 2) * 2 for x in d)
for k in sorted(h): print('%3d-%3d ms %5d %s' % (k, k + 2, h[k], '#' * (h[k] * 120 // len(d))))
print('first 40:', ' '.join('%.0f' % x for x in d[:40]))

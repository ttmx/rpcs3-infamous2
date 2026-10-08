#!/usr/bin/env python3
"""jitmap.py <pid> [cache dir]: write /tmp/perf-<pid>.map from RPCS3's ASMJIT/.objects so perf names JIT code.
The emulator only records objects when <cache>/rpcs3/ASMJIT exists before it starts."""
import mmap, struct, sys, pathlib
cache = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else pathlib.Path(__file__).resolve().parent.parent / 'cache')
with open(cache / 'rpcs3/ASMJIT/.objects', 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m, open(f'/tmp/perf-{sys.argv[1]}.map', 'w') as out:
    n = 0
    for pos in range(0, 0x10000000, 16):
        addr, size, off = struct.unpack_from('<QII', m, pos)
        if not addr: break
        end = m.find(b'\0', off + size, min(off + size + 4096, len(m)))
        if end < 0: continue
        out.write(f"{addr:x} {size:x} {m[off + size:end].decode('utf8', 'replace')}\n"); n += 1
    print(n, 'JIT symbols')

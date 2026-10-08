#!/usr/bin/env python3
"""jithot.py <perf.data> <jit symbol substring>: where inside one recompiled function the samples fall. Prints the
disassembly of the hottest stretches with sample counts (host code from the cache's ASMJIT/.objects)."""
import mmap, struct, sys, pathlib, subprocess, tempfile, collections, re
cache = pathlib.Path(__file__).resolve().parent.parent / 'cache'
want = sys.argv[2]
with open(cache / 'rpcs3/ASMJIT/.objects', 'rb') as f:
    m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    for pos in range(0, 0x10000000, 16):
        addr, size, off = struct.unpack_from('<QII', m, pos)
        if not addr: sys.exit('not found')
        end = m.find(b'\0', off + size, min(off + size + 4096, len(m)))
        name = m[off + size:end].decode('utf8', 'replace')
        if want in name: code = bytes(m[off:off + size]); break
ips = collections.Counter()
out = subprocess.run(['perf', 'script', '-i', sys.argv[1], '-F', 'ip'], capture_output=True, text=True).stdout
for l in out.split():
    try: ip = int(l, 16)
    except ValueError: continue
    if addr <= ip < addr + size: ips[ip] += 1
with tempfile.NamedTemporaryFile(suffix='.bin') as t:
    t.write(code); t.flush()
    dis = subprocess.run(['objdump', '-D', '-b', 'binary', '-m', 'i386:x86-64', '-M', 'intel', '--adjust-vma', hex(addr), t.name], capture_output=True, text=True).stdout
lines = []
for l in dis.splitlines():
    mm = re.match(r'\s*([0-9a-f]+):\t[0-9a-f ]+\t(.*)', l)
    if mm: lines.append((int(mm[1], 16), mm[2]))
total = sum(ips.values()); print(name, size, 'bytes', len(lines), 'instructions', total, 'samples')
cnt = [ips.get(a, 0) for a, _ in lines]
# hottest 60-instruction window
best = max(range(max(1, len(lines) - 60)), key=lambda i: sum(cnt[i:i + 60]))
print('hottest 60 instructions hold %.0f%% of the samples; all instructions with a sample: %d' % (sum(cnt[best:best + 60]) * 100 / max(total, 1), sum(1 for c in cnt if c)))
for i in range(best, min(best + 60, len(lines))): print('%5d  %x  %s' % (cnt[i], lines[i][0] - addr, lines[i][1]))

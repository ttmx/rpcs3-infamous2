#!/usr/bin/env python3
"""Match executed SPU code blocks (RPCS3 SPU cache) against raw bytes of the game executable and group them
into programs by (file offset - load address). Usage: inventory2.py <EBOOT.elf> <spu-cache.dat> [hot_addr ...]"""
import struct, sys, json, collections, pathlib
elf = pathlib.Path(sys.argv[1]).read_bytes()
cache = pathlib.Path(sys.argv[2]).read_bytes()
hot = [int(x, 16) for x in sys.argv[3:]]
entries = []
off = 0
while off + 8 <= len(cache):
    crc, count, addr = struct.unpack_from('>HHI', cache, off); off += 8
    if not count or off + 4 * count > len(cache): break
    d = cache[off:off + 4 * count]; off += 4 * count
    entries.append((addr, b''.join(d[i:i+4][::-1] for i in range(0, len(d), 4))))  # cache stores host-endian words
# verify byte order on a sample
hits = sum(1 for a, d in entries[:200] if elf.find(d[:32]) >= 0)
if hits < 20:
    entries = [(a, b''.join(d[i:i+4][::-1] for i in range(0, len(d), 4))) for a, d in entries]
programs = collections.defaultdict(lambda: dict(blocks=[], words=set()))
unmatched = []
for addr, d in entries:
    if len(d) < 32: continue
    probe = d[:48] if len(d) >= 48 else d
    p = elf.find(probe); found = False
    while p >= 0:
        if (p - addr) % 4 == 0:
            n = 0
            while n * 4 < len(d) and elf[p + n*4:p + n*4 + 4] == d[n*4:n*4 + 4]: n += 1
            if n * 4 >= min(len(d), 48):
                delta = p - addr
                pr = programs[delta]; pr['blocks'].append((addr, n, len(d)//4)); pr['words'].update(range(p // 4, p // 4 + n)); found = True
        p = elf.find(probe, p + 4)
    if not found: unmatched.append((addr, len(d)//4))
# merge deltas into programs; report
rows = []
for delta, pr in programs.items():
    lo = min(pr['words']) * 4; hi = max(pr['words']) * 4 + 4
    addrs = sorted(set(a for a, n, t in pr['blocks']))
    rows.append(dict(delta=delta, file_lo=lo, file_hi=hi, ls_lo=lo - delta, ls_hi=hi - delta, span_instr=(hi - lo)//4, executed_instr=len(pr['words']),
                     blocks=len(pr['blocks']), entry_points=len(addrs), hot=[hex(h) for h in hot if h in addrs]))
rows = [r for r in rows if r['executed_instr'] >= 40]
rows.sort(key=lambda r: r['file_lo'])
print(f'{len(entries)} executed blocks; matched into {len(rows)} programs; unmatched blocks: {len(unmatched)} ({sum(n for a, n in unmatched)} instr)')
for i, r in enumerate(rows):
    r['id'] = i
    print(f"P{i:02d} file 0x{r['file_lo']:07x}-0x{r['file_hi']:07x}  LS 0x{r['ls_lo']:05x}-0x{r['ls_hi']:05x}  span={r['span_instr']:6d} instr  executed={r['executed_instr']:6d} ({100*r['executed_instr']/r['span_instr']:5.1f}%)  blocks={r['blocks']:4d} entries={r['entry_points']:4d}  hot={r['hot']}")
json.dump(dict(programs=rows, unmatched=unmatched[:50]), open('inventory.json', 'w'), indent=1)

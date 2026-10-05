#!/usr/bin/env python3
"""detail.py: assign named hot blocks to programs and print never-executed ranges of given programs."""
import struct, pathlib, json, sys
elf = pathlib.Path('EBOOT-104.elf').read_bytes()
cache = pathlib.Path(sys.argv[1]).read_bytes()
inv = json.load(open('inventory.json'))
progs = [r for r in inv['programs'] if r['ls_lo'] in (0x3050, 0x4030) or r['executed_instr'] >= 1500]
off = 0; raw = []
while off + 8 <= len(cache):
    crc, count, addr = struct.unpack_from('>HHI', cache, off); off += 8
    if not count or off + 4 * count > len(cache): break
    raw.append((addr, cache[off:off + 4 * count])); off += 4 * count
flip = lambda d: b''.join(d[i:i+4][::-1] for i in range(0, len(d), 4))
direct = sum(1 for a, d in raw[:300] if len(d) >= 32 and elf.find(d[:32]) >= 0)
flipped = sum(1 for a, d in raw[:300] if len(d) >= 32 and elf.find(flip(d)[:32]) >= 0)
ents = raw if direct >= flipped else [(a, flip(d)) for a, d in raw]
def places(addr, d):
    p = elf.find(d[:48])
    while p >= 0:
        yield p
        p = elf.find(d[:48], p + 4)
def owner(addr, d):
    return sorted({r['id'] for p in places(addr, d) for r in progs if r['file_lo'] <= p < r['file_hi'] and p - addr == r['delta']})
want = {(0x44a8, 732): 'pNv gather/interp', (0x42c0, 283): 'sxE', (0x8ae8, 245): 'MJW', (0x7170, 181): '1w2k', (0x8a00, 58): 'ns7n', (0x6968, 183): 'ehgy', (0x4448, 620): 'g2YE', (0x41b0, 118): '8Yjs', (0x4250, 313): 'W81t', (0x74f0, 692): 'ryrn (SPU 0x200)'}
for a, d in ents:
    k = (a, len(d) // 4)
    if k in want: print(f"{want[k]:20s} entry 0x{a:05x} {len(d)//4:4d} instr -> {['P%03d' % o for o in owner(a, d)]}")
for pid in [int(x) for x in sys.argv[2:]]:
    r = [x for x in progs if x['id'] == pid][0]
    cov = set()
    for a, d in ents:
        if len(d) < 32: continue
        for p in places(a, d):
            if p - a == r['delta']:
                n = 0
                while n * 4 < len(d) and elf[p + n*4:p + n*4 + 4] == d[n*4:n*4 + 4]: n += 1
                cov.update(range(p // 4, p // 4 + n))
    g = []; start = None
    for w in range(r['file_lo'] // 4, r['file_hi'] // 4):
        if w not in cov:
            if start is None: start = w
        elif start is not None: g.append((start, w)); start = None
    if start is not None: g.append((start, r['file_hi'] // 4))
    g = [(s * 4 - r['delta'], e - s) for s, e in g]
    print(f"\nP{pid:03d} LS 0x{r['ls_lo']:05x}-0x{r['ls_hi']:05x} ({r['span_instr']} instr): {sum(n for _, n in g)} never executed in {len(g)} ranges; ranges >= 12 instr:")
    print('   ' + ', '.join(f"0x{a:05x}+{n}" for a, n in g if n >= 12))

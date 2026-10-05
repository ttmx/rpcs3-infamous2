#!/usr/bin/env python3
"""coverage.py <spu-cache.dat> : per-program executed coverage by file position (cache blocks may start
below their entry address, so position inside the executable is used, not the load address)."""
import struct, pathlib, json, sys, collections
elf = pathlib.Path('EBOOT-104.elf').read_bytes()
cache = pathlib.Path(sys.argv[1]).read_bytes()
inv = json.load(open('inventory.json'))
progs = sorted([r for r in inv['programs'] if r['ls_lo'] in (0x3050, 0x4030) or r['executed_instr'] >= 1500], key=lambda r: r['file_lo'])
# program boundaries: each main program runs until the next one starts
for i, r in enumerate(progs):
    r['end'] = max(r['file_hi'], 0)
off = 0; raw = []
while off + 8 <= len(cache):
    crc, count, addr = struct.unpack_from('>HHI', cache, off); off += 8
    if not count or off + 4 * count > len(cache): break
    raw.append((addr, cache[off:off + 4 * count])); off += 4 * count
flip = lambda d: b''.join(d[i:i+4][::-1] for i in range(0, len(d), 4))
direct = sum(1 for a, d in raw[:300] if len(d) >= 32 and elf.find(d[:32]) >= 0)
flipped = sum(1 for a, d in raw[:300] if len(d) >= 32 and elf.find(flip(d)[:32]) >= 0)
ents = raw if direct >= flipped else [(a, flip(d)) for a, d in raw]
cov = collections.defaultdict(set); owner = {}
unmatched = 0
for idx, (a, d) in enumerate(ents):
    # blocks can contain zero-filled holes: match runs of non-zero words of at least 6 instructions
    words = [d[i:i+4] for i in range(0, len(d), 4)]
    i = 0; hit = False
    while i < len(words):
        if words[i] == b'\0\0\0\0': i += 1; continue
        j = i
        while j < len(words) and words[j] != b'\0\0\0\0': j += 1
        run = b''.join(words[i:j])
        if j - i >= 6:
            p = elf.find(run)
            while p >= 0:
                for r in progs:
                    if r['file_lo'] - 0x400 <= p < r['file_hi'] + 0x4000 and p % 4 == 0:
                        cov[r['id']].update(range(p // 4, p // 4 + (j - i))); hit = True
                        owner.setdefault(idx, set()).add(r['id'])
                p = elf.find(run, p + 4)
        i = j
    unmatched += not hit
print(f'{len(ents)} blocks, {unmatched} not found in the executable')
out = []
for r in progs:
    c = cov[r['id']]
    lo = min(min(c), r['file_lo'] // 4) if c else r['file_lo'] // 4; hi = max(max(c) + 1, r['file_hi'] // 4) if c else r['file_hi'] // 4
    span = hi - lo
    out.append(dict(id=r['id'], file_lo=lo * 4, file_hi=hi * 4, ls_lo=r['ls_lo'], span=span, executed=len(c)))
    print(f"P{r['id']:03d} file 0x{lo*4:07x}-0x{hi*4:07x} span={span:6d} executed={len(c):6d} ({100*len(c)/span:5.1f}%)  never-run={span-len(c):5d}")
print('total span', sum(o['span'] for o in out), 'executed', sum(o['executed'] for o in out))
json.dump(dict(programs=out), open('coverage.json', 'w'), indent=1)
want = {(0x44a8, 732): 'pNv', (0x42c0, 283): 'sxE', (0x8ae8, 245): 'MJW', (0x7170, 181): '1w2k', (0x8a00, 58): 'ns7n', (0x6968, 183): 'ehgy', (0x4448, 620): 'g2YE', (0x41b0, 118): '8Yjs', (0x4250, 313): 'W81t', (0x74f0, 692): 'ryrn', (0xd410, 0): 'Z7np'}
for idx, (a, d) in enumerate(ents):
    if (a, len(d) // 4) in want: print(f"{want[(a, len(d)//4)]:5s} entry 0x{a:05x} {len(d)//4:4d} instr -> {sorted('P%03d' % o for o in owner.get(idx, []))}")

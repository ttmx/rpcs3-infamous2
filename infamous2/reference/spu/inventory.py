#!/usr/bin/env python3
"""Static inventory of SPU programs embedded in the game executable, overlaid with RPCS3's SPU cache
(code that has actually executed). Usage: inventory.py <EBOOT.elf> <spu-cache.dat> [hot_addr ...]"""
import struct, sys, json, collections, pathlib
elf = pathlib.Path(sys.argv[1]).read_bytes()
cache = pathlib.Path(sys.argv[2]).read_bytes()
hot = [int(x, 16) for x in sys.argv[3:]]

# --- embedded SPU ELF images: 32-bit big-endian ELF, e_machine 23
images = []
pos = 0
while True:
    pos = elf.find(b'\x7fELF\x01\x02\x01', pos)
    if pos < 0: break
    e_type, e_machine = struct.unpack_from('>HH', elf, pos + 16)
    if e_machine == 23:
        e_entry, e_phoff, e_shoff = struct.unpack_from('>III', elf, pos + 24)
        e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from('>HHHH', elf, pos + 42)
        segs = []
        end = 52
        for i in range(e_phnum):
            p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from('>IIIIIIII', elf, pos + e_phoff + i * e_phentsize)
            if p_type == 1 and p_filesz:
                segs.append(dict(vaddr=p_vaddr, off=p_offset, filesz=p_filesz, memsz=p_memsz, exec=bool(p_flags & 1)))
                end = max(end, p_offset + p_filesz)
        end = max(end, e_shoff + e_shnum * e_shentsize)
        images.append(dict(file_off=pos, entry=e_entry, segs=segs, size=end, etype=e_type))
    pos += 4

def seg_bytes(img, seg): 
    a = img['file_off'] + seg['off']; return elf[a:a + seg['filesz']]

# --- SPU cache entries: BE u16 crc, u16 count, u32 addr, count * u32
entries = []
off = 0
while off + 8 <= len(cache):
    crc, count, addr = struct.unpack_from('>HHI', cache, off); off += 8
    if not count or off + 4 * count > len(cache): break
    entries.append((addr, cache[off:off + 4 * count])); off += 4 * count
print(f'{len(images)} SPU images in executable; {len(entries)} executed code blocks in SPU cache ({sum(len(d) for _, d in entries)//4} instructions)')

# word order in cache: detect by trying both against images
def variants(d):
    yield d
    yield b''.join(d[i:i+4][::-1] for i in range(0, len(d), 4))

cov = [dict() for _ in images]          # image -> {seg index: set of covered word offsets}
entry_owner = collections.defaultdict(list)
unmatched = 0
for ei, (addr, data) in enumerate(entries):
    found = False
    for v in variants(data):
        probe = v[:min(len(v), 64)]
        for ii, img in enumerate(images):
            for si, seg in enumerate(img['segs']):
                b = seg_bytes(img, seg)
                p = b.find(probe)
                while p >= 0:
                    if p % 4 == 0:
                        # count matching leading words (blocks may contain patched/relocated words later)
                        n = 0
                        while n * 4 < len(v) and b[p + n*4:p + n*4 + 4] == v[n*4:n*4 + 4]: n += 1
                        s = cov[ii].setdefault(si, set())
                        s.update(range(p // 4, p // 4 + n))
                        entry_owner[ei].append((ii, seg['vaddr'] + p, n, len(v)//4))
                        found = True
                    p = b.find(probe, p + 4)
        if found: break
    unmatched += not found
print(f'cache blocks matched to an image by bytes: {len(entry_owner)}; unmatched: {unmatched}')

rows = []
for ii, img in enumerate(images):
    code = sum(s['filesz'] for s in img['segs'] if s['exec']) // 4
    covered = sum(len(v) for si, v in cov[ii].items() if img['segs'][si]['exec'])
    blocks = sum(1 for ei, o in entry_owner.items() if any(x[0] == ii for x in o))
    hot_hits = [hex(h) for h in hot if any(x[0] == ii for ei, o in entry_owner.items() if entries[ei][0] == h for x in o)]
    rows.append(dict(image=ii, file_off=hex(img['file_off']), entry=hex(img['entry']), load=[hex(s['vaddr']) for s in img['segs']], code_instr=code, executed_instr=covered, pct=round(100 * covered / code, 1) if code else 0, cache_blocks=blocks, hot=hot_hits))
for r in sorted(rows, key=lambda r: -r['code_instr']):
    print(f"img{r['image']:2d} @{r['file_off']:>9} entry={r['entry']:>7} load={','.join(r['load']):18s} code={r['code_instr']:6d} instr  executed={r['executed_instr']:6d} ({r['pct']:5.1f}%) blocks={r['cache_blocks']:4d} hot={r['hot']}")
json.dump(dict(images=rows), open('inventory.json', 'w'), indent=1)

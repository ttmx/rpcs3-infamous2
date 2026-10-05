#!/usr/bin/env python3
"""Enumerate SPU code containers embedded in the game executable (writes jobs.json).
 Type D ("da7aba5e"): header at LS 0x4000: 16-byte id, magic da7aba5e at +0x10, code offset 0x30 at +0x14, image size at +0x18.
 Type C ("c0dec0de"): 8-byte id, +0x08 code size, +0x0c total size, +0x10 ?, +0x14 header size, +0x18 magic c0dec0de,
                      +0x1c ?, +0x20 load address, +0x24 ?, code follows at +0x28."""
import struct, pathlib, json
elf = pathlib.Path('EBOOT-104.elf').read_bytes()
jobs = []
p = 0
while True:
    p = elf.find(b'\xda\x7a\xba\x5e', p)
    if p < 0: break
    if p % 4 == 0:
        hdr = p - 0x10; code_off, size = struct.unpack_from('>II', elf, p + 4)
        if code_off == 0x30 and 0x40 <= size <= 0x3c000:
            jobs.append(dict(kind='D', file=hdr, load=0x4000, code_file=hdr + 0x30, code_ls=0x4030, size=size, id=elf[hdr:hdr + 16].hex()))
    p += 4
p = 0
while True:
    p = elf.find(b'\xc0\xde\xc0\xde', p)
    if p < 0: break
    if p % 4 == 0:
        hdr = p - 0x18
        code_size, total, x, hsize = struct.unpack_from('>IIII', elf, hdr + 8)
        y, load, z = struct.unpack_from('>III', elf, p + 4)
        if hsize == 0x30 and 0x40 <= code_size <= 0x3c000 and load < 0x40000:
            jobs.append(dict(kind='C', file=hdr, load=load, code_file=hdr + 0x28, code_ls=load + 0x30, size=code_size, total=total, id=elf[hdr:hdr + 8].hex(), x=x, y=y, z=z))
    p += 4
jobs.sort(key=lambda j: j['file'])
for i, j in enumerate(jobs): j['n'] = i
json.dump(jobs, open('jobs.json', 'w'), indent=1)
if __name__ == '__main__':
    print(len(jobs), 'containers;', sum(1 for j in jobs if j['kind'] == 'D'), 'type D,', sum(1 for j in jobs if j['kind'] == 'C'), 'type C; total', sum(j['size'] for j in jobs) // 4, 'words')
    for j in jobs: print(f"J{j['n']:03d} {j['kind']} file 0x{j['file']:07x} code LS 0x{j['code_ls']:05x} size 0x{j['size']:05x} ({j['size']//4:6d} words) id {j['id'][:16]}")

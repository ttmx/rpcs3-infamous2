#!/usr/bin/env python3
"""jitdis.py <symbol substring> [cache dir]: host code of a JIT function from RPCS3's ASMJIT/.objects, disassembled
(objdump), followed by a mnemonic count."""
import mmap, struct, sys, pathlib, subprocess, tempfile, collections
cache = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else pathlib.Path(__file__).resolve().parent.parent / 'cache')
with open(cache / 'rpcs3/ASMJIT/.objects', 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
    for pos in range(0, 0x10000000, 16):
        addr, size, off = struct.unpack_from('<QII', m, pos)
        if not addr: sys.exit('not found')
        end = m.find(b'\0', off + size, min(off + size + 4096, len(m)))
        name = m[off + size:end].decode('utf8', 'replace')
        if sys.argv[1] in name:
            with tempfile.NamedTemporaryFile(suffix='.bin') as t:
                t.write(m[off:off + size]); t.flush()
                out = subprocess.run(['objdump', '-D', '-b', 'binary', '-m', 'i386:x86-64', '-M', 'intel', '--adjust-vma', hex(addr), t.name], capture_output=True, text=True).stdout
            lines = [l for l in out.splitlines() if '\t' in l]
            print(name, size, 'bytes', len(lines), 'instructions')
            print('\n'.join(lines))
            c = collections.Counter(l.split('\t')[2].split()[0] for l in lines if len(l.split('\t')) > 2)
            print(c.most_common(40)); break

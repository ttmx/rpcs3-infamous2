#!/usr/bin/env python3
"""Minimal SPU disassembler generated from RPCS3's SPUOpcodes.h table.
   spudis.py <file> <file_offset> <ls_address> <instruction_count>   (big-endian words)
   As a module: dis(word, pc) -> text; decode(word) -> (name, fields)"""
import re, struct, sys, pathlib
SRC = pathlib.Path(__file__).resolve().parents[3] / 'rpcs3/Emu/Cell/SPUOpcodes.h'
TABLE = [None] * 2048
for magn, value, name in re.findall(r'\{\s*(\d+),\s*0x([0-9a-fA-F]+),\s*GET\((\w+)\)\s*\}', SRC.read_text()):
    magn = int(magn); value = int(value, 16)
    for i in range(1 << magn):
        TABLE[(value << magn) | i] = (name, magn)
I7 = {'SHLHI','SHLI','ROTHI','ROTI','ROTHMI','ROTMI','ROTMAHI','ROTMAI','SHLQBII','SHLQBYI','ROTQBII','ROTQBYI','ROTQMBII','ROTQMBYI','CBD','CHD','CWD','CDD'}
I8 = {'CFLTS','CFLTU','CSFLT','CUFLT'}
UNARY = {'CLZ','CNTB','FSM','FSMH','FSMB','GB','GBH','GBB','XSBH','XSHW','XSWD','FREST','FRSQEST','ORX','FESD','FRDS'}
def sext(v, bits): return v - (1 << bits) if v & (1 << (bits - 1)) else v
def decode(w):
    e = TABLE[w >> 21]
    if not e: return ('.word', {})
    name, magn = e
    rt = w & 0x7f; ra = (w >> 7) & 0x7f; rb = (w >> 14) & 0x7f
    f = dict(rt=rt, ra=ra, rb=rb, magn=magn)
    if magn == 7: f.update(rt4=(w >> 21) & 0x7f, rc=rt)                    # RRR
    elif magn == 4: f.update(i18=(w >> 7) & 0x3ffff)                      # RI18
    elif magn == 3: f.update(si10=sext((w >> 14) & 0x3ff, 10))            # RI10
    elif magn == 2: f.update(i16=(w >> 7) & 0xffff)                       # RI16
    elif magn == 1: f.update(i8=(w >> 14) & 0xff)
    else: f.update(i7=rb, si7=sext(rb, 7), i8=(w >> 14) & 0xff)
    return (name, f)
def dis(w, pc=0):
    name, f = decode(w)
    if name == '.word': return f'.word 0x{w:08x}'
    m = f['magn']; r = lambda n: f'r{n}'
    if m == 7: return f"{name:8s} {r(f['rt4'])}, {r(f['ra'])}, {r(f['rb'])}, {r(f['rc'])}"
    if m == 4: return f"{name:8s} {r(f['rt'])}, 0x{f['i18']:x}"
    if m == 3:
        if name in ('LQD', 'STQD'): return f"{name:8s} {r(f['rt'])}, {f['si10'] * 16}({r(f['ra'])})"
        return f"{name:8s} {r(f['rt'])}, {r(f['ra'])}, {f['si10']}"
    if m == 2:
        i16 = f['i16']
        if name in ('BR','BRSL','BRZ','BRNZ','BRHZ','BRHNZ','LQR','STQR'): tgt = (pc + (sext(i16, 16) << 2)) & 0x3fffc; return f"{name:8s} {r(f['rt'])}, 0x{tgt:05x}"
        if name in ('BRA','BRASL','LQA','STQA'): return f"{name:8s} {r(f['rt'])}, 0x{(i16 << 2) & 0x3fffc:05x}"
        return f"{name:8s} {r(f['rt'])}, 0x{i16:x}"
    if name in I8: return f"{name:8s} {r(f['rt'])}, {r(f['ra'])}, {f['i8']}"
    if m == 1: return f"{name:8s} {r(f['rt'])}, {r(f['ra'])}, {f['i8']}"
    if name in I7: return f"{name:8s} {r(f['rt'])}, {r(f['ra'])}, {f['si7']}"
    if name in ('RDCH', 'RCHCNT', 'WRCH'): return f"{name:8s} {r(f['rt'])}, ch{f['ra']}"
    if name in ('BI','BISL','BIZ','BINZ','BIHZ','BIHNZ','IRET','BISLED'): return f"{name:8s} {r(f['rt'])}, {r(f['ra'])}"
    if name in ('STOP','NOP','LNOP','SYNC','DSYNC','STOPD'): return f"{name:8s} 0x{w & 0x3fff:x}" if name == 'STOP' else name
    if name in UNARY: return f"{name:8s} {r(f['rt'])}, {r(f['ra'])}"
    return f"{name:8s} {r(f['rt'])}, {r(f['ra'])}, {r(f['rb'])}"
if __name__ == '__main__':
    data = pathlib.Path(sys.argv[1]).read_bytes(); off = int(sys.argv[2], 0); pc = int(sys.argv[3], 0); n = int(sys.argv[4], 0)
    for i in range(n):
        w, = struct.unpack_from('>I', data, off + 4 * i)
        print(f"{pc + 4 * i:05x}: {w:08x}  {dis(w, pc + 4 * i)}")

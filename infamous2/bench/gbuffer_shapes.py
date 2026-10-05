#!/usr/bin/env python3
"""gbuffer_shapes.py <trace.txt> [color hex] [examples]: FIFO command sequences between consecutive draws of one colour target
(default the G-buffer, 0xc0ab8000), with constants and texture slots folded, and how often each register appears."""
import collections, re, sys, pathlib
names = {}
enums = pathlib.Path(__file__).resolve().parent.parent / 'source/rpcs3/Emu/RSX/gcm_enums.h'
for m in re.finditer(r'^\s*(NV\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*>>\s*2', enums.read_text(), re.M): names.setdefault(int(m[2], 16), m[1])
def nm(r):
    if r == 0xffffffff: return 'FLOW'
    b = max((k for k in names if k <= r), default=None)
    return (names[b].replace('NV4097_', '') + (f'+{r-b:x}' if r != b else '')) if b is not None else hex(r)
target = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0xc0ab8000
draws = []; F = None
for l in open(sys.argv[1]):
    p = l.split()
    if p[0] == 'F': F = [(int(a.split(':')[0], 16), int(a.split(':')[1], 16)) for a in p[1:]]
    elif p[0] == 'D':
        draws.append(dict(frame=int(p[1]), color=int(p[15], 16), depth=int(p[16], 16), vp=p[13], F=F)); F = None
fr = sorted(set(d['frame'] for d in draws))[1]
sh = [d for d in draws if d['frame'] == fr and d['color'] == target and d['F'] is not None]
def fold(k):
    k = re.sub(r'TRANSFORM_CONSTANT\+\w+', 'TRANSFORM_CONSTANT', k)
    k = re.sub(r'(SET_TEXTURE_\w+?)\+\w+', r'\1', k)
    k = re.sub(r'(VERTEX_DATA_ARRAY_(OFFSET|FORMAT))\+\w+', r'\1', k)
    return k
def shape(F):
    out = []; prev = None; n = 0
    for r, v in F:
        k = fold(nm(r)) if r != 0xffffffff else f'FLOW{v}'
        if k == prev: n += 1
        else:
            if prev: out.append(prev + (f'x{n}' if n > 1 else ''))
            prev = k; n = 1
    if prev: out.append(prev + (f'x{n}' if n > 1 else ''))
    return ' '.join(out)
pat = collections.Counter(shape(d['F']) for d in sh)
print(len(sh), 'draws;', len(pat), 'distinct command shapes')
tot = 0
for k, v in pat.most_common(25):
    tot += v; print(f'{v:5d} (cum {100*tot/len(sh):.0f}%)  {k[:400]}')
reg = collections.Counter(); words = collections.Counter()
for d in sh:
    seen = set()
    for r, v in d['F']:
        k = fold(nm(r)) if r != 0xffffffff else f'FLOW{v}'
        words[k] += 1; seen.add(k)
    for k in seen: reg[k] += 1
print('\nregister: draws it precedes / words per frame')
for k, v in reg.most_common(60): print(f'  {v:5d} {words[k]:6d}  {k}')
print('words per draw', sum(words.values()) / max(1, len(sh)))
for top, _ in pat.most_common(int(sys.argv[3]) if len(sys.argv) > 3 else 2):
    print('\nexample of:', top[:120])
    n = 0
    for d in sh:
        if shape(d['F']) == top:
            print('  ' + ' '.join(f'{nm(r)}={v:x}' for r, v in d['F'])[:1500]); n += 1
            if n == 2: break

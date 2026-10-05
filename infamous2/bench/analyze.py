#!/usr/bin/env python3
"""analyze.py <trace.txt>: cross-frame stability of the vertex/index sources recorded by RPCS3_VK_GEOM_TRACE."""
import sys, collections
frames = collections.defaultdict(list)   # frame -> [draw dict]
cur = None
for line in open(sys.argv[1]):
    p = line.split()
    if p[0] == 'D':
        cur = dict(frame=int(p[1]), n=int(p[2]), cmd=int(p[3]), prim=int(p[4]), vbase=int(p[5]), vcount=int(p[6]), dcount=int(p[7]),
                   ibase=int(p[8]), itype=int(p[9]), iaddr=int(p[10], 16), ibytes=int(p[11]), ihash=p[12], vp=p[13], omask=p[14],
                   color=int(p[15], 16), depth=int(p[16], 16), nblocks=int(p[17]), pbytes=int(p[18]), vbytes=int(p[19]), imm=int(p[20]),
                   baseoff=int(p[21], 16), blocks=[])
        frames[cur['frame']].append(cur)
    elif p[0] == 'B':
        cur['blocks'].append(dict(addr=int(p[4], 16), len=int(p[5]), stride=int(p[6]), loc=int(p[7]), hash=p[11], attrs=p[12] if len(p) > 12 else ''))
fl = sorted(frames)
print('frames', fl)
def region(a):
    return 'local' if a >= 0xc0000000 else 'main'
for f in fl:
    d = frames[f]
    nb = sum(len(x['blocks']) for x in d)
    print(f"frame {f}: draws {len(d)} blocks {nb} persistent bytes {sum(x['pbytes'] for x in d)/1e6:.2f} MB volatile {sum(x['vbytes'] for x in d)/1e6:.2f} MB index bytes {sum(x['ibytes'] for x in d)/1e6:.2f} MB cmds {dict(collections.Counter(x['cmd'] for x in d))} prims {dict(collections.Counter(x['prim'] for x in d))}")
# Cross-frame classification of the blocks of each frame against the previous frame
for a, b in zip(fl, fl[1:]):
    prev_addr = {}; prev_hash = set()
    for x in frames[a]:
        for k in x['blocks']:
            prev_addr[(k['addr'], k['len'])] = k['hash']; prev_hash.add((k['hash'], k['len']))
    cls = collections.Counter(); byt = collections.Counter(); seen = set(); uniq = collections.Counter()
    for x in frames[b]:
        for k in x['blocks']:
            key = (k['addr'], k['len'])
            if key in prev_addr:
                c = 'same-address-same-bytes' if prev_addr[key] == k['hash'] else 'same-address-changed'
            elif (k['hash'], k['len']) in prev_hash:
                c = 'moved-same-bytes'
            else:
                c = 'new'
            c += ' ' + region(k['addr'])
            cls[c] += 1; byt[c] += k['len']
            if key not in seen:
                seen.add(key); uniq[c] += k['len']
    tot = sum(byt.values())
    print(f'\nblocks of frame {b} vs frame {a}: total {sum(cls.values())} blocks, {tot/1e6:.2f} MB requested, {sum(uniq.values())/1e6:.2f} MB distinct ranges')
    for c in sorted(cls, key=lambda c: -byt[c]):
        print(f'  {c:36s} {cls[c]:6d} blocks {byt[c]/1e6:7.2f} MB ({100*byt[c]/tot:5.1f}%)  distinct {uniq[c]/1e6:6.2f} MB')
    # the same for index buffers
    pi = {}
    for x in frames[a]:
        if x['ibytes']: pi[(x['iaddr'], x['ibytes'])] = x['ihash']
    ic = collections.Counter(); ib = collections.Counter(); ph = set(pi.values())
    for x in frames[b]:
        if not x['ibytes']: continue
        key = (x['iaddr'], x['ibytes'])
        c = ('same-address-same-bytes' if pi[key] == x['ihash'] else 'same-address-changed') if key in pi else ('moved-same-bytes' if x['ihash'] in ph else 'new')
        c += ' ' + region(x['iaddr'])
        ic[c] += 1; ib[c] += x['ibytes']
    print('  index buffers:', {c: (ic[c], round(ib[c]/1e6, 2)) for c in ic})
# Within-frame repetition
f = fl[-1]
seen = collections.Counter()
for x in frames[f]:
    for k in x['blocks']: seen[(k['addr'], k['len'])] += 1
print(f'\nframe {f}: distinct ranges {len(seen)}, requests {sum(seen.values())}, repeat histogram {sorted(collections.Counter(seen.values()).items())[:8]}')
# Render targets
rt = collections.Counter(); rb = collections.Counter()
for x in frames[f]:
    k = (hex(x['color']), hex(x['depth'])); rt[k] += 1; rb[k] += x['pbytes']
print('draws per (color, depth) target:')
for k, v in rt.most_common(14): print('  ', k, v, f'{rb[k]/1e6:.2f} MB')
# Layouts
lay = collections.Counter(); lb = collections.Counter()
for x in frames[f]:
    k = ' | '.join(f"s{b['stride']} " + ','.join(':'.join(a.split(':')[:3]) for a in b['attrs'].split(';') if a) for b in x['blocks'])
    lay[k] += 1; lb[k] += x['pbytes']
print('vertex layouts (stride attr:type:size):')
for k, v in lay.most_common(14): print(f'  {v:5d} {lb[k]/1e6:6.2f} MB  {k}')
print('vertex programs:', len(set(x['vp'] for x in frames[f])))
import statistics
vc = [x['vcount'] for x in frames[f]]
print('vertices/draw median', statistics.median(vc), 'mean', round(statistics.mean(vc), 1), 'max', max(vc), '; draws with <=64 vertices', sum(v <= 64 for v in vc))

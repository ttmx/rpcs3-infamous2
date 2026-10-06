#!/usr/bin/env python3
"""passes.py <trace.txt> [frame index]: one RPCS3_VK_GEOM_TRACE frame by render target: size, draws, vertices,
shader count, and the most common command sequences between consecutive draws."""
import sys, collections
regs = {}; frames = collections.OrderedDict(); pend_f = None
for line in open(sys.argv[1]):
    p = line.split()
    if p[0] == 'F': pend_f = [w.split(':')[0] for w in p[1:]]
    elif p[0] == 'R':
        for w in p[1:]:
            if ':' in w: r, v = w.split(':'); regs[int(r, 16)] = int(v, 16)
    elif p[0] == 'D':
        d = dict(frame=int(p[1]), prim=int(p[4]), dcount=int(p[7]), itype=int(p[9]), vp=p[13], color=p[15], depth=p[16], nblocks=int(p[17]),
                 w=regs.get(0x200, 0) >> 16, h=regs.get(0x204, 0) >> 16, fmt=regs.get(0x208, 0), f=tuple(pend_f or ()))
        frames.setdefault(d['frame'], []).append(d); pend_f = None
names = {'1830': 'cull', '32c': 'depth_test', '314': 'blend', '330': 'depthmask?', '1fc0': 'vp_const_load', '1efc': 'vp_const', '1808': 'begin_end', '1814': 'draw_arrays',
         '1824': 'draw_index', '181c': 'index_addr', '1680': 'vtx_off0', '1740': 'vtx_fmt0', '1a00': 'tex0_off', '8e4': 'fp_addr', '1d60': 'fp_ctrl', '1e9c': 'vp_start', '1ff0': 'vp_mask'}
fl = list(frames); f = fl[int(sys.argv[2]) if len(sys.argv) > 2 else len(fl) - 1]
draws = frames[f]; print('frames', fl, 'showing', f, 'draws', len(draws))
order = []; groups = collections.OrderedDict()
for d in draws:
    key = (d['color'], d['depth'], d['w'], d['h'])
    groups.setdefault(key, []).append(d)
for (color, depth, w, h), ds in groups.items():
    shapes = collections.Counter(tuple(sorted(set(x['f']))) for x in ds)
    print(f"\ncolor {color} depth {depth} {w}x{h}: {len(ds)} draws, {sum(x['dcount'] for x in ds)} vertices/indices, prims {dict(collections.Counter(x['prim'] for x in ds))}, "
          f"{len(set(x['vp'] for x in ds))} vertex programs, words between draws avg {sum(len(x['f']) for x in ds)/len(ds):.1f}, vertex count median {sorted(x['dcount'] for x in ds)[len(ds)//2]}")
    for shape, n in shapes.most_common(4):
        print(f"   {n:5d}x  " + ' '.join(names.get(r, r) for r in shape)[:230])

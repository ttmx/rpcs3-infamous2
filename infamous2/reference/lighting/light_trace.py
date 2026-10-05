#!/usr/bin/env python3
"""Dataflow trace of the lighting job on one tile.  light_trace.py <tile index>  -> trace-<tile>.pkl"""
import sys, struct, pickle, numpy as np
sys.path.insert(0, '../ssao'); sys.path.insert(0, '../spu')
import light_emu, tracer, spuemu
def trace(tile, max_tiles=1):
    def before(mem): mem[0xa93000][0xa00:0xa06] = struct.pack('>HHH', 0, tile, tile)
    tracer.NODES.clear()
    s, mem, stats = light_emu.make(50, cls=tracer.Tracer, before=before)
    try:
        while stats['puts'] < 4 * max_tiles:      # a tile is written back as normals + depth blocks (PUT lists)
            if s.pc == 0x1760: s.pc = int(s.r[0][0]) & 0x3fffc
            s.step()
    except light_emu.Done as e: print('done:', e)
    print('instructions', s.count, 'nodes', len(tracer.NODES), stats, 'outputs', len(s.out))
    return s, mem
if __name__ == '__main__':
    tile = int(sys.argv[1]); s, mem = trace(tile)
    pickle.dump(dict(nodes=tracer.NODES, out=s.out, src=s.src, lt=s.lt, A=bytes(mem[light_emu.A_EA]), B=bytes(mem[light_emu.B_EA])), open(f'trace-{tile}.pkl', 'wb'))
    eas = sorted(s.out); print('output EAs: 0x%x .. 0x%x' % (eas[0], eas[-1]), 'tagged', sum(1 for e in eas if s.out[e] is not None))

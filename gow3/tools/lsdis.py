#!/usr/bin/env python3
"""lsdis.py <ls dump> <start hex> <end hex>: disassemble a range of an SPU local store dump (RPCS3_TMP_LS_DUMP)."""
import struct, sys
sys.path.insert(0, '/home/tiago/Applications/rpcs3/profiling/native-investigation/build-prep/source/infamous2/reference/spu')
import spudis
d = open(sys.argv[1], 'rb').read(); a = int(sys.argv[2], 16); b = int(sys.argv[3], 16)
for pc in range(a, b, 4):
    w = struct.unpack_from('>I', d, pc)[0]
    print(f'{pc:05x}: {w:08x}  {spudis.dis(w, pc)}')

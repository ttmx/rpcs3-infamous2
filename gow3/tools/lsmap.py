#!/usr/bin/env python3
"""lsmap.py <ls dump>: functions of an SPU local store dump (those that save the link register), who calls each, and
the call stack at the time of the dump."""
import struct, sys, collections
d = open(sys.argv[1], 'rb').read()
W = lambda pc: struct.unpack_from('>I', d, pc)[0]
starts = [pc for pc in range(0x2800, 0x20000, 4) if W(pc) == 0x24004080]
calls = collections.defaultdict(list)
for pc in range(0x2800, 0x20000, 4):
    w = W(pc)
    if (w >> 23) == 0x66:
        off = (w >> 7) & 0xffff
        if off & 0x8000: off -= 0x10000
        calls[(pc + off * 4) & 0x3fffc].append(pc)
    if (w >> 23) == 0x62: calls[((w >> 7) & 0xffff) * 4].append(pc)
fn = lambda pc: max([s for s in starts if s <= pc], default=0)
targets = sorted(set(calls) | set(starts))
for s in targets:
    if 0x2800 <= s < 0x20000 and (s in starts or len(calls[s]) > 0):
        print(hex(s), 'saves lr' if s in starts else '        ', 'called from', [hex(c) + ' (fn ' + hex(fn(c)) + ')' for c in calls.get(s, [])][:8])
g = d[0x40000:]
sp = struct.unpack_from('<I', g, 1 * 16 + 12)[0]; print('sp', hex(sp), 'lr', hex(struct.unpack_from('<I', g, 12)[0]))
for i in range(10):
    nxt = struct.unpack_from('>I', d, sp)[0]; lr = struct.unpack_from('>I', d, sp + 16)[0]
    print(' frame', hex(sp), 'saved lr', hex(lr), 'in fn', hex(fn(lr)))
    if not nxt or nxt <= sp or nxt >= 0x40000: break
    sp = nxt

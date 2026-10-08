#!/usr/bin/env python3
"""lsdiff.py <capture with RPCS3_SPU_JOB_CAPTURE_LS>: replay the job, and at its first PUT compare the whole local
store with the real one. Prints the differing 16-byte slots in the order the replay wrote them, with the writing pc."""
import sys, struct; sys.path.insert(0, __file__.rsplit('/', 1)[0])
import gj, spuemu, spudis
j = gj.Job(sys.argv[1]); s = j.spu(); writer = {}
orig = spuemu.SPU.sq
def sq(self, a, v): writer[a & 0x3fff0] = (self.count, self.pc); orig(self, a, v)
spuemu.SPU.sq = sq
s.step()
while not s.out[0] and s.count < 5_000_000 and s.pc != 0x32b0 and s.phase == 0: s.step()
if not j.lsd or not s.out[0]: sys.exit('no PUT or no local store record: %s' % s.notes)
real = j.lsd[0][2]; mine = bytes(s.ls)
def close(x, y):
    # equal, or four floats within a relative 1e-3 of each other
    if x == y: return True
    fx = struct.unpack('>4f', x); fy = struct.unpack('>4f', y)
    return all(abs(p - q) <= 1e-3 * max(abs(p), abs(q), 1e-6) for p, q in zip(fx, fy))
bad = [a for a in range(0, 0x40000, 16) if not close(mine[a:a + 16], real[a:a + 16])]
print('instructions', s.count, 'differing slots', len(bad), 'of them written by the replay', sum(1 for a in bad if a in writer))
rows = sorted((writer[a][0], a) for a in bad if a in writer)
for n, a in rows[:int(sys.argv[2]) if len(sys.argv) > 2 else 24]:
    pc = writer[a][1]; w = struct.unpack_from('>I', s.ls, pc)[0]
    print('  at %7d  pc %05x  %-34s slot %05x  mine %s  real %s' % (n, pc, spudis.dis(w, pc), a, mine[a:a + 16].hex(), real[a:a + 16].hex()))
unw = [a for a in bad if a not in writer]
print('not written by the replay:', ' '.join('%x' % a for a in unw[:40]))

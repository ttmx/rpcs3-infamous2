"""Direct call of the game's raw-occlusion kernel in the interpreter (and with dataflow tracing)."""
import pickle, struct, sys, numpy as np
sys.path.insert(0, '../spu')
import spuemu
H = pickle.load(open('ao-kernel-entry.pkl', 'rb'))
ZWIN = 0x8280; NTILE = 0x14f80; OUT = 0x31180
def setup(cls, zwin, ntile):
    s = cls(bytearray(H['ls']), H['gpr'], H['pc'])
    s.ls[ZWIN:ZWIN + 63 * 416] = np.asarray(zwin, dtype='>f4').tobytes()
    s.ls[NTILE:NTILE + 90 * 640] = np.asarray(ntile, dtype=np.uint8).tobytes()
    return s
def run(zwin, ntile, cls=spuemu.SPU):
    s = setup(cls, zwin, ntile); ret = int(H['gpr'][0][0]) & 0x3fffc
    while s.pc != ret: s.step()
    return np.frombuffer(bytes(s.ls[OUT:OUT + 45 * 80]), dtype=np.uint8).reshape(45, 80).copy(), s
def tile_inputs(z, A, ty, tx):
    """z [360,640] float32, A [720,1280,4] uint8; tile origin (ty, tx) in half-res pixels."""
    zp = np.pad(z, ((9, 9), (12, 12)), mode='edge')
    return zp[ty:ty + 63, tx:tx + 104], A[2 * ty:2 * ty + 90, 2 * tx:2 * tx + 160]

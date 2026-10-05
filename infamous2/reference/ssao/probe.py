import numpy as np, struct, sys
import stage_emu
C = 0x37b08000; C1 = 0xb80; C2 = 0x37be9b80 - C
def run(stage, z=None, ao=None, d24=None, normals=None, verbose=False):
    """Run a stage on substituted buffers. z: [360,640] float, ao: [360,640] uint8, d24: [720,1280] int, normals: [720,1280,4] uint8."""
    def ov(mem):
        if z is not None: mem[C][C1:C1 + 921600] = np.asarray(z, dtype='>f4').tobytes()
        if ao is not None: mem[C][C2:C2 + 230400] = np.asarray(ao, dtype=np.uint8).tobytes()
        if d24 is not None:
            d = np.asarray(d24, dtype=np.uint32); b = np.zeros((720, 1280, 4), np.uint8)
            b[:, :, 0] = d >> 16; b[:, :, 1] = (d >> 8) & 255; b[:, :, 2] = d & 255
            mem[0x37784b80][:] = b.tobytes()
        if normals is not None: mem[0x37400b80][:] = np.asarray(normals, dtype=np.uint8).tobytes()
    mem, s, why = stage_emu.emulate(stage, verbose=verbose, override=ov)
    return dict(z=np.frombuffer(bytes(mem[C][C1:C1 + 921600]), dtype='>f4').reshape(360, 640).astype(np.float32),
                ao=np.frombuffer(bytes(mem[C][C2:C2 + 230400]), dtype=np.uint8).reshape(360, 640).copy(),
                full=np.frombuffer(bytes(mem[0xcf800000][:921600]), dtype=np.uint8).reshape(720, 1280).copy(), why=why, count=s.count)

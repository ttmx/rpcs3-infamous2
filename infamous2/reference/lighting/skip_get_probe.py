"""Offline check for skipping the stubbed lighting job's G-buffer GETs: with the tile function returning at
entry, everything the job does besides moving image rows must not depend on the image bytes. Runs the stubbed
job on the captured G-buffers and on random bytes and compares the transfer log, instruction count and all
non-image memory."""
import os,struct
import numpy as np
import ours,light_emu
os.chdir(ours.ROOT)
A,B,p,rs=ours.inputs()
rng=np.random.default_rng(7)
def run(tile,a,b):
    def before(mem):
        mem[light_emu.A_EA][:]=a;mem[light_emu.B_EA][:]=b
        mem[0xa93000][0xa00:0xa06]=struct.pack('>HHH',0,tile,tile)
        mem.setdefault(0x40300000,bytearray(0x1000))
    log=[]
    s,mem,stats=light_emu.make(50,before=before,log=log)
    s.ls[0x4ac8:0x4acc]=bytes.fromhex('35000000');s.cache.pop(0x4ac8,None)
    end=''
    try:
        while stats['puts']<4 and s.count<3_000_000:
            if s.pc==0x1760: s.pc=int(s.r[0][0])&0x3fffc
            s.step()
    except light_emu.Done as e: end=str(e)
    other={k:bytes(v) for k,v in mem.items() if k not in (light_emu.A_EA,light_emu.B_EA)}
    return log,s.count,s.pc,end,other
ok=True
for tile in (0,226,334,502,718,719):
    real=run(tile,A.tobytes(),B.tobytes())
    for name,fill in (('random',lambda:rng.integers(0,256,A.size,dtype=np.uint8).tobytes()),('zero',lambda:bytes(A.size)),('ff',lambda:b'\xff'*A.size)):
        other=run(tile,fill(),fill())
        same=[real[i]==other[i] for i in range(5)]
        ok&=all(same)
        print('tile',tile,name,'log',same[0],'insns',same[1],real[1],'pc',same[2],'end',same[3],'memory',same[4],flush=True)
print('PASS' if ok else 'FAIL')

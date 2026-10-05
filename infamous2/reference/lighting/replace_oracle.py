"""For tiles where the GPU result differs from what the game's JIT-compiled SPU
job produced, ask the SPU interpreter oracle which of the two it agrees with."""
from pathlib import Path
import os,sys,struct
import numpy as np
import ours,validate
os.chdir(ours.ROOT)
D=Path(sys.argv[1]);limit=int(sys.argv[3]) if len(sys.argv)>3 else 6
base=str(D/f'frame-{int(sys.argv[2]):03d}')
outA,outB,inA,inB=np.fromfile(base+'-gpu.bin','<u4').astype('>u4').view(np.uint8).reshape(4,720,1280,4)
spuA,spuB=[np.fromfile(base+f'-spu-{c}.bin',np.uint8).reshape(720,1280,4) for c in 'ab']
params=Path(base+'-params.bin').read_bytes();raw=Path(base+'-lights.bin').read_bytes();rs=[raw[i:i+48] for i in range(0,len(raw),48)]
err=np.abs(outA.astype(int)-spuA).max(axis=2).reshape(18,40,40,32).max(axis=(1,3)).ravel()
print('lights',len(rs),'kinds',sorted({struct.unpack_from('>I',r,20)[0] for r in rs}),'tiles over 4:',int((err>4).sum()))
for tile in np.argsort(-err)[:limit]:
    y,x=tile//40*40,tile%40*32;area=np.s_[y:y+40,x:x+32]
    o=validate.oracle(inA,inB,params,rs,int(tile),budget=40_000_000)
    c=lambda a,b:validate.compare(a[area],b[area])['max_error']
    print('tile',int(tile),'gpu-vs-game A/B',c(outA,spuA),c(outB,spuB),'| gpu-vs-interpreter',c(outA,o[0]),c(outB,o[1]),'| game-vs-interpreter',c(spuA,o[0]),c(spuB,o[1]),flush=True)

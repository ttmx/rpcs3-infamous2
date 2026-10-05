"""Inspect native tile-volume predicates against SPU call arguments."""
import json,os,struct,sys
from pathlib import Path
import numpy as np
import ours,validate
os.chdir(ours.ROOT)
base=Path(sys.argv[1]);cfg=json.loads(Path(str(base)+'.json').read_text());z=np.load(str(base)+'.npz');A,B,p,rs=ours.inputs();tile=cfg['tile'];y,x=tile//40*40,tile%40*32;area=np.s_[y:y+40,x:x+32];A[area]=z['inputA'];B[area]=z['inputB'];p=bytes.fromhex(cfg['params']);rs=[bytes.fromhex(r) for r in cfg['records']]
M=np.frombuffer(p[16:80],'>f4').astype('f4').reshape(4,4);pos=ours.positions(B[area],M,(x,y),(1280,720));lo,hi,v=ours.tile_bounds(pos,B[area]);planes=ours.tile_planes(p,(40,32),(x,y),(1280,720));print('native planes',planes[0,0]);print('native bounds',lo[0,0],hi[0,0])
def observe(s,m,st):
 if s.pc==0x4c60:
  sp=int(s.r[1][0]);print('SPU planes',np.frombuffer(s.ls[sp+32:sp+96],'>f4').reshape(4,4).T)
 if s.pc==0x4cc0:
  sp=int(s.r[1][0]);print('SPU sphere',np.frombuffer(s.ls[sp+96:sp+112],'>f4'));print('SPU bounds',np.frombuffer(s.ls[sp+112:sp+144],'>f4'))
 if s.pc in (0x4e88,0x5004,0x4f44,0x51d8):print('SPU call',hex(s.pc),'light',int(s.r[91][0]))
validate.oracle(A,B,p,rs,tile,observer=observe)
for i,r in enumerate(rs):
 lp=np.frombuffer(r[:12],'>f4').astype('f4');outer=np.frombuffer(r[34:36],'>f2').astype('f4')[0];nearest=np.clip(lp,lo,hi);print('native',i,'plane',ours.dot(planes,lp),'outer',outer,'boundsd2',ours.dot(lp-nearest,lp-nearest),'pixelmin',ours.dot(lp-pos,lp-pos).min())

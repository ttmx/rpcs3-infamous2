"""Replay selected live tiles through the original lighting SPU interpreter."""
from pathlib import Path
import json,os,struct,sys
import numpy as np
import validate
ROOT=Path(__file__).resolve().parent
os.chdir(ROOT)
fused='--fused' in sys.argv or '--jit-numerics' in sys.argv
if '--jit-numerics' in sys.argv:
 def jit_fi(s,q,pc):
  emu=validate.light_emu.spuemu;U=np.uint32
  a=emu.R(s,q['ra']);b=emu.R(s,q['rb'])
  base=(b&U(0x007ffc00))<<U(9);ymul=(b&U(0x3ff))*(a&U(0x7ffff));adjust=ymul>base
  value=(base-ymul)>>np.where(adjust,U(8),U(9));expo=(b&U(0xff800000))-np.where(adjust,U(1<<23),U(0))
  emu.setr(s,q['rt'],expo|(value&U(0x007fffff)))
 validate.light_emu.spuemu.OPS['FI']=jit_fi
validate.light_emu.spuemu.FUSED=fused
p=Path(sys.argv[1]).resolve()
results=[]
for frame,tile in [(13,5),(13,19),(13,59),(14,212),(15,212)]:
 base=p/f'frame-{frame:03d}'
 z=np.load(str(base)+'-arrays.npz');params=bytearray(Path(str(base)+'-params.bin').read_bytes())
 struct.pack_into('>I',params,0x94,0x37d6d690)
 lights=Path(str(base)+'-lights.bin').read_bytes();records=[lights[i:i+48] for i in range(0,len(lights),48)]
 target=validate.oracle(z['inputA'],z['inputB'],bytes(params),records,tile)
 y,x=tile//40*40,tile%40*32;area=np.s_[y:y+40,x:x+32]
 native=validate.ours.lighting(z['inputA'][area],z['inputB'][area],bytes(params),records,origin=(x,y),screen=(1280,720))
 result=dict(frame=frame,tile=tile,gpu_vs_interpreter={},native_vs_interpreter={},game_vs_interpreter={})
 for name,t in zip(('A','B'),target):
  result['native_vs_interpreter'][name]=validate.compare(native[0 if name=='A' else 1],t[area])
  result['gpu_vs_interpreter'][name]=validate.compare(z['gpu'+name][area],t[area])
  result['game_vs_interpreter'][name]=validate.compare(z['spu'+name][area],t[area])
 print(json.dumps(result),flush=True);results.append(result)
(p/('oracle-jit-numerics.json' if '--jit-numerics' in sys.argv else 'oracle-native-fused.json' if fused else 'oracle-native.json')).write_text(json.dumps(results,indent=2)+'\n')

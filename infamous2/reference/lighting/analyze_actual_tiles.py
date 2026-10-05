"""Replay completed SPU tile inputs, using the matching uploaded consumer pixels.

No emulator is launched. The SPU DMA probe is diagnostic-only; the shader uses
native lighting formulas. Parameter pointers are not arithmetic inputs.
"""
from pathlib import Path
import json,sys
import numpy as np
import ours
from validate import compare
D=Path(sys.argv[1]);size=1280*720*4
frames={j['pair']:j['sample'] for p in D.glob('frame-*-metadata.json') for j in [json.loads(p.read_text())]}
results=[]
for path in sorted(D.glob('pair-*-tile-*-actual.npz')):
 pair,tile=map(int,(path.name.split('-')[1],path.name.split('-')[3]));frame=frames[pair];base=str(path).removesuffix('.npz')
 z=np.load(path);params=Path(base+'-params.bin').read_bytes();lights=Path(base+'-lights.bin').read_bytes();records=[lights[i:i+48] for i in range(0,len(lights),48)]
 y,x=tile//40*40,tile%40*32
 native=ours.lighting(z['A'],z['B'],params,records,origin=(x,y),screen=(1280,720))
 raw=np.fromfile(D/f'frame-{frame:03d}-gpu.bin',np.uint8)
 uploaded=[raw[i*size:(i+1)*size].reshape(720,1280,4)[y:y+40,x:x+32][...,[3,2,1,0]].copy() for i in (4,5)]
 result=dict(pair=pair,frame=frame,tile=tile,light_count=len(records),native_vs_uploaded={c:compare(a,b) for c,a,b in zip('AB',native,uploaded)},active_equal=bool(native[0][...,0].any()==uploaded[0][...,0].any()))
 print(json.dumps(result),flush=True);results.append(result)
(D/'actual-tiles-comparison.json').write_text(json.dumps(results,indent=2)+'\n')

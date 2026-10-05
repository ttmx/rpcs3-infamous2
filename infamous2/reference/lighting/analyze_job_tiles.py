"""Compare native formulas with matching SPU GET/PUT tile data, offline.

Readback-pair IDs are observations, not a proof of guest frame identity. Match
same-thread job parameters and input/output transfers before drawing conclusions.
"""
from pathlib import Path
import json,struct,sys
import numpy as np
import ours
from validate import compare
D=Path(sys.argv[1]);size=1280*720*4;groups={};results=[]
paths=sorted([*D.glob('pair-*-get-*.bin'),*D.glob('pair-*-put-*.bin')],key=lambda p:int(p.stem.split('-')[-1]))
for path in paths:
 raw=path.read_bytes();get=raw[:8]==b'LIGHTGET';assert get or raw[:8]==b'LIGHTPUT'
 pc,cmd,tag,lst,lsa,sz,pair,tid=struct.unpack_from('<8I',raw,8);ls=raw[40:40+0x40000];items=raw[40+0x40000:]
 params=ls[0xa000:0xa100];count=struct.unpack_from('>I',params,0x90)[0]
 assert count<=64, 'Unsupported light count in diagnostic replay'
 records=ls[0xa100:0xa100+count*48]
 pos=lsa&0x3fff0
 touched=set()
 for off in range(0,sz,8):
  flags,n,ea=struct.unpack_from('>HHI',items,off);data=ls[pos+(ea&15):pos+(ea&15)+n];pos+=(n+15)&~15
  for c,base in enumerate((0x37400b80,0x37784b80)):
   if n!=128 or not base<=ea<base+size:continue
   y,x=divmod((ea-base)//4,1280);tile=y//40*40+x//32
   if tile not in (59,212,502):continue
   key=(tid,tile,params,records)
   if get:
    old=groups.get(key)
    if old is None or old['seen'][c]:
     # Each list owns all 40 rows of one channel; a repeated GET starts a new tile generation.
     if y%40==0:
      if c==0 or old is None:groups[key]=dict(input=[np.zeros((40,32,4),np.uint8) for _ in range(2)],output=[np.zeros((40,32,4),np.uint8) for _ in range(2)],seen=[set(),set()],written=[set(),set()],pair=pair)
   if key not in groups:continue
   g=groups[key];g['input' if get else 'output'][c][y%40]=np.frombuffer(data,np.uint8).reshape(32,4);g['seen' if get else 'written'][c].add(y%40);touched.add(key)
 for key in touched:
  g=groups[key]
  if not all(len(s)==40 for s in [*g['seen'],*g['written']]):continue
  tid,tile,params,records=key;y,x=tile//40*40,tile%40*32;rs=[records[i:i+48] for i in range(0,len(records),48)]
  native=ours.lighting(*g['input'],params,rs,origin=(x,y),screen=(1280,720))
  result=dict(input_pair=g['pair'],output_pair=pair,thread=tid,tile=tile,light_count=len(rs),native_vs_spu_put={c:compare(a,b) for c,a,b in zip('AB',native,g['output'])},active_equal=bool(native[0][...,0].any()==g['output'][0][...,0].any()))
  base=D/f'job-{len(results):03d}-tile-{tile}';np.savez_compressed(str(base)+'.npz',inputA=g['input'][0],inputB=g['input'][1],spuA=g['output'][0],spuB=g['output'][1]);Path(str(base)+'-params.bin').write_bytes(params);Path(str(base)+'-lights.bin').write_bytes(records)
  print(json.dumps(result),flush=True);results.append(result);del groups[key]
summary=dict(tiles=results,matched_tiles=len(results),unmatched_groups=len(groups),active_disagreements=sum(not r['active_equal'] for r in results),max_error={c:max((r['native_vs_spu_put'][c]['max_error'] for r in results),default=0) for c in 'AB'})
(D/'job-tiles-comparison.json').write_text(json.dumps(summary,indent=2)+'\n');print({k:v for k,v in summary.items() if k!='tiles'})

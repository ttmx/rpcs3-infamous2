"""Validate native hardware shaders on isolated, matched live SPU tile cases."""
from pathlib import Path
import json,sys
import numpy as np
import gpu_validate
from validate import compare
D=Path(sys.argv[1]);results=[]
for path in sorted(D.glob('job-*-tile-*.npz')):
 base=str(path).removesuffix('.npz');tile=int(path.stem.split('-')[-1]);y,x=tile//40*40,tile%40*32;area=np.s_[y:y+40,x:x+32]
 z=np.load(path);A=np.zeros((720,1280,4),np.uint8);B=np.full_like(A,255);A[area]=z['inputA'];B[area]=z['inputB']
 p=Path(base+'-params.bin').read_bytes();lights=Path(base+'-lights.bin').read_bytes();records=[lights[i:i+48] for i in range(0,len(lights),48)]
 output=gpu_validate.run(A,B,p,records)
 r=dict(case=path.stem,light_count=len(records),gpu_vs_spu_put={c:compare(a[area],z['spu'+c]) for c,a in zip('AB',output)},active_equal=bool(output[0][area][...,0].any()==z['spuA'][...,0].any()))
 results.append(r);print(json.dumps(r),flush=True)
summary=dict(cases=results,case_count=len(results),active_disagreements=sum(not r['active_equal'] for r in results),max_error={c:max((r['gpu_vs_spu_put'][c]['max_error'] for r in results),default=0) for c in 'AB'})
(D/'job-gpu-comparison.json').write_text(json.dumps(summary,indent=2)+'\n');print({k:v for k,v in summary.items() if k!='cases'})

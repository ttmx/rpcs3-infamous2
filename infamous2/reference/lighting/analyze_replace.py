"""Compare replacement-mode dumps (RPCS3_NATIVE_LIGHTING bit 2) with what the
SPU job left in guest memory for the same frame, and with the reference run
on the dumped inputs and job parameters."""
from pathlib import Path
import json,sys
import numpy as np
import ours
from validate import compare
D=Path(sys.argv[1]);reference=int(sys.argv[2]) if len(sys.argv)>2 else 4;results=[]
for path in sorted(D.glob('frame-*-gpu.bin')):
    base=str(path).removesuffix('-gpu.bin')
    outA,outB,inA,inB=np.fromfile(path,'<u4').astype('>u4').view(np.uint8).reshape(4,720,1280,4)
    spu=[np.fromfile(base+f'-spu-{c}.bin',np.uint8).reshape(720,1280,4) for c in 'ab']
    params=Path(base+'-params.bin').read_bytes();raw=Path(base+'-lights.bin').read_bytes()
    r=dict(frame=path.name[:9],lights=len(raw)//48,camera=params[16:80].hex()[:16],
           gpu_vs_spu={c:compare(o,s) for c,o,s in zip('AB',(outA,outB),spu)})
    tiles=lambda a:a[...,0].reshape(18,40,40,32).any(axis=(1,3))
    r['active_tile_disagreements']=int(np.count_nonzero(tiles(outA)!=tiles(spu[0])))
    r['tiles_over_4']=int(np.count_nonzero((np.abs(outA.astype(int)-spu[0]).max(axis=2)>4).reshape(18,40,40,32).any(axis=(1,3))))
    if len(results)%max(1,reference)==0:
        ref=ours.lighting(inA,inB,params,[raw[i:i+48] for i in range(0,len(raw),48)])
        r['gpu_vs_reference']={c:compare(o,n)['max_error'] for c,o,n in zip('AB',(outA,outB),ref)}
    results.append(r)
    print(r['frame'],'lights',r['lights'],'cam',r['camera'],'gpu-vs-spu max',*[m['max_error'] for m in r['gpu_vs_spu'].values()],'within1',*[round(m['pixels_within_1'],6) for m in r['gpu_vs_spu'].values()],'tile disagreements',r['active_tile_disagreements'],'tiles>4',r['tiles_over_4'],'ref',r.get('gpu_vs_reference'),flush=True)
summary=dict(frames=len(results),cameras=len({r['camera'] for r in results}),light_counts=sorted({r['lights'] for r in results}),
    max_error={c:max(r['gpu_vs_spu'][c]['max_error'] for r in results) for c in 'AB'},
    active_tile_disagreements=sum(r['active_tile_disagreements'] for r in results),tiles_over_4=sum(r['tiles_over_4'] for r in results),cases=results)
(D/'replace-comparison.json').write_text(json.dumps(summary,indent=2)+'\n');print({k:v for k,v in summary.items() if k!='cases'})

"""Compare aligned live GPU shadow dumps to SPU output and the NumPy oracle."""
import json
from pathlib import Path
import struct
import sys
import numpy as np
import ours
from validate import compare

N=1280*720
IMAGE_BYTES=N*4


def analyze(directory):
    directory=Path(directory).resolve()
    results=[]
    for path in sorted(directory.glob('frame-*-gpu.bin')):
        base=str(path).removesuffix('-gpu.bin')
        if not Path(base+'-spu-a.bin').exists(): continue
        raw=np.fromfile(path,np.uint8)
        assert len(raw) in (IMAGE_BYTES*4,IMAGE_BYTES*6), path
        gpu=[np.frombuffer(raw[i*IMAGE_BYTES:(i+1)*IMAGE_BYTES],'<u4').astype('>u4').view(np.uint8).reshape(720,1280,4) for i in range(2)]
        inputs=[raw[i*IMAGE_BYTES:(i+1)*IMAGE_BYTES].reshape(720,1280,4)[...,[3,2,1,0]].copy() for i in (2,3)]
        params=Path(base+'-params.bin').read_bytes()
        lights=Path(base+'-lights.bin').read_bytes()
        records=[lights[i:i+48] for i in range(0,len(lights),48)]
        count=struct.unpack_from('>I',params,0x90)[0]
        assert len(records)==count
        ref=ours.lighting(*inputs,params,records)
        spu=[np.fromfile(base+'-spu-'+name.lower()+'.bin',np.uint8).reshape(720,1280,4) for name in ('A','B')]
        uploaded=[raw[i*IMAGE_BYTES:(i+1)*IMAGE_BYTES].reshape(720,1280,4)[...,[3,2,1,0]].copy() for i in (4,5)] if len(raw)==IMAGE_BYTES*6 else None
        consume=Path(base+'-consume-params.bin').read_bytes()
        consume_lights=Path(base+'-consume-lights.bin')
        result=dict(lights_unchanged=consume_lights.read_bytes()==lights if consume_lights.exists() else None,frame=Path(base).name,light_count=count,parameters_unchanged=params==consume,
                    matrix_unchanged=params[16:80]==consume[16:80],
                    gpu_vs_reference={name:compare(g,r) for name,g,r in zip(('A','B'),gpu,ref)},
                    gpu_vs_spu={name:compare(g,r) for name,g,r in zip(('A','B'),gpu,spu)},
                    native_vs_spu={name:compare(g,r) for name,g,r in zip(('A','B'),ref,spu)})
        if uploaded is not None:
            result['native_vs_uploaded']={name:compare(g,r) for name,g,r in zip(('A','B'),ref,uploaded)}
            result['guest_vs_uploaded']={name:compare(g,r) for name,g,r in zip(('A','B'),spu,uploaded)}
        # Active-tile disagreements isolate culling/freshness from float rounding.
        ga=gpu[0][...,0].reshape(18,40,40,32).any((1,3))
        sa=spu[0][...,0].reshape(18,40,40,32).any((1,3))
        result['active_tile_disagreements']=np.argwhere(ga!=sa).tolist()
        ra=ref[0][...,0].reshape(18,40,40,32).any((1,3))
        result['native_active_tile_disagreements']=np.argwhere(ra!=sa).tolist()
        np.savez_compressed(base+'-arrays.npz',inputA=inputs[0],inputB=inputs[1],gpuA=gpu[0],gpuB=gpu[1],spuA=spu[0],spuB=spu[1])
        results.append(result)
        print(result['frame'],'params',result['parameters_unchanged'],'lights',result['lights_unchanged'],'GPU/ref max',*[v['max_error'] for v in result['gpu_vs_reference'].values()],
              'GPU/SPU <=1',*[round(v['pixels_within_1']*100,5) for v in result['gpu_vs_spu'].values()],
              'max',*[v['max_error'] for v in result['gpu_vs_spu'].values()], 'tiles',len(result['active_tile_disagreements']),'native max',*[v['max_error'] for v in result['native_vs_spu'].values()],'native tiles',len(result['native_active_tile_disagreements']),flush=True)
    (directory/'comparison-native-culling.json').write_text(json.dumps(results,indent=2)+'\n')
    return results


if __name__=='__main__': analyze(sys.argv[1])

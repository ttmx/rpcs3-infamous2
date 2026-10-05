"""Deterministic isolated lighting differential fuzzing; never launches RPCS3.

The original SPU interpreter is the test oracle. The implementation under test
is the native NumPy algorithm. Save all counterexamples for replay/minimization.
"""
import argparse,json,os,struct,time
from pathlib import Path
import numpy as np
import ours,validate
ROOT=Path(__file__).resolve().parent

def run(out,count,seed,gpu_every=0):
    os.chdir(ROOT);out=out.resolve();out.mkdir(parents=True,exist_ok=True)
    rng=np.random.default_rng(seed); A0,B0,params,records=ours.inputs()
    results=[];start=time.monotonic()
    # Real failed-camera inputs are part of the corpus, alongside mutations.
    live=Path(os.environ.get('LIGHT_CAPTURES',ROOT/'captures'))/'dock-shadow-04'
    z=np.load(live/'frame-013-arrays.npz');raw=(live/'frame-013-lights.bin').read_bytes()
    directed=[('real_spot_near',z['inputA'],z['inputB'],(live/'frame-013-params.bin').read_bytes(),[raw[j:j+48] for j in range(0,len(raw),48)],5)]
    for i in range(count):
        if i<len(directed):label,A,B,p,rs,tile=directed[i]
        else:
            label='mutated';A=A0.copy();B=B0.copy();p=params
            tile=int(rng.choice([226,212,334,5,260,380,420,520,610]))
            if i%3: tile=226
            y,x=tile//40*40,tile%40*32;area=np.s_[y:y+40,x:x+32]
            if i%5==0:A[area]=rng.integers(0,256,(40,32,4),dtype=np.uint8)
            elif i%5==1:A[area][...,0]=rng.choice(np.array([0,1,127,128,254,255],np.uint8),(40,32))
            if i%7==0:
                B[area][...,:3]=np.where(rng.random((40,32,1))<.08,255,B[area][...,:3])
            if i%13==0:
                depth=np.clip(((B[area][...,0].astype(np.int64)<<16)|(B[area][...,1].astype(np.int64)<<8)|B[area][...,2].astype(np.int64))+rng.integers(-512,513,(40,32)),0,0xffffff).astype(np.uint32)
                B[area][...,:3]=np.stack([depth>>16,(depth>>8)&255,depth&255],axis=2).astype(np.uint8)
            if i%11==0:B[area][...,3]=rng.integers(0,256,(40,32),dtype=np.uint8)
            rs=[records[int(rng.integers(len(records)))] for _ in range(int(rng.integers(0,9)))]
            # Directed point/spot mutations around a real illuminated tile.
            if i%3:
                tile=226;y,x=200,832;area=np.s_[y:y+40,x:x+32]
                rs=[]
                pos=ours.positions(B[area],np.frombuffer(p[16:80],'>f4').astype('f4').reshape(4,4),(x,y),(1280,720))
                for j in range(int(rng.integers(1,5))):
                    r=bytearray(records[5]);lp=np.frombuffer(r[:12],'>f4').astype('f4')
                    lp+=rng.uniform(-200,200,3).astype('f4');r[:12]=lp.astype('>f4').tobytes()
                    inner=float(rng.uniform(0,500));outer=inner+float(rng.uniform(1,1600));r[32:36]=np.array([inner,outer],'>f2').tobytes()
                    if i%3==2:
                        struct.pack_into('>I',r,20,2)
                        d=pos[int(rng.integers(40)),int(rng.integers(32))]-lp;d/=np.linalg.norm(d)
                        d+=rng.uniform(-.4,.4,3);d/=np.linalg.norm(d)
                        cosine=float(rng.uniform(.1,.95));r[36:40]=np.array([-1/(1-cosine),cosine],'>f2').tobytes()
                        r[40:46]=d.astype('>f2').tobytes();r[46:48]=np.array([rng.choice([0,10,40,500,1000])],'>f2').tobytes()
                    rs.append(bytes(r))
        y,x=tile//40*40,tile%40*32;area=np.s_[y:y+40,x:x+32]
        actual=ours.lighting(A[area],B[area],p,rs,origin=(x,y),screen=(1280,720))
        target=validate.oracle(A,B,p,rs,tile)
        metrics={n:validate.compare(a,t[area]) for n,a,t in zip(('A','B'),actual,target)}
        structural=int(np.count_nonzero((actual[0][...,0]==0)!=(target[0][area][...,0]==0)))
        passed=not structural and all(m['max_error']<=4 for m in metrics.values())
        gpu_metrics=None
        if gpu_every and i%gpu_every==0:
            import gpu_validate
            gpu=gpu_validate.run(A,B,p,rs)
            gpu_metrics={n:validate.compare(a[area],t[area]) for n,a,t in zip(('A','B'),gpu,target)}
            gpu_structural=int(np.count_nonzero((gpu[0][area][...,0]==0)!=(target[0][area][...,0]==0)))
            passed &= not gpu_structural and all(m['max_error']<=4 for m in gpu_metrics.values())
        result=dict(case=i,label=label,tile=tile,lights=len(rs),structural_pixels=structural,metrics=metrics,gpu_metrics=gpu_metrics,passed=bool(passed))
        if not passed:
            base=out/f'case-{i:04d}'
            np.savez_compressed(str(base)+'.npz',inputA=A[area],inputB=B[area],actualA=actual[0],actualB=actual[1],oracleA=target[0][area],oracleB=target[1][area])
            Path(str(base)+'.json').write_text(json.dumps(result|dict(params=p.hex(),records=[r.hex() for r in rs]),indent=2)+'\n')
        results.append(result)
        print(i,label,'tile',tile,'lights',len(rs),'max',*[m['max_error'] for m in metrics.values()],'structural',structural,'PASS' if passed else 'FAIL',flush=True)
        (out/'results.json').write_text(json.dumps(dict(seed=seed,cases=results,elapsed_seconds=time.monotonic()-start,passed=all(r['passed'] for r in results)),indent=2)+'\n')
    return results
if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('--out',type=Path,required=True);a.add_argument('--count',type=int,default=120);a.add_argument('--seed',type=int,default=1071);a.add_argument('--gpu-every',type=int,default=0);args=a.parse_args();run(args.out,args.count,args.seed,args.gpu_every)

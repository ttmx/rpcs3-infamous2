"""Isolated differential fuzzing on real captured frames; never launches RPCS3.

fuzz_isolated.py mutates one capture: two cameras, nine tiles, mostly <=8
lights. This takes whole live frames (real camera, real light table, real
G-buffers) and checks randomly chosen tiles anywhere on screen against the
original SPU interpreter, for both the NumPy reference and the hardware shader.
Readback-time frame pairing does not matter here: the oracle is fed exactly
the inputs given to the implementations under test.
"""
import argparse,json,os,subprocess,tempfile,time
from pathlib import Path
import numpy as np
import ours,validate,gpu_validate
ROOT=Path(__file__).resolve().parent
# LIGHT_CAPTURES: directory holding dock-shadow-*/frame-* captures of real frames
LIVE=Path(os.environ.get('LIGHT_CAPTURES',ROOT/'captures'))

def frames():
    """One frame per distinct (camera, light table), densest tables first."""
    seen={};
    for arrays in sorted(LIVE.glob('dock-shadow-*/frame-*-arrays.npz')):
        base=str(arrays).removesuffix('-arrays.npz');p=Path(base+'-params.bin');l=Path(base+'-lights.bin')
        if not p.exists() or not l.exists():continue
        params=p.read_bytes();raw=l.read_bytes()
        if len(params)!=256 or len(raw)%48 or len(raw)//48>gpu_validate.MAX_LIGHTS:continue
        seen.setdefault((params[16:144],raw),(arrays,params,raw))
    return sorted(seen.values(),key=lambda f:(-len(f[2]),str(f[0])))

def gpu_run(work,A,B,params,records):
    # gpu_validate.run uses fixed file names; keep a private directory so a
    # concurrent fuzz_isolated.py run cannot race on them.
    (work/'input.bin').write_bytes(gpu_validate.pack_input(A,B,params,records))
    subprocess.run([str(ROOT/'gpu/runner'),str(work/'input.bin'),str(ROOT/'gpu/tiles.spv'),str(ROOT/'gpu/lighting.spv'),str(work/'output.bin')],check=True,stdout=subprocess.DEVNULL)
    return np.fromfile(work/'output.bin','<u4').astype('>u4').view(np.uint8).reshape(2,*A.shape)

def choose(rng,active,count):
    """Lit tiles, their unlit neighbours (the culling boundary), screen edges."""
    grid=active.reshape(18,40);pad=np.pad(grid,1)
    near=(pad[:-2,1:-1]|pad[2:,1:-1]|pad[1:-1,:-2]|pad[1:-1,2:])&~grid
    edge=np.zeros_like(grid);edge[[0,-1]]=True;edge[:,[0,-1]]=True
    picks=[]
    for mask,share in ((grid,.5),(near,.25),(edge,.125),(np.ones_like(grid),.125)):
        pool=np.setdiff1d(np.flatnonzero(mask),picks)
        picks+=[int(t) for t in rng.choice(pool,min(len(pool),max(1,round(count*share))),replace=False)]
    return picks

def run(out,frame_count,tiles_per_frame,seed,budget):
    os.chdir(ROOT);out=out.resolve();out.mkdir(parents=True,exist_ok=True)
    rng=np.random.default_rng(seed);results=[];start=time.monotonic()
    corpus=frames();step=max(1,len(corpus)//frame_count)
    # Always keep the densest tables, then spread over the remaining cameras.
    chosen=(corpus[:frame_count//3]+corpus[frame_count//3::step])[:frame_count]
    with tempfile.TemporaryDirectory() as tmp:
        for arrays,params,raw in chosen:
            z=np.load(arrays);A,B=z['inputA'],z['inputB'];rs=[raw[i:i+48] for i in range(0,len(raw),48)]
            name=str(arrays.relative_to(LIVE)).removesuffix('-arrays.npz')
            native=ours.lighting(A,B,params,rs);gpu=gpu_run(Path(tmp),A,B,params,rs)
            whole={c:validate.compare(g,n) for c,g,n in zip('AB',gpu,native)}
            active=native[0][...,0].reshape(18,40,40,32).any(axis=(1,3)).ravel()
            print(name,'lights',len(rs),'active tiles',int(active.sum()),'gpu-vs-native max',*[m['max_error'] for m in whole.values()],flush=True)
            for tile in choose(rng,active,tiles_per_frame):
                y,x=tile//40*40,tile%40*32;area=np.s_[y:y+40,x:x+32]
                try:target=validate.oracle(A,B,params,rs,tile,budget=budget)
                except RuntimeError as e:
                    # Not a pass: the oracle gave no answer for this tile.
                    results.append(dict(frame=name,tile=tile,lights=len(rs),skipped=str(e)));print(' ',tile,'SKIP',e,flush=True);continue
                r=dict(frame=name,tile=tile,lights=len(rs),lit_pixels=int(np.count_nonzero(target[0][area][...,1:].any(axis=2))),oracle_active=bool(target[0][area][...,0].any()))
                for backend,actual in (('native',native),('gpu',gpu)):
                    r[backend]={c:validate.compare(a[area],t[area]) for c,a,t in zip('AB',actual,target)}
                    r[backend+'_structural']=int(np.count_nonzero((actual[0][area][...,0]==0)!=(target[0][area][...,0]==0)))
                r['passed']=not r['native_structural'] and not r['gpu_structural'] and all(m['max_error']<=4 for b in ('native','gpu') for m in r[b].values())
                if not r['passed']:
                    np.savez_compressed(out/f'case-{len(results):04d}.npz',inputA=A[area],inputB=B[area],nativeA=native[0][area],nativeB=native[1][area],gpuA=gpu[0][area],gpuB=gpu[1][area],oracleA=target[0][area],oracleB=target[1][area])
                results.append(r)
                print(' ',tile,'lit',r['lit_pixels'],'native',*[m['max_error'] for m in r['native'].values()],'gpu',*[m['max_error'] for m in r['gpu'].values()],'structural',r['native_structural'],r['gpu_structural'],'PASS' if r['passed'] else 'FAIL',flush=True)
            results.append(dict(frame=name,lights=len(rs),whole_frame_gpu_vs_native=whole,active_tiles=int(active.sum())))
            done=[r for r in results if 'passed' in r]
            (out/'results.json').write_text(json.dumps(dict(seed=seed,budget=budget,cases=results,tiles=len(done),skipped=sum('skipped' in r for r in results),elapsed_seconds=time.monotonic()-start,passed=all(r['passed'] for r in done)),indent=2)+'\n')
if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('--out',type=Path,required=True);a.add_argument('--frames',type=int,default=24);a.add_argument('--tiles',type=int,default=12);a.add_argument('--seed',type=int,default=4071);a.add_argument('--budget',type=int,default=40_000_000);args=a.parse_args();run(args.out,args.frames,args.tiles,args.seed,args.budget)

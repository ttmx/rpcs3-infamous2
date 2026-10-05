"""Exercise native/GPU light-mask word boundaries against isolated SPU jobs."""
import json,os
import numpy as np
import ours,validate,gpu_validate
os.chdir(ours.ROOT)
A,B,p,rs=ours.inputs();area=np.s_[200:240,832:864];pos=ours.positions(B[area],np.frombuffer(p[16:80],'>f4').astype('f4').reshape(4,4),(832,200),(1280,720))
spot=bytearray(rs[6]);spot[:12]=rs[5][:12];spot[32:36]=rs[5][32:36];d=pos[5,27]-np.frombuffer(spot[:12],'>f4').astype('f4');d/=np.linalg.norm(d);spot[40:46]=d.astype('>f2').tobytes()
results=[]
import sys
for n in ([int(v) for v in sys.argv[1:]] or (1,31,32,33,59,63,64,65,69,96,127,128,129,200,255,256)):
 for kind in ('point','mixed'):
  far=bytearray(rs[5]);far[:12]=np.array([1e7,1e7,1e7],'>f4').tobytes();far[32:36]=np.array([0,1],'>f2').tobytes()
  records=[bytes(far) for _ in range(n)]
  records[-1]=rs[5]
  if kind=='mixed':
   records[-1]=bytes(spot)
   if n>1: records[max(0,n//2-1)]=rs[5]
  oracle=validate.oracle(A,B,p,records,226)
  native=ours.lighting(A[area],B[area],p,records,origin=(832,200),screen=(1280,720))
  gpu=gpu_validate.run(A,B,p,records)
  metrics={backend:{c:validate.compare(a,t[area]) for c,a,t in zip(('A','B'),actual,oracle)} for backend,actual in [('native',native),('gpu',[g[area] for g in gpu])]}
  passed=all(m['max_error']<=4 for group in metrics.values() for m in group.values())
  result=dict(count=n,kind=kind,metrics=metrics,passed=passed);results.append(result);print(n,kind,metrics,flush=True)
  (ours.ROOT/'fuzz-light-counts.json').write_text(json.dumps(dict(cases=results,passed=all(r['passed'] for r in results)),indent=2)+'\n')

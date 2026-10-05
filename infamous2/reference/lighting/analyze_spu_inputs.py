"""Compare completed live SPU DMA GET bytes with the paired GPU inputs."""
from pathlib import Path
import json,struct,sys
import numpy as np
import ours
D=Path(sys.argv[1]);
import re
metadata=list(D.glob('frame-*-metadata.json'))
if metadata:
 index_by_pair={int(j['pair']):int(j['sample']) for p in metadata for j in [json.loads(p.read_text())]}
else:
 log=Path(json.loads((D/'provenance.json').read_text())['cache'])/'rpcs3/RPCS3.log'
 index_by_pair={int(pair):int(index) for index,pair in re.findall(r'Shadow frame (\d+) completed \(pair (\d+)\)',log.read_text())}
N=1280*720;SIZE=N*4;groups={};rows=[]
for path in sorted(D.glob('pair-*-get-*.bin')):
 raw=path.read_bytes();assert raw[:8]==b'LIGHTGET'
 pc,cmd,tag,lst,lsa,sz,pair,tid=struct.unpack_from('<8I',raw,8);ls=raw[40:40+0x40000];items=raw[40+0x40000:]
 if pair not in index_by_pair:continue
 frame=index_by_pair[pair]
 gpu_file=D/f'frame-{frame:03d}-gpu.bin'
 if not gpu_file.exists():continue
 gpu=np.fromfile(gpu_file,np.uint8)
 inp=[gpu[i*SIZE:(i+1)*SIZE].reshape(720,1280,4)[...,[3,2,1,0]].copy() for i in (2,3)]
 pos=lsa&0x3fff0
 params=ls[0xa000:0xa100];count=struct.unpack_from('>I',params,0x90)[0];records=ls[0xa100:0xa100+min(count,64)*48]
 changed=0;total=0
 for off in range(0,sz,8):
  flags,size,ea=struct.unpack_from('>HHI',items,off);data=ls[pos+(ea&15):pos+(ea&15)+size];pos+=(size+15)&~15
  for c,base in enumerate((0x37400b80,0x37784b80)):
   if size!=128 or not(base<=ea<base+SIZE):continue
   index=(ea-base)//4;y,x=divmod(index,1280);tile=y//40*40+x//32
   if tile not in (59,212):continue
   expected=inp[c][y,x:x+32].tobytes();changed+=sum(a!=b for a,b in zip(data,expected));total+=size
   key=(pair,tile);g=groups.setdefault(key,dict(A=np.zeros((40,32,4),np.uint8),B=np.zeros((40,32,4),np.uint8),seen=[set(),set()],params=params,records=records))
   g['AB'[c]][y%40]=np.frombuffer(data,np.uint8).reshape(32,4);g['seen'][c].add(y%40)
 rows.append(dict(file=path.name,pair=pair,changed_bytes=changed,compared_bytes=total,light_count=count,params_equal=params==(D/f'frame-{frame:03d}-params.bin').read_bytes(),lights_equal=records==(D/f'frame-{frame:03d}-lights.bin').read_bytes()))
for (pair,tile),g in groups.items():
 base=D/f'pair-{pair}-tile-{tile}-actual'
 np.savez_compressed(str(base)+'.npz',A=g['A'],B=g['B']);Path(str(base)+'-params.bin').write_bytes(g['params']);Path(str(base)+'-lights.bin').write_bytes(g['records'])
 print(pair,tile,'rows',*[len(s) for s in g['seen']],flush=True)
summary=dict(transfers=rows,changed_bytes=sum(r['changed_bytes'] for r in rows),compared_bytes=sum(r['compared_bytes'] for r in rows),params_changed=sum(not r['params_equal'] for r in rows),lights_changed=sum(not r['lights_equal'] for r in rows))
(D/'spu-input-comparison.json').write_text(json.dumps(summary,indent=2)+'\n');print({k:v for k,v in summary.items() if k!='transfers'})

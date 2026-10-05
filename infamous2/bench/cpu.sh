#!/bin/bash
# cpu.sh <seconds>: CPU seconds used per thread group of the running emulator over an interval
P=$(pgrep -x rpcs3); snap(){ for t in /proc/$P/task/*; do echo "$(cat $t/comm 2>/dev/null)|$(awk '{print $14+$15}' $t/stat 2>/dev/null)"; done; }
A=$(snap); sleep $1; B=$(snap)
python3 - "$1" <<PY
import sys,collections
a=collections.Counter(); b=collections.Counter()
for blob,c in ((r"""$A""",a),(r"""$B""",b)):
    for l in blob.splitlines():
        n,_,v=l.partition('|')
        if not v: continue
        g='SPURS workers' if n.startswith('SPU[0x') and n.rstrip().endswith(('000100','000100]')) or (n.startswith('SPU[0x') and '0001' in n[5:13]) else ('SPU other' if n.startswith('SPU[') else ('RSX' if n.startswith('rsx') else ('PPU' if n.startswith('PPU') else 'other')))
        c[g]+=float(v)
secs=float(sys.argv[1]); tot=0
out=[]
for g in ('SPURS workers','SPU other','RSX','PPU','other'):
    d=(b[g]-a[g])/100/secs; tot+=d; out.append(f"{g}={d:.2f}")
print('cores used: '+' '.join(out)+f" total={tot:.2f}")
PY

"""Offline check of the lighting stub: return from the tile function (LS 0x4ac8)
at entry and compare everything the job does besides pixels with the original."""
import os,struct,sys
import numpy as np
import ours,validate,light_emu
os.chdir(ours.ROOT)
A,B,p,rs=ours.inputs()
def run(tile,stub):
    def before(mem):
        mem[light_emu.A_EA][:]=A.tobytes();mem[light_emu.B_EA][:]=B.tobytes()
        mem[0xa93000][0xa00:0xa06]=struct.pack('>HHH',0,tile,tile)
        mem.setdefault(0x40300000,bytearray(0x1000))
    s,mem,stats=light_emu.make(50,before=before)
    if stub: s.ls[0x4ac8:0x4acc]=bytes.fromhex('35000000');s.cache.pop(0x4ac8,None)
    calls=0;log=[]
    try:
        while stats['puts']<4 and s.count<3_000_000:
            if s.pc==0x1760: s.pc=int(s.r[0][0])&0x3fffc
            if s.pc==0x4ac8: calls+=1
            s.step()
    except light_emu.Done as e: log.append(str(e))
    return s,mem,dict(stats),calls,log
for tile in (226,334,0,718,719):
    a=run(tile,False);b=run(tile,True)
    diff=[hex(k) for k in a[1] if k not in (light_emu.A_EA,light_emu.B_EA) and a[1][k]!=b[1][k]]
    y,x=tile//40*40,tile%40*32;area=np.s_[y:y+40,x:x+32]
    img=lambda m,ea:np.frombuffer(m[ea],np.uint8).reshape(720,1280,4)
    unchanged=[bool((img(b[1],ea)==src).all()) for ea,src in ((light_emu.A_EA,A),(light_emu.B_EA,B))]
    print('tile',tile,'orig: insns',a[0].count,'calls',a[3],a[2],a[4],'| stub: insns',b[0].count,'calls',b[3],b[2],b[4],'| non-image memory differs:',diff,'| stub leaves images as input:',unchanged,'| final pc',hex(a[0].pc),hex(b[0].pc))

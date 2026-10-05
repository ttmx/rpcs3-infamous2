import numpy as np
zh=np.load('f3-zhalf.npy'); aoh=np.load('f3-aohalf.npy').astype(np.float64); d24=np.load('f3-depth24.npy'); ao=np.load('f3-ao.npy').astype(np.float64)
zf=10.0/(1.0-d24/2**24+1e-12)
H,W=720,1280
yy,xx=np.mgrid[0:H,0:W]
i0=np.minimum(yy//2,359); j0=np.minimum(xx//2,639); i1=np.minimum(i0+1,359); j1=np.minimum(j0+1,639)
fy=(yy%2)*0.5; fx=(xx%2)*0.5
def up(weight_fn, offs=0):
    num=np.zeros((H,W)); den=np.zeros((H,W))
    for (ii,jj,wb) in ((i0,j0,(1-fy)*(1-fx)),(i0,j1,(1-fy)*fx),(i1,j0,fy*(1-fx)),(i1,j1,fy*fx)):
        w=wb*weight_fn(zh[ii,jj],zf) if weight_fn else wb
        num+=w*aoh[ii,jj]; den+=w
    out=np.where(den>1e-12,num/np.maximum(den,1e-12),aoh[i0,j0])
    return out
mask=d24<0xffffff
def err(img): 
    e=np.abs(np.round(img)-ao)[mask]; return e.mean(), (e<=1).mean()*100, (e<=8).mean()*100
print('nearest (top-left)      ',err(aoh[i0,j0]))
print('bilinear                ',err(up(None)))
for k in (0.02,0.05,0.1,0.2,0.5):
    print(f'bilateral 1/(eps+|dz|/z) k={k}',err(up(lambda a,b,k=k: 1.0/(k*1e-2+np.abs(a-b)/b))))
for k in (2,5,10,20,50,100):
    print(f'tent clamp(1-k*|dz|/z) k={k}',err(up(lambda a,b,k=k: np.clip(1-k*np.abs(a-b)/b,1e-4,1))))
print('target sky values:',np.unique(ao[~mask])[:5], ' half-res AO at sky:',np.unique(aoh[zh>1e5])[:5])

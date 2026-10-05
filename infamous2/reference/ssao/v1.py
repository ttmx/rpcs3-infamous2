import numpy as np
from PIL import Image, ImageDraw
zh_game=np.load('f3-zhalf.npy'); aoh=np.load('f3-aohalf.npy').astype(np.float64); d24=np.load('f3-depth24.npy'); target=np.load('f3-ao.npy')
# stage 1 (ours): half-res linear depth from the top-left pixel of each 2x2 block
zh=10.0/(1.0-d24[0::2,0::2]/2**24+1e-12)
zf=10.0/(1.0-d24/2**24+1e-12)
# stage 3 (ours): depth-aware 2x upsample of the half-res occlusion
H,W=720,1280; yy,xx=np.mgrid[0:H,0:W]
i0=np.minimum(yy//2,359); j0=np.minimum(xx//2,639); i1=np.minimum(i0+1,359); j1=np.minimum(j0+1,639); fy=(yy%2)*0.5; fx=(xx%2)*0.5
num=np.zeros((H,W)); den=np.zeros((H,W))
for ii,jj,wb in ((i0,j0,(1-fy)*(1-fx)),(i0,j1,(1-fy)*fx),(i1,j0,fy*(1-fx)),(i1,j1,fy*fx)):
    w=wb/(0.002+np.abs(zh[ii,jj]-zf)/zf); num+=w*aoh[ii,jj]; den+=w
img=np.round(num/den).astype(np.uint8)
e=np.abs(img.astype(int)-target.astype(int))
s=Image.new('RGB',(640*3,360+28),'black'); dr=ImageDraw.Draw(s)
for i,(t,im) in enumerate((('v1: our depth conversion + our upsample, game\'s middle stage',img),('SPU original (target)',target),(f'difference x4  mean error {e.mean():.1f}/255, {(e<=1).mean()*100:.0f}% within 1 level, {(e<=8).mean()*100:.0f}% within 8',np.clip(e*4,0,255).astype(np.uint8)))):
    s.paste(Image.fromarray(im).convert('RGB').resize((640,360)),(640*i,28)); dr.text((640*i+8,8),t,fill='white')
s.save('02-v1-two-of-three-stages.png'); print(e.mean(),(e<=1).mean(),(e<=8).mean())

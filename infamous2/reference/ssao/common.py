import numpy as np
from PIL import Image, ImageDraw
depth = np.load('depth24.npy'); target = np.load('target-ao.npy'); normals = np.load('normals.npy')
sky = depth == 0xffffff
def score(img):
    d = np.abs(img.astype(int) - target.astype(int))
    m = ~sky
    return dict(mean_abs=float(d[m].mean()), within8=float((d[m] <= 8).mean() * 100), sky_exact=float((img[sky] == target[sky]).mean() * 100))
def sheet(img, title, path):
    d = np.abs(img.astype(int) - target.astype(int)).clip(0, 255).astype(np.uint8)
    s = Image.new('RGB', (640 * 3, 360 + 28), 'black'); dr = ImageDraw.Draw(s)
    sc = score(img)
    for i, (t, im) in enumerate(((title, img), ('SPU original (target)', target), (f"difference x4  mean error {sc['mean_abs']:.1f}/255, {sc['within8']:.0f}% of pixels within 8", np.clip(d.astype(int) * 4, 0, 255).astype(np.uint8)))):
        s.paste(Image.fromarray(im).convert('RGB').resize((640, 360)), (640 * i, 28)); dr.text((640 * i + 8, 8), t, fill='white')
    s.save(path); return sc

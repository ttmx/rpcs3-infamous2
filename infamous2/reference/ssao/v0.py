import numpy as np
from common import *
# v0: a generic guess, no knowledge of the game's algorithm yet
z = 1.0 / (1.0 - depth.astype(np.float64) / 2**24 + 1e-7)          # linear depth up to scale
zh = z[::2, ::2]
occ = np.zeros_like(zh)
dirs = [(1,0),(-1,0),(0,1),(0,-1),(1,1),(-1,-1),(1,-1),(-1,1)]
for r in (3, 7):
    for dx, dy in dirs:
        s = np.roll(np.roll(zh, dy * r, 0), dx * r, 1)
        diff = (zh - s) / zh
        occ += np.clip(diff * 40, 0, 1) * (diff < 0.05)
ao = np.clip(1 - occ / (len(dirs) * 2) * 2.2, 0, 1)
k = np.ones(5) / 5
for ax in (0, 1): ao = np.apply_along_axis(lambda v: np.convolve(v, k, 'same'), ax, ao)
full = np.repeat(np.repeat(ao, 2, 0), 2, 1)
img = (full * 255).astype(np.uint8); img[sky] = 100
print(sheet(img, 'v0: generic screen-space AO guess', '01-v0-generic-guess.png'))

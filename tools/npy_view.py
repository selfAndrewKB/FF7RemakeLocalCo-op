"""Render .npy textures (from rdc_fetch.py) to small PNGs: npy_view.py <out.png> <a.npy> [b.npy ...] [--gain G]
Each image is tonemapped x/(1+x) (or x*gain) and tiled in a grid (2 columns), each scaled to 960 wide."""
import sys
import numpy as np
from PIL import Image, ImageDraw

args = sys.argv[1:]
gain = None
if "--gain" in args:
    i = args.index("--gain"); gain = float(args[i + 1]); del args[i:i + 2]
out, files = args[0], args[1:]
tiles = []
for f in files:
    a = np.load(f)[..., :3]
    a = np.clip(a * gain, 0, 1) if gain else a / (1 + np.abs(a))
    im = Image.fromarray((np.clip(a, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)).resize((960, int(960 * a.shape[0] / a.shape[1])))
    ImageDraw.Draw(im).text((4, 4), f.split("\\")[-1], fill=(255, 0, 255))
    tiles.append(im)
cols = 2 if len(tiles) > 1 else 1
rows = (len(tiles) + cols - 1) // cols
h = max(t.height for t in tiles)
sheet = Image.new("RGB", (960 * cols, h * rows))
for i, t in enumerate(tiles):
    sheet.paste(t, ((i % cols) * 960, (i // cols) * h))
sheet.save(out)

# app.ico from ic_fluent_image_24_filled.svg: MuPDF gives the anti-aliased shape per size (it ignores SVG gradients),
# the gradient is applied here as the SVG defines it (objectBoundingBox, x2=100% rotated 45 deg, pad, sRGB lerp).
# usage: python make_icon.py   (needs pymupdf, numpy, pillow)
import io, os, re, struct
import fitz
import numpy as np
from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))
svg = open(os.path.join(here, 'ic_fluent_image_24_filled.svg'), 'rb').read()
stops = [tuple(int(c[i:i + 2], 16) / 255.0 for i in (1, 3, 5)) for c in re.findall(rb'stop-color="(#[0-9a-fA-F]{6})"', svg) for c in [c.decode()]]
c0, c1 = np.array(stops[0]), np.array(stops[-1])
bbox = (3.0, 3.0, 21.0, 21.0)   # path bounds in the 24-unit viewBox
page = fitz.open(stream=svg, filetype='svg')[0]

pngs = []
for size in (16, 20, 24, 32, 40, 48, 64, 96, 128, 256):
    pix = page.get_pixmap(matrix=fitz.Matrix(size / 24.0, size / 24.0), alpha=True)
    a = np.frombuffer(pix.samples, np.uint8).reshape(pix.height, pix.width, 4)[:, :, 3].astype(np.float32) / 255.0
    px = (np.arange(size) + 0.5) * 24.0 / size
    u = (px[None, :] - bbox[0]) / (bbox[2] - bbox[0])
    v = (px[:, None] - bbox[1]) / (bbox[3] - bbox[1])
    t = np.clip((u + v) * np.cos(np.pi / 4), 0.0, 1.0)[..., None]
    rgb = c0 * (1.0 - t) + c1 * t
    img = np.dstack([np.round(rgb * 255.0), np.round(a * 255.0)]).astype(np.uint8)
    buf = io.BytesIO()
    Image.fromarray(img, 'RGBA').save(buf, 'PNG', optimize=True)
    pngs.append((size, buf.getvalue()))

with open(os.path.join(here, 'app.ico'), 'wb') as f:
    f.write(struct.pack('<HHH', 0, 1, len(pngs)))
    offset = 6 + 16 * len(pngs)
    for size, data in pngs:
        f.write(struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
        offset += len(data)
    for _, data in pngs:
        f.write(data)
print('app.ico:', ', '.join(str(s) for s, _ in pngs))

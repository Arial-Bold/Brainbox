#!/usr/bin/env python3
"""Makes dist/opening.bnr: the 96x32 banner your Launcher shows for BrainBox
(Professor Scotty's head in the beam of light on the dark stage, and the
app's name), with the name and description that go with it.

    python3 tools/make_banner.py

Needs Python 3 with Pillow and NumPy, and tools/banner_head.png (a picture
of his head with a see-through background, drawn by the PC copy of
source/pixel3d.c)."""
import os, struct
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FONT = os.path.join(HERE, 'fonts', 'MPLUSRounded1c-ExtraBold-latin.ttf')

SHORT_NAME = 'BrainBox'
SHORT_MAKER = 'Arial-Bold'
LONG_NAME = 'BrainBox'
LONG_MAKER = 'Arial-Bold'
DESCRIPTION = 'Ask Professor Scotty anything: a hologram professor who answers out loud, with Groq, Gemini, Claude or ChatGPT.'

S = 8                       # drawn 8 times bigger, then shrunk (smooth edges)
W, H = 96 * S, 32 * S

def banner_image():
    img = Image.new('RGBA', (W, H), (0, 0, 0, 255))
    px = np.zeros((H, W, 4), np.float32)
    # the stage: dark at the top, a deep blue floor
    y = np.linspace(0, 1, H)[:, None]
    top, bot = np.array([7, 7, 20]), np.array([20, 16, 52])
    px[..., :3] = top + (bot - top) * y[..., None]
    px[..., 3] = 255
    img = Image.fromarray(px.astype(np.uint8), 'RGBA')
    d = ImageDraw.Draw(img)
    horizon = int(H * 0.80)
    cx = int(W * 0.20)
    # the floor grid, in perspective
    floor = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    fd = ImageDraw.Draw(floor)
    for k in range(-14, 15):
        fd.line([(cx + k * 3.0 * S, horizon), (cx + k * 12.0 * S, H)], fill=(70, 70, 150, 95), width=S // 2)
    for k in range(3):
        yy = horizon + (H - horizon) * (0.18 + 0.32 * k)
        fd.line([(0, yy), (W, yy)], fill=(70, 70, 150, 70), width=S // 2)
    mask = Image.new('L', (W, H), 0)
    ImageDraw.Draw(mask).rectangle([0, horizon, W, H], fill=255)
    img.paste(floor, (0, 0), Image.composite(floor, Image.new('RGBA', (W, H)), mask).split()[3])
    # the beam: a soft cone of blue light up from the floor
    beam = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    bd = ImageDraw.Draw(beam)
    bd.polygon([(cx - 11 * S, horizon), (cx + 11 * S, horizon), (cx + 7 * S, 0), (cx - 7 * S, 0)], fill=(70, 120, 255, 70))
    beam = beam.filter(ImageFilter.GaussianBlur(3 * S))
    img = Image.alpha_composite(img, beam)
    # the pedestal ring
    ring = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(ring).ellipse([cx - 12 * S, horizon - 1.6 * S, cx + 12 * S, horizon + 1.6 * S], outline=(190, 225, 255, 255), width=S // 2)
    glow = ring.filter(ImageFilter.GaussianBlur(S))
    img = Image.alpha_composite(Image.alpha_composite(img, glow), ring)
    # his head, floating in the beam, with a little glow round it
    head = Image.open(os.path.join(HERE, 'banner_head.png')).convert('RGBA')
    hh = int(H * 0.86)
    head = head.resize((round(head.width * hh / head.height), hh), Image.LANCZOS)
    hx, hy = cx - head.width // 2, int(H * 0.03)
    halo = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    a = head.split()[3]
    halo.paste(Image.new('RGBA', head.size, (120, 180, 255, 120)), (hx, hy), a)
    halo = halo.filter(ImageFilter.GaussianBlur(2 * S))
    img = Image.alpha_composite(img, halo)
    img.alpha_composite(head, (hx, hy))
    # hologram scanlines over him
    lines = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    ld = ImageDraw.Draw(lines)
    for yy in range(0, H, S * 2):
        ld.rectangle([0, yy, W, yy + S * 0.7], fill=(6, 10, 40, 45))
    lmask = Image.new('L', (W, H), 0)
    lmask.paste(a, (hx, hy))
    img.paste(lines, (0, 0), Image.composite(lines, Image.new('RGBA', (W, H)), lmask).split()[3])
    # the name: BRAINBOX big, and who's in it small underneath, in the apps' colours
    x0 = int(W * 0.40)
    def word(text, size, colour, y, spacing=0):
        f = ImageFont.truetype(FONT, size)
        t = Image.new('RGBA', (W, H), (0, 0, 0, 0))
        td = ImageDraw.Draw(t)
        x = x0
        for ch in text:   # (letter by letter, for the spacing)
            td.text((x + S * 0.5, y + S * 0.6), ch, font=f, fill=(0, 0, 0, 170))
            td.text((x, y), ch, font=f, fill=colour)
            x += f.getlength(ch) + spacing
        return t, x
    t2, xe = word('BRAINBOX', int(11.2 * S), (236, 235, 255, 255), int(3.0 * S), spacing=int(0.1 * S))
    t1, _ = word('PROF. SCOTTY', int(5.6 * S), (140, 220, 255, 255), int(17.5 * S), spacing=int(0.7 * S))
    img = Image.alpha_composite(img, t1)
    # (a soft blue glow behind the big word)
    g2 = t2.filter(ImageFilter.GaussianBlur(1.5 * S))
    img = Image.alpha_composite(img, Image.fromarray((np.array(g2) * [0.45, 0.7, 1.0, 0.9]).astype(np.uint8), 'RGBA'))
    img = Image.alpha_composite(img, t2)
    return img.resize((96, 32), Image.LANCZOS)

def rgb5a3(img):
    """the GameCube's RGB5A3 texture format, in 4x4 tiles"""
    a = np.array(img.convert('RGBA'))
    out = bytearray()
    for ty in range(0, 32, 4):
        for tx in range(0, 96, 4):
            for y in range(ty, ty + 4):
                for x in range(tx, tx + 4):
                    r, g, b, al = (int(v) for v in a[y, x])
                    if al >= 224:
                        v = 0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)
                    else:
                        v = (al >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4)
                    out += struct.pack('>H', v)
    return bytes(out)

def field(s, n):
    b = s.encode('ascii')[:n - 1]
    return b + b'\0' * (n - len(b))

def main():
    img = banner_image()
    data = b'BNR1' + b'\0' * 0x1C + rgb5a3(img)
    data += field(SHORT_NAME, 0x20) + field(SHORT_MAKER, 0x20) + field(LONG_NAME, 0x40) + field(LONG_MAKER, 0x40) + field(DESCRIPTION, 0x80)
    assert len(data) == 0x1960
    os.makedirs(os.path.join(ROOT, 'dist'), exist_ok=True)
    open(os.path.join(ROOT, 'dist', 'opening.bnr'), 'wb').write(data)
    img.save(os.path.join(HERE, 'preview_banner.png'))
    print('wrote dist/opening.bnr')

if __name__ == '__main__':
    main()

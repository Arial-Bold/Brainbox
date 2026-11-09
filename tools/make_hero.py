#!/usr/bin/env python3
"""Makes hero.png: the 640x160 picture for BrainBox's page in an app store,
the same picture as the banner (make_banner.py), drawn wider and larger.

    python3 tools/make_hero.py
"""
import os, re
HERE = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(HERE, 'make_banner.py')).read()
# the banner's drawing, at 4:1 instead of 3:1, and finished at 640x160
src = src.replace("S = 8 ", "S = 10 ").replace("W, H = 96 * S, 32 * S", "W, H = 128 * S, 32 * S")
src = src.replace("img.resize((96, 32), Image.LANCZOS)", "img.resize((640, 160), Image.LANCZOS)")
src = src.replace("cx = int(W * 0.20)", "cx = int(W * 0.18)").replace("x0 = int(W * 0.40)", "x0 = int(W * 0.36)")
src = src.split("def rgb5a3")[0]   # (just the picture)
ns = {'__file__': os.path.join(HERE, 'make_banner.py')}
exec(src, ns)
img = ns['banner_image']()
os.makedirs(os.path.join(os.path.dirname(HERE), 'dist'), exist_ok=True)
img.convert('RGB').save(os.path.join(os.path.dirname(HERE), 'dist', 'hero.png'))
print('wrote dist/hero.png')

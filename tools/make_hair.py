"""Her hair from a hair model made of many strands: the strands are filled
into a grid of small cubes, softened (blurred) into smooth locks, and the
surface found again (marching cubes): one smooth, closed shape in BrainBox's
soft style, light enough for the GameCube -> hair_bun_mesh.h"""
import sys, numpy as np
from scipy.ndimage import gaussian_filter, binary_closing, binary_fill_holes
from skimage.measure import marching_cubes
d = sys.argv[1]; out = sys.argv[2]
VOX = float(sys.argv[3]) if len(sys.argv) > 3 else 0.008   # cube size (model units)
SOFT = float(sys.argv[4]) if len(sys.argv) > 4 else 2.0    # softening, in cubes
STEP = int(sys.argv[5]) if len(sys.argv) > 5 else 2        # surface detail (bigger: fewer triangles)
LEVEL = float(sys.argv[6]) if len(sys.argv) > 6 else 0.35
S = float(sys.argv[7]) if len(sys.argv) > 7 else 1.36
C0 = np.array([0, 1.0, 0.0]); C1 = np.array([0, float(sys.argv[8]) if len(sys.argv) > 8 else 0.55, 0.01])
V = np.load(d + '/V.npy'); F = np.load(d + '/F.npy')
# points over the strands' surfaces (corners, middles and edge midpoints)
a, b, c = V[F[:, 0]], V[F[:, 1]], V[F[:, 2]]
pts = np.concatenate([V, (a + b + c) / 3, (a + b) / 2, (b + c) / 2, (c + a) / 2])
lo = V.min(0) - 6 * VOX; hi = V.max(0) + 6 * VOX
dims = np.ceil((hi - lo) / VOX).astype(int) + 1
g = np.zeros(dims, bool)
ix = np.floor((pts - lo) / VOX).astype(int)
g[ix[:, 0], ix[:, 1], ix[:, 2]] = True
# each strand filled in (its inside, not just its skin)
g = binary_closing(g, iterations=1)
for ax in range(3):   # (fill each slice's enclosed holes, along each direction)
    g = g | np.stack([binary_fill_holes(np.take(g, k, axis=ax)) for k in range(g.shape[ax])], axis=ax)
f = gaussian_filter(g.astype(np.float32), SOFT)
verts, faces, normals, _ = marching_cubes(f, LEVEL, step_size=STEP)
P = verts * VOX + lo
# into BrainBox's space: turned round (its front, +z, to the camera), scaled, centred on her head
Q = (P - C0) * S
Q = np.stack([-Q[:, 0], Q[:, 1], -Q[:, 2]], 1) + C1
# normals: from the softened field (pointing out of the hair)
N = normals.copy()
N = np.stack([-N[:, 0], N[:, 1], -N[:, 2]], 1)
N /= np.linalg.norm(N, axis=1)[:, None] + 1e-12
T = faces[:, ::-1].copy()   # (winding to match)
# (check: out from the middle of her head, for most of it)
print('normals out:', (np.einsum('ij,ij->i', N, Q - C1) > 0).mean().round(3))
with open(out, 'w') as o:
    o.write('/* hair_bun_mesh.h - her hair, from a hair model, softened (made by tools/make_hair.py) */\n')
    o.write('#define BUN_NV %d\n#define BUN_NT %d\n#define BUN_CAP_NV BUN_NV\n' % (len(Q), len(T)))
    o.write('static const float bun_mesh_v[BUN_NV][6] = {\n')
    for p, n in zip(Q, N): o.write(' {%.4ff,%.4ff,%.4ff,%.3ff,%.3ff,%.3ff},\n' % (p[0], p[1], p[2], n[0], n[1], n[2]))
    o.write('};\nstatic const unsigned short bun_mesh_t[BUN_NT][3] = {\n')
    for t in T: o.write(' {%d,%d,%d},\n' % tuple(t))
    o.write('};\n')
print(dims, len(Q), 'verts', len(T), 'tris; y', Q[:, 1].min().round(3), Q[:, 1].max().round(3), 'x', Q[:, 0].min().round(3), Q[:, 0].max().round(3), 'z', Q[:, 2].min().round(3), Q[:, 2].max().round(3))

/* pixel3d.c - see pixel3d.h.
 *
 * Professor Pixel is built each frame from simple smooth shapes (stretched
 * spheres and tubes) in his pose of the moment, lit on the CPU a vertex at a
 * time, the way the mock-ups were: a soft light ramp from a warm key light,
 * a cool fill, a blue glow around his edges, a shine on the glossy parts
 * (eyes, nose, spectacles, bow tie), and glowing props. Then he's projected
 * into the 640x480 layout and drawn as triangles with the depth buffer on. */
#include "pixel3d.h"
#include <math.h>
#include <string.h>

#define MAXV 11000
#define MAXT 15000

typedef struct { float x, y, z; } V3;
enum { SKIN, GLOSS, EMIS, MATTE, DARK };

static V3 vp[MAXV], vn[MAXV];
static float vr[MAXV], vg[MAXV], vb[MAXV];
static unsigned char vmat[MAXV], vgrp[MAXV];   /* the material; the group (how it moves, see place()) */
static unsigned short tri[MAXT][3];
static int nverts, ntris;

static inline V3 v3(float x, float y, float z) { V3 v = { x, y, z }; return v; }
static inline V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 mul(V3 a, float k) { return v3(a.x * k, a.y * k, a.z * k); }
static inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline V3 norm(V3 a) { float l = sqrtf(dot(a, a)); return l > 1e-9f ? mul(a, 1 / l) : a; }

typedef struct { float m[3][3]; } M3;
static M3 rotx(float a) { float c = cosf(a), s = sinf(a); M3 r = { { { 1, 0, 0 }, { 0, c, -s }, { 0, s, c } } }; return r; }
static M3 rotz(float a) { float c = cosf(a), s = sinf(a); M3 r = { { { c, -s, 0 }, { s, c, 0 }, { 0, 0, 1 } } }; return r; }
static V3 apply(const M3 *m, V3 v) {
    return v3(m->m[0][0] * v.x + m->m[0][1] * v.y + m->m[0][2] * v.z, m->m[1][0] * v.x + m->m[1][1] * v.y + m->m[1][2] * v.z,
              m->m[2][0] * v.x + m->m[2][1] * v.y + m->m[2][2] * v.z);
}

/* ------------------------------------------------------------------ shapes */

static float cur_r, cur_g, cur_b;
static int cur_mat, cur_grp;
enum { G_HEAD, G_GLOBE, G_PENCIL, G_FIXED };
static void colour(float r, float g, float b, int mat) { cur_r = r; cur_g = g; cur_b = b; cur_mat = mat; }

static int vert(V3 p, V3 n) {
    if (nverts >= MAXV) return -1;
    vp[nverts] = p; vn[nverts] = n;
    vr[nverts] = cur_r; vg[nverts] = cur_g; vb[nverts] = cur_b; vmat[nverts] = (unsigned char)cur_mat; vgrp[nverts] = (unsigned char)cur_grp;
    return nverts++;
}

/* the grid of a shape (rows x cols points, the columns wrapping round) */
static void grid(int base, int cols, int rows) {
    for (int j = 0; j < rows - 1; j++)
        for (int i = 0; i < cols; i++) {
            if (ntris + 2 > MAXT) return;
            int i2 = (i + 1) % cols;
            int a = base + j * cols + i, b = base + j * cols + i2, c = base + (j + 1) * cols + i2, d = base + (j + 1) * cols + i;
            tri[ntris][0] = a; tri[ntris][1] = b; tri[ntris][2] = c; ntris++;
            tri[ntris][0] = a; tri[ntris][1] = c; tri[ntris][2] = d; ntris++;
        }
}

/* a stretched sphere: centre c, radii r, turned by rot (NULL: none) */
/* sines and cosines round a circle of n steps, worked out once */
static const float *circle(int n, bool sine) {
    static float tab[65][2][65];
    static bool done[65];
    if (n < 1 || n > 64) n = 64;
    if (!done[n]) {
        for (int i = 0; i <= n; i++) { tab[n][0][i] = cosf(6.2831853f * i / n); tab[n][1][i] = sinf(6.2831853f * i / n); }
        done[n] = true;
    }
    return tab[n][sine];
}

static void ellipsoid(V3 c, V3 r, int nu, int nvv, const M3 *rot) {
    if (nverts + (nvv + 1) * nu > MAXV) return;
    int base = nverts;
    const float *cs = circle(nu, false), *sn = circle(nu, true);
    const float *hc = circle(2 * nvv, false), *hs = circle(2 * nvv, true);   /* half circles: 0..pi */
    for (int j = 0; j <= nvv; j++) {
        float st = hs[j], ct = hc[j];
        for (int i = 0; i < nu; i++) {
            V3 u = v3(st * cs[i], ct, st * sn[i]);
            V3 p = v3(u.x * r.x, u.y * r.y, u.z * r.z), n = norm(v3(u.x / r.x, u.y / r.y, u.z / r.z));
            if (rot) { p = apply(rot, p); n = apply(rot, n); }
            vert(add(p, c), n);
        }
    }
    grid(base, nu, nvv + 1);
}
static void ell(float cx, float cy, float cz, float rx_, float ry_, float rz_, int nu, int nvv, const M3 *rot) {
    ellipsoid(v3(cx, cy, cz), v3(rx_, ry_, rz_), nu, nvv, rot);
}

/* a tube of radius r along the points of a path */
static void tube(const V3 *path, int n, float r, int seg) {
    if (n < 2 || nverts + n * seg > MAXV) return;
    int base = nverts;
    V3 up = v3(0, 0, -1);
    for (int i = 0; i < n; i++) {
        V3 t = norm(sub(path[i + 1 < n ? i + 1 : i], path[i > 0 ? i - 1 : i]));
        V3 b = cross(t, up);
        if (sqrtf(dot(b, b)) < 1e-3f) b = cross(t, v3(0, 1, 0));
        b = norm(b);
        V3 nn = cross(b, t);
        const float *cs = circle(seg, false), *sn = circle(seg, true);
        for (int k = 0; k < seg; k++) {
            V3 d = add(mul(b, cs[k]), mul(nn, sn[k]));
            vert(add(path[i], mul(d, r)), d);
        }
    }
    int t0 = ntris;
    grid(base, seg, n);
    for (int k = t0; k < ntris; k++) { unsigned short x = tri[k][1]; tri[k][1] = tri[k][2]; tri[k][2] = x; }   /* (wound the other way to the spheres: turned round so the outside faces out) */
}

/* a point on the front of the head (for the lines drawn on it) */
#define HCX 0.0f
#define HCY 0.40f
#define HRX 0.56f
#define HRY 0.74f
#define HRZ 0.60f
static V3 surf(float x, float y, float out) {
    float k = 1 - (x / HRX) * (x / HRX) - ((y - HCY) / HRY) * ((y - HCY) / HRY);
    return v3(x, y, -HRZ * sqrtf(k > 0 ? k : 0) - out);
}

/* ------------------------------------------------------------ the professor */

/* The parts that never change shape (most of him) are made and lit once
 * (S); the moving ones (eyes, lids, brows, mouth, spectacles, bow tie, the
 * globe's rings, the scribble) every frame (D). How the whole head tilts and
 * bobs, and the props float, is applied when he's drawn (place()). */
static int ns, nst;   /* the static part: vertices and triangles */
static void build(const PixelPose *ps, bool S) {
    bool D = !S;
    if (S) nverts = ntris = 0; else { nverts = ns; ntris = nst; }
    cur_grp = G_HEAD;
    float t = ps->t, talk = ps->talk, blink = ps->blink, th = ps->think;
    const float SK[3] = { 0.90f, 0.73f, 0.64f }, HAIR[3] = { 0.80f, 0.80f, 0.84f }, BROW[3] = { 0.86f, 0.86f, 0.90f };
    M3 r;
    if (S) {
    colour(SK[0], SK[1], SK[2], SKIN);
    ell(HCX, HCY, 0, HRX, HRY, HRZ, 32, 22, NULL);              /* the head: a tall egg */
    ell(0, 0.02f, -0.06f, 0.46f, 0.36f, 0.50f, 24, 14, NULL);   /* a fuller jaw */
    }
    for (int sx = -1; sx <= 1; sx += 2) {
        if (S) {
        colour(0.92f, 0.69f, 0.61f, SKIN); ell(0.29f * sx, 0.22f, -0.43f, 0.12f, 0.09f, 0.08f, 12, 8, NULL);   /* cheeks */
        colour(0.92f, 0.66f, 0.58f, SKIN); ell(0.56f * sx, 0.40f, 0.02f, 0.08f, 0.16f, 0.07f, 12, 8, NULL);    /* ears */
        /* eyes, with heavy upper lids (a wise, amused squint) */
        colour(0.98f, 0.98f, 1.0f, GLOSS); ell(0.20f * sx, 0.52f, -0.50f, 0.125f, 0.11f, 0.085f, 14, 10, NULL);
        colour(0.90f, 0.66f, 0.58f, SKIN); ell(0.20f * sx, 0.45f, -0.51f, 0.12f, 0.03f, 0.08f, 12, 6, NULL);   /* lower lids */
        /* tufts of grey hair round the sides and back */
        static const float tufts[4][7] = { { 0.54f, 0.64f, 0.10f, 0.17f, 0.21f, 0.28f, 0.35f }, { 0.57f, 0.42f, 0.16f, 0.14f, 0.22f, 0.26f, 0.05f },
                                           { 0.45f, 0.84f, 0.20f, 0.19f, 0.14f, 0.26f, 0.65f }, { 0.32f, 0.70f, 0.42f, 0.22f, 0.24f, 0.20f, 0.2f } };
        colour(HAIR[0], HAIR[1], HAIR[2], MATTE);
        for (int k = 0; k < 4; k++) {
            r = rotz(tufts[k][6] * sx);
            ell(tufts[k][0] * sx, tufts[k][1], tufts[k][2], tufts[k][3], tufts[k][4], tufts[k][5], 12, 8, &r);
        }
        continue;
        }
        colour(0.20f, 0.30f, 0.48f, GLOSS);
        ell(0.19f * sx + 0.01f + 0.025f * th + ps->look_x * 0.03f, 0.51f + 0.03f * th, -0.58f, 0.058f, 0.064f, 0.02f, 10, 8, NULL);
        float lid = 0.06f + 0.06f * blink;
        r = rotz(-0.18f * sx);
        colour(SK[0], SK[1], SK[2], SKIN); ell(0.20f * sx, 0.57f - 0.04f * blink, -0.515f, 0.13f, lid, 0.092f, 14, 8, &r);
        /* big expressive eyebrows */
        float lift = 0.03f * talk + (sx > 0 ? 0.07f : -0.015f) * th + ps->brows * 0.04f;
        colour(BROW[0], BROW[1], BROW[2], MATTE);
        r = rotz(0.22f * sx); ell(0.22f * sx, 0.68f + lift, -0.52f, 0.15f, 0.045f, 0.06f, 12, 6, &r);
        r = rotz(0.5f * sx); ell(0.33f * sx, 0.705f + lift, -0.47f, 0.07f, 0.04f, 0.05f, 10, 6, &r);
    }
    if (S) {
    colour(HAIR[0], HAIR[1], HAIR[2], MATTE); ell(0, 0.62f, 0.48f, 0.30f, 0.24f, 0.12f, 14, 8, NULL);   /* the back of the head */
    /* forehead lines */
    colour(0.80f, 0.56f, 0.48f, MATTE);
    for (int k = 0; k < 2; k++) {
        V3 p[9];
        for (int i = 0; i < 9; i++) {
            float x = -0.2f + k * 0.03f + (0.4f - k * 0.06f) * i / 8;
            p[i] = surf(x, (k ? 0.95f : 0.88f) + 0.02f * cosf(x * 6), 0.01f);
        }
        tube(p, 9, 0.009f, 4);
    }
    /* a long nose */
    colour(SK[0], SK[1], SK[2], SKIN); r = rotx(0.3f); ell(0, 0.36f, -0.62f, 0.075f, 0.17f, 0.10f, 12, 10, &r);
    colour(0.96f, 0.64f, 0.56f, GLOSS); ell(0, 0.24f, -0.70f, 0.085f, 0.075f, 0.08f, 12, 10, NULL);
    colour(0.94f, 0.66f, 0.58f, SKIN);
    for (int sx = -1; sx <= 1; sx += 2) ell(0.065f * sx, 0.22f, -0.66f, 0.045f, 0.04f, 0.04f, 8, 6, NULL);
    colour(HAIR[0], HAIR[1], HAIR[2], MATTE);
    ell(0, -0.13f, -0.42f, 0.12f, 0.13f, 0.09f, 12, 8, NULL);   /* a neat grey goatee */
    ell(0, -0.04f, -0.47f, 0.05f, 0.03f, 0.03f, 8, 6, NULL);
    /* a short neck, a white collar and the bow tie's knot: where he ends */
    colour(SK[0], SK[1], SK[2], SKIN); ell(0, -0.27f, 0.02f, 0.20f, 0.13f, 0.20f, 14, 8, NULL);
    colour(0.94f, 0.95f, 1.0f, MATTE);
    for (int sx = -1; sx <= 1; sx += 2) { r = rotz(-0.5f * sx); ell(0.10f * sx, -0.36f, -0.22f, 0.10f, 0.05f, 0.08f, 10, 6, &r); }
    colour(0.72f, 0.06f, 0.10f, GLOSS); ell(0, -0.42f, -0.35f, 0.055f, 0.055f, 0.045f, 10, 8, NULL);
    }
    if (D) {
    /* the mouth: a lopsided, knowing smile, or open as he talks */
    if (talk > 0.05f) {
        colour(0.38f, 0.10f, 0.14f, DARK); ell(0, 0.07f, -0.55f, 0.12f, 0.025f + 0.05f * talk, 0.03f, 14, 8, NULL);
    } else {
        V3 p[11];
        int n = th < 0.5f ? 11 : 7;
        for (int i = 0; i < n; i++) {
            float x = th < 0.5f ? -0.15f + 0.32f * i / 10 : 0.14f * i / 6;
            float y = 0.08f + 0.9f * (x - 0.01f) * (x - 0.01f) + 0.12f * x;
            V3 s = surf(x, y, 0);
            if (s.z > -0.53f) s.z = -0.53f;
            p[i] = s;
        }
        colour(0.55f, 0.22f, 0.24f, MATTE); tube(p, n, 0.016f, 6);
    }
    /* spectacles: up on his forehead, or down on his nose when he thinks */
    {
        bool down = th >= 0.5f;
        M3 R = rotx(down ? 0.08f : 0.95f);
        V3 at = down ? v3(0, 0.50f, -0.66f) : v3(0, 0.92f, -0.42f);
        float gw = down ? 0.13f : 0.12f, gh = down ? 0.09f : 0.07f, gx = down ? 0.20f : 0.19f;
        colour(0.95f, 0.78f, 0.30f, GLOSS);
        for (int sx = -1; sx <= 1; sx += 2) {
            V3 p[29];
            for (int i = 0; i < 29; i++) {
                float a = 6.2831853f * i / 28, cx = cosf(a) * 1.6f, cy = sinf(a) * 1.6f;
                cx = cx < -1 ? -1 : cx > 1 ? 1 : cx;
                cy = cy < -1 ? -1 : cy > 1 ? 1 : cy;
                p[i] = add(apply(&R, v3(gw * cx + gx * sx, gh * cy, 0)), at);
            }
            tube(p, 29, 0.017f, 6);
        }
        V3 b[3] = { add(apply(&R, v3(-0.07f, 0.02f, 0)), at), add(apply(&R, v3(0, 0.035f, 0)), at), add(apply(&R, v3(0.07f, 0.02f, 0)), at) };
        tube(b, 3, 0.015f, 6);
    }
    /* the red bow tie (it bounces) */
    {
        M3 T = rotz(0.15f * sinf(t * 2) + 0.25f * ps->chuckle);
        colour(0.86f, 0.10f, 0.14f, GLOSS);
        for (int sx = -1; sx <= 1; sx += 2) {
            int base = nverts;
            M3 r2 = rotz(0.25f * sx);
            ellipsoid(v3(0, 0, 0), v3(0.14f, 0.09f, 0.06f), 14, 8, &r2);
            for (int i = base; i < nverts; i++) {
                vp[i] = add(apply(&T, add(vp[i], v3(0.14f * sx, 0, 0))), v3(0, -0.42f, -0.30f));
                vn[i] = apply(&T, vn[i]);
            }
        }
    }
    }
    /* floating props: a little glowing globe, and a pencil that scribbles
     * (always made, drawn only when wanted: see pixel3d_draw) */
    {
        V3 gc0 = v3(-1.08f, 0.18f, -0.30f), gc = v3(-1.08f, 0.18f + 0.04f * sinf(t * 1.4f), -0.30f);
        if (S) {
        cur_grp = G_GLOBE;
        colour(0.16f, 0.36f, 0.80f, GLOSS); ellipsoid(gc0, v3(0.22f, 0.22f, 0.22f), 20, 14, NULL);
        colour(0.40f, 0.85f, 0.55f, GLOSS);
        ellipsoid(add(gc0, v3(-0.06f, 0.07f, -0.19f)), v3(0.09f, 0.06f, 0.05f), 10, 6, NULL);
        ellipsoid(add(gc0, v3(0.07f, -0.06f, -0.19f)), v3(0.07f, 0.08f, 0.05f), 10, 6, NULL);
        colour(0.55f, 0.95f, 1.0f, EMIS);
        for (int k = -1; k <= 1; k++) {
            V3 p[25];
            float y = 0.11f * k, rr = sqrtf(0.225f * 0.225f - y * y);
            for (int i = 0; i < 25; i++) { float a = 6.2831853f * i / 24; p[i] = add(gc0, v3(rr * cosf(a), y, rr * sinf(a))); }
            tube(p, 25, 0.006f, 4);
        }
        V3 pc = v3(1.06f, 0.10f, -0.35f), d = v3(sinf(-0.6f), -cosf(-0.6f), 0);
        cur_grp = G_PENCIL;
        V3 body[6];
        for (int i = 0; i < 6; i++) body[i] = add(pc, mul(d, -0.26f + 0.44f * i / 5));
        colour(0.98f, 0.80f, 0.25f, GLOSS); tube(body, 6, 0.04f, 6);
        M3 rp = rotz(-0.6f);
        colour(0.92f, 0.78f, 0.60f, SKIN); ellipsoid(add(pc, mul(d, 0.23f)), v3(0.035f, 0.06f, 0.035f), 8, 6, &rp);
        colour(0.2f, 0.2f, 0.25f, MATTE); ellipsoid(add(pc, mul(d, 0.28f)), v3(0.015f, 0.025f, 0.015f), 6, 4, &rp);
        colour(0.98f, 0.55f, 0.60f, MATTE); ellipsoid(add(pc, mul(d, -0.29f)), v3(0.042f, 0.05f, 0.042f), 8, 6, &rp);
        }
        if (D) {
        cur_grp = G_FIXED;
        colour(0.55f, 0.95f, 1.0f, EMIS);
        for (int k = 0; k < 3; k++) {
            V3 p[25];
            float ang = t * 0.8f + k * 3.14159265f / 3;
            for (int i = 0; i < 25; i++) {
                float a = 6.2831853f * i / 24;
                p[i] = add(gc, v3(0.225f * cosf(a) * cosf(ang), 0.225f * sinf(a), 0.225f * cosf(a) * sinf(ang)));
            }
            tube(p, 25, 0.007f, 4);
        }
        V3 pc = v3(1.06f, 0.10f + 0.05f * sinf(t * 3), -0.35f), d = v3(sinf(-0.6f), -cosf(-0.6f), 0);
        V3 tip = add(pc, mul(d, 0.31f)), sc[30];
        for (int i = 0; i < 30; i++) {
            float x = i / 29.0f;
            sc[i] = v3(tip.x - 0.32f + 0.32f * x, tip.y - 0.02f + 0.04f * sinf(x * 18 + t * 4 * ps->scribble), tip.z);
        }
        colour(0.55f, 0.95f, 1.0f, EMIS); tube(sc, 30, 0.008f, 4);
        }
    }
    if (S) { ns = nverts; nst = ntris; }
}

/* how each group moves this frame */
static M3 head_rot;
static float head_bob, globe_bob, pencil_bob, frame_t;
static bool show_props;
static inline V3 place(int i) {
    V3 p = vp[i];
    switch (vgrp[i]) {
    case G_HEAD: {   /* the head floats: a little bigger, tilting as he thinks */
        V3 c0 = v3(0, 0.2f, 0);
        return add(add(mul(apply(&head_rot, sub(p, c0)), 1.16f), c0), v3(0, -0.02f + head_bob, 0));
    }
    case G_GLOBE: return add(p, v3(0, globe_bob, 0));
    case G_PENCIL: return add(p, v3(0, pencil_bob, 0));
    default: return p;
    }
}

/* --------------------------------------------------------------- lighting */

static float ramp(float d) {   /* soft light, with a gentle shoulder */
    static const float xs[4] = { 0, 0.15f, 0.45f, 1 }, ys[4] = { 0.30f, 0.42f, 0.80f, 1.0f };
    if (d <= 0) return ys[0];
    for (int i = 0; i < 3; i++)
        if (d <= xs[i + 1]) return ys[i] + (ys[i + 1] - ys[i]) * (d - xs[i]) / (xs[i + 1] - xs[i]);
    return 1;
}

static void light(float *r, float *g, float *b, int i) {
    static V3 key, fill, half;
    static bool once;
    if (!once) {
        once = true;
        key = norm(v3(-0.5f, 0.7f, -0.6f));
        fill = norm(v3(0.7f, -0.1f, -0.5f));
        half = norm(add(key, v3(0, 0, -1)));
    }
    V3 n = norm(vn[i]);
    float d = dot(n, key), f = dot(n, fill), facing = -n.z;
    if (d < 0) d = 0;
    if (f < 0) f = 0;
    if (facing < 0) facing = 0;
    float e = 1 - facing, rim = e * e * (0.6f * e + 0.4f);   /* about (1 - facing)^2.4 */
    float dx = n.x - half.x, dy = n.y - half.y, q = (dx * dx + dy * dy) / 0.012f;
    float spec = q < 6 ? expf(-q) : 0;
    float cr = vr[i], cg = vg[i], cb = vb[i];
    int m = vmat[i];
    float R, Gc, B;
    if (m == EMIS) {
        R = cr * 1.15f + rim * 0.4f; Gc = cg * 1.15f + rim * 0.4f; B = cb * 1.15f + rim * 0.4f;
    } else {
        float k = ramp(d) * 0.74f + 0.12f;
        R = cr * k * 0.95f + cr * f * 0.25f;
        Gc = cg * k * 0.97f + cg * f * 0.25f;
        B = cb * k * 1.05f + cb * f * 0.45f;
        float rk = m == DARK ? 0 : 0.55f;
        R += 0.45f * rim * rk; Gc += 0.85f * rim * rk; B += 1.0f * rim * rk;
        float sk = m == GLOSS ? 0.8f : m == SKIN ? 0.08f : 0.04f;
        R += spec * sk; Gc += spec * sk; B += spec * sk;
    }
    *r = R; *g = Gc; *b = B;
}

/* ------------------------------------------------------------------ drawing */

static float sx_[MAXV], sy_[MAXV], sz_[MAXV];
static unsigned char cr_[MAXV], cg_[MAXV], cb_[MAXV];

#ifndef CULL_SIGN
#define CULL_SIGN 1.0f
#endif
static unsigned char to8(float v) { v *= 255; return v <= 0 ? 0 : v >= 255 ? 255 : (unsigned char)v; }

void pixel3d_build(const PixelPose *ps) {
    static bool made;
    if (!made) {   /* the parts that never change: made and lit once */
        build(ps, true);
        for (int i = 0; i < ns; i++) {
            float r, g, b;
            light(&r, &g, &b, i);
            cr_[i] = to8(r); cg_[i] = to8(g); cb_[i] = to8(b);
        }
        made = true;
    }
    build(ps, false);
    float t = ps->t, th = ps->think;
    head_rot = rotz(0.07f * th + 0.02f * sinf(t) + 0.03f * ps->chuckle * sinf(t * 18));
    head_bob = 0.02f * sinf(t * 1.1f);
    globe_bob = 0.04f * sinf(t * 1.4f);
    pencil_bob = 0.05f * sinf(t * 3);
    show_props = ps->props;
    frame_t = t;
    for (int i = ns; i < nverts; i++) {
        float r, g, b;
        light(&r, &g, &b, i);
        cr_[i] = to8(r); cg_[i] = to8(g); cb_[i] = to8(b);
    }
}

void pixel3d_draw(float x, float y, float scale, float alpha, float mirror_y, PixelBox *box) {
    const float CAMY = 0.15f, CAMZ = -4.9f, FOV = 560 * scale;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < nverts; i++) {
        V3 p = place(i);
        if (mirror_y > -100) p.y = 2 * mirror_y - p.y;   /* the floor reflection */
        float zc = p.z - CAMZ;
        if (zc < 0.5f) zc = 0.5f;
        sx_[i] = x + FOV * p.x / zc;
        sy_[i] = y - FOV * (p.y - CAMY) / zc;
        sz_[i] = -(zc - 2.0f) / 6.0f;   /* nearer: closer to 0 */
        if (sx_[i] < x0) x0 = sx_[i];
        if (sx_[i] > x1) x1 = sx_[i];
        if (sy_[i] < y0) y0 = sy_[i];
        if (sy_[i] > y1) y1 = sy_[i];
    }
    if (box) { box->x0 = x0; box->y0 = y0; box->x1 = x1; box->y1 = y1; }
    (void)frame_t;
    unsigned char a = to8(alpha);
    /* the triangles to draw (the props only when wanted), in batches (a batch
     * can hold 65535 vertices) */
    static unsigned short list[MAXT];
    int nl = 0;
    /* triangles facing away are left out (every shape is closed, so they're
     * hidden anyway): half the work for the GPU and the CPU */
    float facing = mirror_y > -100 ? -CULL_SIGN : CULL_SIGN;
    for (int k = 0; k < ntris; k++) {
        if (!show_props && vgrp[tri[k][0]] != G_HEAD) continue;
        int i0 = tri[k][0], i1 = tri[k][1], i2 = tri[k][2];
        float area = (sx_[i1] - sx_[i0]) * (sy_[i2] - sy_[i0]) - (sy_[i1] - sy_[i0]) * (sx_[i2] - sx_[i0]);
        if (area * facing < 0) continue;
        list[nl++] = (unsigned short)k;
    }
    for (int start = 0; start < nl; start += 20000) {
        int n = nl - start < 20000 ? nl - start : 20000;
        GX_Begin(GX_TRIANGLES, GX_VTXFMT1, (u16)(n * 3));
        for (int j = start; j < start + n; j++)
            for (int c = 0; c < 3; c++) {
                int k = list[j];
                int i = tri[k][c];
                GX_Position3f32(sx_[i], sy_[i], sz_[i]);
                GX_Color4u8(cr_[i], cg_[i], cb_[i], a);
                GX_TexCoord2f32(0, 0);
            }
        GX_End();
    }
}

int pixel3d_triangles(void) { return ntris; }

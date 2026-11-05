/* head3d.c - see head3d.h.
 *
 * Professor Scotty is built each frame from simple smooth shapes (stretched
 * spheres and tubes) in his pose of the moment, lit on the CPU a vertex at a
 * time, the way the mock-ups were: a soft light ramp from a warm key light,
 * a cool fill, a blue glow around his edges, a shine on the glossy parts
 * (eyes, nose, spectacles, bow tie), and glowing props. Then he's projected
 * into the 640x480 layout and drawn as triangles with the depth buffer on. */
#include "head3d.h"
#include "earth_map.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

#define MAXV 24000
#define MAXT 40000

typedef struct { float x, y, z; } V3;
enum { SKIN, GLOSS, EMIS, MATTE, DARK };

static V3 vp[MAXV], vn[MAXV];
static float vr[MAXV], vg[MAXV], vb[MAXV], vsk[MAXV];   /* colour; how shiny (-1: as its material) */
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

#define GLOBE_C v3(-1.08f, 0.18f, -0.30f)
static int globe_v0, globe_v1;   /* the globe's vertices (lit again each frame as it turns) */

static float cur_r, cur_g, cur_b;
static int cur_mat, cur_grp;
enum { G_HEAD, G_GLOBE, G_PENCIL, G_FIXED };
static void colour(float r, float g, float b, int mat) { cur_r = r; cur_g = g; cur_b = b; cur_mat = mat; }

static int vert(V3 p, V3 n) {
    if (nverts >= MAXV) return -1;
    vp[nverts] = p; vn[nverts] = n;
    vr[nverts] = cur_r; vg[nverts] = cur_g; vb[nverts] = cur_b; vsk[nverts] = -1; vmat[nverts] = (unsigned char)cur_mat; vgrp[nverts] = (unsigned char)cur_grp;
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

/* ------------------------------------------------------------- the sculpt
 *
 * His head, face, ears, neck and hair are one smooth surface, not separate
 * balls: each part is a stretched sphere, and they're melted together
 * (a "smooth union" of their distance fields), so the cheeks, nose, ears and
 * jaw flow into one another the way a sculpted head does, and the hair sits
 * in soft clumps. The surface is found once, at the start, by sending a ray
 * out from inside the head in every direction (more of them towards his
 * face) and finding where each one leaves him. */

/* a tube whose radius changes along it (rad: one for each point) */
static void tube_r(const V3 *path, int n, const float *rad, int seg) {
    if (n < 2 || nverts + n * seg + 2 > MAXV) return;
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
            V3 off = add(mul(b, cs[k] * rad[i]), mul(nn, sn[k] * rad[i] * 0.7f));   /* (a little flatter front to back) */
            vert(add(path[i], off), d);
        }
    }
    int t0 = ntris;
    grid(base, seg, n);
    for (int k = t0; k < ntris; k++) { unsigned short x = tri[k][1]; tri[k][1] = tri[k][2]; tri[k][2] = x; }
    /* closed at the ends */
    for (int e = 0; e < 2; e++) {
        int i = e ? n - 1 : 0;
        V3 t = norm(sub(path[e ? n - 1 : 1], path[e ? n - 2 : 0]));
        int c = vert(add(path[i], mul(t, rad[i] * 0.6f * (e ? 1 : -1))), mul(t, e ? 1.0f : -1.0f));
        if (c < 0) return;
        for (int k = 0; k < seg && ntris < MAXT; k++) {
            int a = base + i * seg + k, b = base + i * seg + (k + 1) % seg;
            if (e) { tri[ntris][0] = a; tri[ntris][1] = c; tri[ntris][2] = b; }
            else { tri[ntris][0] = a; tri[ntris][1] = b; tri[ntris][2] = c; }
            ntris++;
        }
    }
}

/* A surface made from a formula f(u, v) (u along it, 0..1; v round it, 0..1,
 * wrapping), its normals worked out from the formula; turned by rot and
 * moved to at. The normals are made to point outwards (away from the middle). */
typedef void (*SurfFn)(float u, float v, const void *ctx, V3 *out);
static void param_surface(int nu, int nv, SurfFn f, const void *ctx, const M3 *rot, V3 at) {
    if (nverts + (nu + 1) * nv > MAXV) return;
    int base = nverts;
    V3 mid = v3(0, 0, 0);
    for (int j = 0; j <= nu; j++)
        for (int i = 0; i < nv; i++) { V3 p; f((float)j / nu, (float)i / nv, ctx, &p); mid = add(mid, p); }
    mid = mul(mid, 1.0f / ((nu + 1) * nv));
    const float e = 0.002f;
    for (int j = 0; j <= nu; j++) {
        float u = (float)j / nu;
        for (int i = 0; i < nv; i++) {
            float v = (float)i / nv;
            V3 p, pu0, pu1, pv0, pv1;
            f(u, v, ctx, &p);
            f(u > e ? u - e : u, v, ctx, &pu0); f(u < 1 - e ? u + e : u, v, ctx, &pu1);
            f(u, v - e, ctx, &pv0); f(u, v + e, ctx, &pv1);
            V3 n = norm(cross(sub(pu1, pu0), sub(pv1, pv0)));
            if (dot(n, sub(p, mid)) < 0) n = mul(n, -1);
            if (rot) { p = apply(rot, p); n = apply(rot, n); }
            vert(add(p, at), n);
        }
    }
    grid(base, nv, nu + 1);
}

/* a bow tie wing: from the knot (u 0) out to its end (u 1) */
typedef struct { int sx; const M3 *tilt; } BowWing;
static void bow_wing(float u, float v, const void *ctx, V3 *out) {
    const BowWing *w = ctx;
    float a = 6.2831853f * v, s = sinf(a), c = cosf(a);
    float flare = 1 - (1 - u) * (1 - u) * (1 - 0.4f * u);
    float h = 0.036f + 0.084f * flare;              /* half height: pinched at the knot, wide at the end */
    float d = 0.026f + 0.032f * sinf(3.1415927f * fminf(u * 1.1f, 1));   /* puffy in the middle */
    /* the folds gathered into the knot, fading out along the wing */
    float fold = 0.010f * sinf(a * 5) * (1 - u) * (1 - u);
    /* the end: the top and bottom corners reach further out than the middle (a notch) */
    float len = 0.225f - 0.038f * powf(1 - s * s, 1.5f);
    /* the end thins to an edge (front to back), so its outline keeps the notch */
    float thin = u > 0.80f ? sqrtf(fmaxf(0, 1 - ((u - 0.80f) / 0.20f) * ((u - 0.80f) / 0.20f))) : 1;
    float round_ = u > 0.80f ? 0.92f + 0.08f * thin : 1;   /* (corners just softened) */
    float x = 0.035f + len * u;
    V3 p = v3(x, (h + fold) * s * round_, (d + fold * 0.6f) * c * thin);
    p = apply(w->tilt, v3(p.x * w->sx, p.y, p.z));
    *out = p;
}

/* the knot: a short wrapped band, a little squarish, with creases running round it */
static void bow_knot(float u, float v, const void *ctx, V3 *out) {
    (void)ctx;
    float a = 6.2831853f * v, c = cosf(a), s = sinf(a);
    float y = (u - 0.5f) * 0.11f;
    float end = sqrtf(fmaxf(0, 1 - (2 * u - 1) * (2 * u - 1)));   /* rounded top and bottom */
    float sq = powf(fabsf(c), 0.7f) * (c < 0 ? -1 : 1), sz = powf(fabsf(s), 0.7f) * (s < 0 ? -1 : 1);   /* squarish */
    float crease = 1 + 0.06f * sinf(a * 8) * (s < 0 ? 1 : 0.3f);
    float rx = 0.048f * crease * (0.55f + 0.45f * end), rz = 0.040f * crease * (0.55f + 0.45f * end);
    *out = v3(sq * rx, y * (0.6f + 0.4f * end), sz * rz - 0.012f);
}

static V3 brow_surf[16];

/* an eyelid over an eye (centre c, radii r): the eye's shape a little larger,
 * from the top down to the angle edge (from straight up), with a rounded rim */
static void eyelid(V3 c, V3 r, float edge, float tilt) {
    const int nu = 28, rows = 10;
    const float grow = 1.09f;
    r = v3(r.x * grow, r.y * grow, r.z + 0.03f);   /* (deep enough to clear the iris and pupil) */
    if (nverts + nu * (rows + 1) > MAXV) return;
    int base = nverts;
    const float *cs = circle(nu, false), *sn = circle(nu, true);
    for (int j = 0; j <= rows; j++) {
        for (int i = 0; i < nu; i++) {
            /* the edge slopes (lower at the outer corner), the shell itself stays on the eye's shape */
            float e = edge + tilt * cs[i];
            if (e > 3.05f) e = 3.05f;
            float th = e * j / rows, st = sinf(th), ct = cosf(th);
            V3 u = v3(st * cs[i], ct, st * sn[i]);
            V3 p = v3(u.x * r.x, u.y * r.y, u.z * r.z);
            V3 n = norm(v3(u.x / r.x, u.y / r.y, u.z / r.z));
            vert(add(p, c), n);
        }
    }
    grid(base, nu, rows + 1);
    /* the rim: a soft roll along the front of the edge */
    V3 rim[17];
    for (int i = 0; i < 17; i++) {
        float a = 3.1415927f + 3.1415927f * i / 16;   /* the front half */
        float e = edge + tilt * cosf(a);
        if (e > 3.05f) e = 3.05f;
        float st = sinf(e), ct = cosf(e);
        rim[i] = add(v3(st * cosf(a) * r.x * 1.02f, ct * r.y * 1.02f, st * sinf(a) * r.z * 1.02f), c);
    }
    tube(rim, 17, 0.013f, 8);
}

typedef struct { V3 c, r; M3 inv; bool rot; float col[3]; float shine; bool hair; } Blob;
#define MAXB 40
static Blob blobs[MAXB];
static int nblobs;
#define K_SKIN 0.055f    /* how much the parts of his face melt together */
#define K_HAIR 0.045f    /* ...and the clumps of hair */
#define K_JOIN 0.018f    /* ...and the hair into his head (a little crease) */

static void blob(float cx, float cy, float cz, float rx_, float ry_, float rz_, const M3 *rot, const float *col, float shine, bool hair) {
    if (nblobs >= MAXB) return;
    Blob *b = &blobs[nblobs++];
    b->c = v3(cx, cy, cz); b->r = v3(rx_, ry_, rz_);
    b->rot = rot != NULL;
    if (rot) for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) b->inv.m[i][j] = rot->m[j][i];   /* (turning back: the transpose) */
    b->col[0] = col[0]; b->col[1] = col[1]; b->col[2] = col[2];
    b->shine = shine; b->hair = hair;
}

static inline float blob_d(const Blob *b, V3 p) {   /* about how far p is outside it */
    V3 q = sub(p, b->c);
    if (b->rot) q = apply(&b->inv, q);
    float ax = q.x / b->r.x, ay = q.y / b->r.y, az = q.z / b->r.z;
    float bx = ax / b->r.x, by = ay / b->r.y, bz = az / b->r.z;
    float k0 = sqrtf(ax * ax + ay * ay + az * az), k1 = sqrtf(bx * bx + by * by + bz * bz);
    return k1 > 1e-6f ? k0 * (k0 - 1) / k1 : -fminf(b->r.x, fminf(b->r.y, b->r.z));
}

static inline float smin(float a, float b, float k) {
    float h = k - fabsf(a - b);
    if (h <= 0) return a < b ? a : b;
    h /= k;
    return (a < b ? a : b) - h * h * k * 0.25f;
}

/* the distance to the surface from p, using the blobs listed (all: list NULL) */
static float sculpt_d(V3 p, const unsigned char *list, int n) {
    float ds = 1e9f, dh = 1e9f;
    int count = list ? n : nblobs;
    for (int k = 0; k < count; k++) {
        const Blob *b = &blobs[list ? list[k] : k];
        float d = blob_d(b, p);
        if (b->hair) dh = smin(dh, d, K_HAIR); else ds = smin(ds, d, K_SKIN);
    }
    return smin(ds, dh, K_JOIN);
}

static void make_blobs(void) {
    static const float SK[3] = { 0.90f, 0.73f, 0.64f }, CHEEK[3] = { 0.93f, 0.66f, 0.60f }, EAR[3] = { 0.92f, 0.66f, 0.58f },
                       NOSE[3] = { 0.95f, 0.65f, 0.57f }, LID[3] = { 0.89f, 0.68f, 0.60f }, HAIR[3] = { 0.80f, 0.80f, 0.84f };
    nblobs = 0;
    M3 r;
    blob(0, 0.40f, 0, 0.56f, 0.74f, 0.60f, NULL, SK, 0.08f, false);          /* the head: a tall egg */
    blob(0, 0.02f, -0.06f, 0.46f, 0.36f, 0.50f, NULL, SK, 0.08f, false);     /* a fuller jaw */
    blob(0, -0.27f, 0.02f, 0.20f, 0.16f, 0.20f, NULL, SK, 0.06f, false);     /* the neck */
    r = rotx(0.3f); blob(0, 0.36f, -0.62f, 0.07f, 0.17f, 0.10f, &r, SK, 0.1f, false);   /* a long nose */
    blob(0, 0.24f, -0.70f, 0.085f, 0.075f, 0.08f, NULL, NOSE, 0.5f, false);
    for (int sx = -1; sx <= 1; sx += 2) {
        blob(0.065f * sx, 0.22f, -0.66f, 0.045f, 0.04f, 0.04f, NULL, NOSE, 0.15f, false);   /* nostrils */
        blob(0.29f * sx, 0.23f, -0.41f, 0.13f, 0.10f, 0.09f, NULL, CHEEK, 0.1f, false);     /* cheeks */
        r = rotz(-0.12f * sx); blob(0.57f * sx, 0.40f, 0.03f, 0.075f, 0.16f, 0.07f, &r, EAR, 0.08f, false);   /* ears */
        blob(0.20f * sx, 0.45f, -0.50f, 0.12f, 0.035f, 0.08f, NULL, LID, 0.08f, false);    /* under the eyes */
        /* tufts of grey hair round the sides and back, swept back */
        static const float tufts[7][7] = {
            { 0.58f, 0.68f, 0.10f, 0.17f, 0.22f, 0.30f, 0.45f }, { 0.62f, 0.46f, 0.16f, 0.14f, 0.22f, 0.28f, 0.10f },
            { 0.47f, 0.88f, 0.20f, 0.19f, 0.13f, 0.28f, 0.75f }, { 0.34f, 0.74f, 0.42f, 0.22f, 0.24f, 0.21f, 0.20f },
            { 0.64f, 0.60f, -0.04f, 0.10f, 0.15f, 0.13f, 0.30f }, { 0.54f, 0.30f, 0.30f, 0.13f, 0.13f, 0.21f, -0.2f },
            { 0.66f, 0.80f, -0.08f, 0.08f, 0.09f, 0.09f, 0.90f } };
        for (int k = 0; k < 7; k++) {
            r = rotz(tufts[k][6] * sx);
            blob(tufts[k][0] * sx, tufts[k][1], tufts[k][2], tufts[k][3], tufts[k][4], tufts[k][5], &r, HAIR, 0.04f, true);
        }
    }
    blob(0, 0.62f, 0.47f, 0.30f, 0.25f, 0.13f, NULL, HAIR, 0.04f, true);     /* the back of the head */
    blob(0, 0.40f, 0.50f, 0.26f, 0.20f, 0.11f, NULL, HAIR, 0.04f, true);
}

/* a point on the front of his face (for the lines and the mouth drawn on it) */
static V3 surf_trace(float x, float y) {
    V3 p = v3(x, y, -1.2f);
    unsigned char list[MAXB];   /* (only the parts this line passes near) */
    int n = 0;
    for (int k = 0; k < nblobs; k++) {
        const Blob *b = &blobs[k];
        float rad = fmaxf(b->r.x, fmaxf(b->r.y, b->r.z)) + 0.12f, dx = b->c.x - x, dy = b->c.y - y;
        if (dx * dx + dy * dy < rad * rad) list[n++] = (unsigned char)k;
    }
    if (!n) return v3(x, y, 0);
    for (int i = 0; i < 60; i++) {
        float d = sculpt_d(p, list, n);
        if (d < 0.0005f) break;
        p.z += d * 0.9f;
        if (p.z > 0) break;
    }
    return p;
}
/* (worked out once on a grid over his face; between its points, blended) */
#define ZM_X0 -0.5f
#define ZM_Y0 -0.3f
#define ZM_STEP 0.025f
#define ZM_W 41
#define ZM_H 57
static float zmap[ZM_H][ZM_W];
static bool zmap_made;
static void make_zmap(void) {
    for (int j = 0; j < ZM_H; j++)
        for (int i = 0; i < ZM_W; i++) zmap[j][i] = surf_trace(ZM_X0 + i * ZM_STEP, ZM_Y0 + j * ZM_STEP).z;
    zmap_made = true;
}
static V3 surf(float x, float y, float out) {
    float fx = (x - ZM_X0) / ZM_STEP, fy = (y - ZM_Y0) / ZM_STEP;
    if (!zmap_made || fx < 0 || fy < 0 || fx >= ZM_W - 1 || fy >= ZM_H - 1) { V3 p = surf_trace(x, y); p.z -= out; return p; }
    int i = (int)fx, j = (int)fy;
    float u = fx - i, v = fy - j;
    float z = (zmap[j][i] * (1 - u) + zmap[j][i + 1] * u) * (1 - v) + (zmap[j + 1][i] * (1 - u) + zmap[j + 1][i + 1] * u) * v;
    return v3(x, y, z - out);
}

/* the sculpted head, as a mesh (static) */
#define SC_COLS 96
#define SC_ROWS 64
static void sculpt(void) {
    const V3 O = v3(0, 0.30f, 0);
    if (nverts + SC_COLS * (SC_ROWS + 1) > MAXV) return;
    int base = nverts;
    for (int j = 0; j <= SC_ROWS; j++) {
        /* rows: more of them across the middle (his face) */
        float w = (float)j / SC_ROWS * 2 - 1;                 /* -1 top .. 1 bottom */
        float theta = 1.5707963f * (1 + 0.62f * w + 0.38f * w * w * w);
        for (int i = 0; i < SC_COLS; i++) {
            /* columns: more of them round the front */
            float v = (float)i / SC_COLS * 2 - 1;             /* -1..1, the front at 0 */
            float phi = -1.5707963f + 3.1415927f * (0.5f * v + 0.5f * v * v * v);
            V3 dir = v3(sinf(theta) * cosf(phi), cosf(theta), sinf(theta) * sinf(phi));
            /* the parts near this ray */
            unsigned char list[MAXB];
            int n = 0;
            for (int k = 0; k < nblobs; k++) {
                const Blob *b = &blobs[k];
                float rad = fmaxf(b->r.x, fmaxf(b->r.y, b->r.z)) + 0.12f;
                V3 oc = sub(b->c, O);
                float along = dot(oc, dir);
                V3 closest = sub(oc, mul(dir, along < 0 ? 0 : along));
                if (dot(closest, closest) < rad * rad) list[n++] = (unsigned char)k;
            }
            /* from outside, inwards, to the surface */
            float t = 1.45f;
            for (int it = 0; it < 80; it++) {
                float d = sculpt_d(add(O, mul(dir, t)), list, n);
                if (d < 0.0004f) break;
                t -= fmaxf(d * 0.85f, 0.0015f);
                if (t < 0.02f) { t = 0.02f; break; }
            }
            V3 p = add(O, mul(dir, t));
            /* its slope (the normal), and its colour from the parts nearest */
            const float e = 0.004f;
            V3 nrm = norm(v3(sculpt_d(add(p, v3(e, 0, 0)), list, n) - sculpt_d(sub(p, v3(e, 0, 0)), list, n),
                             sculpt_d(add(p, v3(0, e, 0)), list, n) - sculpt_d(sub(p, v3(0, e, 0)), list, n),
                             sculpt_d(add(p, v3(0, 0, e)), list, n) - sculpt_d(sub(p, v3(0, 0, e)), list, n)));
            float wsum = 0, col[3] = { 0, 0, 0 }, shine = 0, dmin = 1e9f, dh = 1e9f, ds = 1e9f;
            float dd[MAXB];
            for (int k = 0; k < n; k++) {
                dd[k] = blob_d(&blobs[list[k]], p);
                if (dd[k] < dmin) dmin = dd[k];
                if (blobs[list[k]].hair) dh = fminf(dh, dd[k]); else ds = fminf(ds, dd[k]);
            }
            /* hair or skin: a crisp edge between them; the skin's tints blend softly */
            float hairy = 0.5f + (ds - dh) / 0.016f;
            hairy = hairy < 0 ? 0 : hairy > 1 ? 1 : hairy;
            float skin_col[3] = { 0, 0, 0 }, skin_sh = 0, sw = 0;
            for (int k = 0; k < n; k++) {
                const Blob *b = &blobs[list[k]];
                if (b->hair) continue;
                float wk = expf(-(dd[k] - ds) / 0.02f);
                skin_col[0] += b->col[0] * wk; skin_col[1] += b->col[1] * wk; skin_col[2] += b->col[2] * wk;
                skin_sh += b->shine * wk; sw += wk;
            }
            if (sw > 0) { skin_col[0] /= sw; skin_col[1] /= sw; skin_col[2] /= sw; skin_sh /= sw; }
            else { skin_col[0] = 0.90f; skin_col[1] = 0.73f; skin_col[2] = 0.64f; skin_sh = 0.08f; }
            /* the hair: a little darker in the creases between clumps */
            float crease = 0;
            {
                float d2 = 1e9f;   /* the nearest hair clump on its own */
                for (int k = 0; k < n; k++) if (blobs[list[k]].hair) d2 = fminf(d2, dd[k]);
                crease = d2 > 0 ? fminf(d2 / 0.03f, 1) : 0;
            }
            float hk = 1 - 0.22f * crease;
            col[0] = skin_col[0] * (1 - hairy) + 0.80f * hk * hairy;
            col[1] = skin_col[1] * (1 - hairy) + 0.80f * hk * hairy;
            col[2] = skin_col[2] * (1 - hairy) + 0.85f * hk * hairy;
            shine = skin_sh * (1 - hairy) + 0.04f * hairy;
            (void)wsum; (void)dmin;
            colour(col[0], col[1], col[2], SKIN);
            int vi = vert(p, nrm);
            if (vi >= 0) vsk[vi] = shine;
        }
    }
    grid(base, SC_COLS, SC_ROWS + 1);
}

/* ------------------------------------------------------------ the professor */

/* The parts that never change shape (most of him) are made and lit once
 * (S); the moving ones (eyes, lids, brows, mouth, spectacles, bow tie, the
 * globe's rings, the scribble) every frame (D). How the whole head tilts and
 * bobs, and the props float, is applied when he's drawn (place()). */
static int ns, nst;   /* the static part: vertices and triangles */
static void build(const HeadPose *ps, bool S) {
    bool D = !S;
    if (S) nverts = ntris = 0; else { nverts = ns; ntris = nst; }
    cur_grp = G_HEAD;
    float t = ps->t, talk = ps->talk, blink = ps->blink, th = ps->think;
    const float SK[3] = { 0.90f, 0.73f, 0.64f }, BROW[3] = { 0.86f, 0.86f, 0.90f };
    if (S) {
    make_blobs();
    make_zmap();
    sculpt();   /* head, face, ears, neck and hair: one smooth surface */
    }
    for (int sx = -1; sx <= 1; sx += 2) {
        if (S) {
        /* eyes, with heavy upper lids (a wise, amused squint) */
        colour(0.98f, 0.98f, 1.0f, GLOSS); ell(0.20f * sx, 0.52f, -0.50f, 0.125f, 0.11f, 0.085f, 24, 16, NULL);
        continue;
        }
        colour(0.20f, 0.30f, 0.48f, GLOSS);
        ell(0.19f * sx + 0.01f + 0.025f * th + ps->look_x * 0.03f, 0.51f + 0.03f * th, -0.58f, 0.058f, 0.064f, 0.02f, 16, 10, NULL);
        colour(0.03f, 0.04f, 0.08f, GLOSS);
        ell(0.19f * sx + 0.01f + 0.025f * th + ps->look_x * 0.03f, 0.51f + 0.03f * th, -0.596f, 0.026f, 0.029f, 0.008f, 12, 6, NULL);   /* the pupil */
        /* the upper lid: a smooth shell over the top of the eye, down to a soft
         * rounded edge (heavy-lidded; all the way down for a blink) */
        colour(SK[0], SK[1], SK[2], SKIN);
        eyelid(v3(0.20f * sx, 0.52f, -0.50f), v3(0.125f, 0.11f, 0.085f), 1.28f + (2.95f - 1.28f) * blink, 0.16f * sx * (1 - blink));
        /* big expressive eyebrows */
        float lift = 0.03f * talk + (sx > 0 ? 0.07f : -0.015f) * th + ps->brows * 0.04f;
        colour(BROW[0], BROW[1], BROW[2], MATTE);
        {
            /* a bushy brow: thick at the inner end, sweeping up and out to a point */
            V3 p[8]; float rad[8];
            for (int i = 0; i < 8; i++) {
                float u = i / 7.0f;
                float x = (0.07f + 0.36f * u) * sx;
                float y = 0.665f + lift + 0.07f * sinf(u * 2.6f) - 0.01f * u;
                p[i] = add(brow_surf[i + (sx > 0 ? 8 : 0)], v3(x - brow_surf[i + (sx > 0 ? 8 : 0)].x, y - brow_surf[i + (sx > 0 ? 8 : 0)].y, 0));
                rad[i] = 0.018f + 0.032f * sinf(3.1415927f * (0.18f + 0.82f * u)) * (1 - 0.45f * u);
            }
            tube_r(p, 8, rad, 10);
        }
    }
    if (S) {
    /* forehead lines */
    colour(0.80f, 0.56f, 0.48f, MATTE);
    for (int k = 0; k < 2; k++) {
        V3 p[9];
        for (int i = 0; i < 9; i++) {
            float x = -0.2f + k * 0.03f + (0.4f - k * 0.06f) * i / 8;
            p[i] = surf(x, (k ? 0.87f : 0.815f) + 0.015f * cosf(x * 6), 0.008f);
        }
        tube(p, 9, 0.009f, 4);
    }
    /* where the brows sit (worked out once: they move up and down from here) */
    for (int sx = -1, k = 0; sx <= 1; sx += 2)
        for (int i = 0; i < 8; i++, k++) {
            float u = i / 7.0f;
            brow_surf[k] = surf((0.07f + 0.36f * u) * sx, 0.665f + 0.07f * sinf(u * 2.6f), 0.012f);
        }
    /* a big, full grey moustache: a rounded lobe each side, meeting under his
     * nose and sweeping up at the ends into little curls */
    colour(0.84f, 0.84f, 0.88f, MATTE);
    {
        static const float key[10][3] = {   /* x, y, thickness, from the middle out to the curl */
            { -0.020f, 0.150f, 0.048f }, { 0.040f, 0.140f, 0.066f }, { 0.100f, 0.118f, 0.086f }, { 0.155f, 0.104f, 0.092f },
            { 0.208f, 0.110f, 0.080f }, { 0.246f, 0.134f, 0.058f }, { 0.264f, 0.168f, 0.038f }, { 0.258f, 0.198f, 0.023f },
            { 0.238f, 0.214f, 0.013f }, { 0.218f, 0.212f, 0.006f } };
        enum { N = 28 };
        for (int sx = -1; sx <= 1; sx += 2) {
            V3 p[N]; float rad[N];
            for (int i = 0; i < N; i++) {
                /* a smooth curve through the key points (Catmull-Rom) */
                float f = (float)i / (N - 1) * 9;
                int k = (int)f; if (k > 8) k = 8;
                float t = f - k;
                float v[3];
                for (int c = 0; c < 3; c++) {
                    float p0 = key[k > 0 ? k - 1 : 0][c], p1 = key[k][c], p2 = key[k + 1][c], p3 = key[k + 2 < 10 ? k + 2 : 9][c];
                    v[c] = 0.5f * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
                }
                rad[i] = v[2];
                p[i] = surf(v[0] * sx, v[1], v[2] * 0.45f);
            }
            tube_r(p, N, rad, 16);
        }
    }
    }
    if (D) {
    /* the mouth: a lopsided, knowing smile, or open as he talks */
    if (talk > 0.05f) {
        float mh = 0.022f + 0.05f * talk, mw = 0.115f - 0.015f * talk;
        V3 m = surf(0, 0.012f, -0.016f);   /* (just under the moustache) */
        colour(0.30f, 0.07f, 0.11f, DARK); ell(0, 0.012f, m.z, mw, mh, 0.03f, 20, 10, NULL);
        if (talk > 0.25f) {   /* a glimpse of his top teeth */
            colour(0.96f, 0.94f, 0.90f, SKIN); ell(0, 0.012f + mh * 0.62f, m.z - 0.012f, mw * 0.62f, 0.016f, 0.02f, 14, 6, NULL);
        }
        /* his lips, round the edge */
        V3 lp[25];
        for (int i = 0; i < 25; i++) {
            float a = 6.2831853f * i / 24;
            lp[i] = surf(cosf(a) * (mw + 0.008f), 0.012f + sinf(a) * (mh + 0.008f), 0.002f);
        }
        colour(0.78f, 0.44f, 0.42f, SKIN); tube(lp, 25, 0.015f, 6);
    } else {
        V3 p[11];
        int n = th < 0.5f ? 11 : 7;
        for (int i = 0; i < n; i++) {
            float x = th < 0.5f ? -0.15f + 0.32f * i / 10 : 0.14f * i / 6;
            float y = 0.0f + 0.9f * (x - 0.01f) * (x - 0.01f) + 0.12f * x;
            p[i] = surf(x, y, 0.004f);
        }
        colour(0.55f, 0.22f, 0.24f, MATTE); tube(p, n, 0.016f, 6);
    }
    /* spectacles: up on his forehead, or down on his nose when he thinks */
    {
        bool down = th >= 0.5f;
        M3 R = rotx(down ? 0.08f : 0.95f);
        V3 at = down ? v3(0, 0.50f, -0.66f) : v3(0, 0.975f, surf(0, 0.975f, 0).z - 0.035f);   /* (up: resting on his forehead, above the lines) */
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
    /* the red bow tie (it bounces): two puffy wings, pinched into folds at
     * the knot and flaring out to notched ends, and a wrapped knot */
    {
        M3 T = rotz(0.15f * sinf(t * 2) + 0.25f * ps->chuckle);
        const V3 at = v3(0, -0.42f, -0.31f);
        colour(0.86f, 0.10f, 0.14f, GLOSS);
        for (int sx = -1; sx <= 1; sx += 2) {
            M3 tilt = rotz(0.16f * sx);
            BowWing w = { sx, &tilt };
            param_surface(18, 24, bow_wing, &w, &T, at);
        }
        colour(0.80f, 0.08f, 0.12f, GLOSS);
        param_surface(10, 24, bow_knot, NULL, &T, at);
    }
    }
    /* floating props: a little glowing globe of the Earth that turns, and a
     * pencil that scribbles (always made, drawn only when wanted: see
     * head3d_draw). The globe turns as a whole (its matrix, in
     * head3d_draw), so its land stays put on it. */
    {
        V3 gc0 = GLOBE_C;
        if (S) {
        cur_grp = G_GLOBE;
        globe_v0 = nverts;
        /* the Earth: sea and land from the map, a little ice at the poles */
        {
            const int nu = 72, nvv = 40;
            const float R = 0.22f;
            int base = nverts;
            for (int j = 0; j <= nvv; j++) {
                float th = 3.1415927f * j / nvv, st = sinf(th), ct = cosf(th);
                float lat = 90 - 180.0f * j / nvv;
                for (int i = 0; i < nu; i++) {
                    float ph = 6.2831853f * i / nu;
                    V3 u = v3(st * cosf(ph), ct, st * sinf(ph));
                    /* (longitude round the globe; the map blended between its spots) */
                    float lon = -180 + 360.0f * i / nu;
                    float mx = (lon + 180) / 360 * EARTH_W - 0.5f, my = (90 - lat) / 180 * EARTH_H - 0.5f;
                    int x0 = (int)floorf(mx), y0 = (int)floorf(my);
                    float fx = mx - x0, fy = my - y0;
                    float land = ((earth_at(x0, y0) * (1 - fx) + earth_at(x0 + 1, y0) * fx) * (1 - fy) +
                                  (earth_at(x0, y0 + 1) * (1 - fx) + earth_at(x0 + 1, y0 + 1) * fx) * fy) / 15.0f;
                    land = land < 0.35f ? 0 : land > 0.65f ? 1 : (land - 0.35f) / 0.3f;
                    float ice = fabsf(lat) > 62 ? fminf((fabsf(lat) - 62) / 10, 1) : 0;
                    /* sea: deep blue, lighter towards the coasts; land: greens, drier towards the tropics' deserts */
                    float dry = expf(-((fabsf(lat) - 24) * (fabsf(lat) - 24)) / 120.0f) * 0.6f;
                    float sr = 0.10f, sg = 0.30f, sb = 0.78f;
                    float lr = 0.30f + 0.45f * dry, lg = 0.66f - 0.12f * dry, lb = 0.32f - 0.08f * dry;
                    float rr = sr + (lr - sr) * land, gg = sg + (lg - sg) * land, bb = sb + (lb - sb) * land;
                    float ik = land > 0.2f || fabsf(lat) > 74 ? ice : 0;
                    rr += (0.95f - rr) * ik; gg += (0.97f - gg) * ik; bb += (1.0f - bb) * ik;
                    colour(rr, gg, bb, land > 0.5f ? MATTE : GLOSS);
                    int vi = vert(add(gc0, mul(u, R)), u);
                    if (vi >= 0) vsk[vi] = land > 0.5f ? 0.06f : 0.55f;
                }
            }
            grid(base, nu, nvv + 1);
        }
        /* glowing hologram rings: a few lines of latitude and two meridians, turning with it */
        colour(0.55f, 0.95f, 1.0f, EMIS);
        for (int k = -1; k <= 1; k++) {
            V3 p[33];
            float y = 0.11f * k, rr = sqrtf(0.226f * 0.226f - y * y);
            for (int i = 0; i < 33; i++) { float a = 6.2831853f * i / 32; p[i] = add(gc0, v3(rr * cosf(a), y, rr * sinf(a))); }
            tube(p, 33, 0.005f, 4);
        }
        for (int k = 0; k < 2; k++) {
            V3 p[33];
            float ang = k * 1.5707963f;
            for (int i = 0; i < 33; i++) {
                float a = 6.2831853f * i / 32;
                p[i] = add(gc0, v3(0.226f * cosf(a) * cosf(ang), 0.226f * sinf(a), 0.226f * cosf(a) * sinf(ang)));
            }
            tube(p, 33, 0.005f, 4);
        }
        globe_v1 = nverts;
        /* the pencil: a six-sided yellow body, a metal band and a pink eraser
         * at one end, and at the other the sharpened wood narrowing to its lead */
        V3 pc = v3(1.06f, 0.10f, -0.35f), d = v3(sinf(-0.6f), -cosf(-0.6f), 0);
        cur_grp = G_PENCIL;
        #define ALONG(k) add(pc, mul(d, (k)))
        {
            V3 body[2] = { ALONG(-0.20f), ALONG(0.16f) };
            colour(0.98f, 0.80f, 0.25f, GLOSS); tube(body, 2, 0.040f, 6);
        }
        {
            V3 band[2] = { ALONG(-0.255f), ALONG(-0.20f) };
            colour(0.80f, 0.82f, 0.88f, GLOSS); tube(band, 2, 0.043f, 16);
            V3 rub[4] = { ALONG(-0.30f), ALONG(-0.295f), ALONG(-0.28f), ALONG(-0.255f) };
            float rr[4] = { 0.022f, 0.036f, 0.041f, 0.041f };
            colour(0.98f, 0.55f, 0.60f, MATTE); tube_r(rub, 4, rr, 16);
        }
        {
            V3 wood[2] = { ALONG(0.16f), ALONG(0.262f) };
            float wr[2] = { 0.040f, 0.012f };
            colour(0.93f, 0.78f, 0.58f, MATTE); tube_r(wood, 2, wr, 16);
            V3 lead[2] = { ALONG(0.262f), ALONG(0.305f) };
            float lr[2] = { 0.012f, 0.0015f };
            colour(0.22f, 0.22f, 0.26f, GLOSS); tube_r(lead, 2, lr, 12);
        }
        #undef ALONG
        }
        if (D) {
        cur_grp = G_FIXED;
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

/* how each group moves this frame (see head3d_draw) */
static M3 head_rot;
static float head_bob, globe_bob, pencil_bob;
static M3 globe_rot;      /* the globe turning, on its tilted axis */
static bool show_props;

/* --------------------------------------------------------------- lighting */

static float ramp(float d) {   /* soft light, with a gentle shoulder */
    static const float xs[4] = { 0, 0.15f, 0.45f, 1 }, ys[4] = { 0.30f, 0.42f, 0.80f, 1.0f };
    if (d <= 0) return ys[0];
    for (int i = 0; i < 3; i++)
        if (d <= xs[i + 1]) return ys[i] + (ys[i + 1] - ys[i]) * (d - xs[i]) / (xs[i + 1] - xs[i]);
    return 1;
}

static void light_n(float *r, float *g, float *b, int i, V3 normal);
static void light(float *r, float *g, float *b, int i) { light_n(r, g, b, i, vn[i]); }
static void light_n(float *r, float *g, float *b, int i, V3 normal) {
    static V3 key, fill, half;
    static bool once;
    if (!once) {
        once = true;
        key = norm(v3(-0.5f, 0.7f, -0.6f));
        fill = norm(v3(0.7f, -0.1f, -0.5f));
        half = norm(add(key, v3(0, 0, -1)));
    }
    V3 n = norm(normal);
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
        float sk = vsk[i] >= 0 ? vsk[i] : m == GLOSS ? 0.8f : m == SKIN ? 0.08f : 0.04f;
        R += spec * sk; Gc += spec * sk; B += spec * sk;
    }
    *r = R; *g = Gc; *b = B;
}

/* ------------------------------------------------------------------ drawing
 *
 * The GPU does the work: his vertices sit in arrays in memory (positions in
 * his own space, colours already lit), and the GPU turns, places and
 * projects them with a matrix for each part (the head, the globe, the pencil,
 * the rest) and a perspective matching the mock-ups. The parts that never
 * change are recorded once in display lists (the GPU replays them); the
 * moving parts are sent each frame as indices into the arrays. */

static f32 apos[MAXV * 3] ATTRIBUTE_ALIGN(32);
static u32 acol[MAXV] ATTRIBUTE_ALIGN(32);
enum { NGRP = 4 };
static unsigned short *stat_list[NGRP];            /* the static triangles of each group that can ever face him */
static int stat_n[NGRP];
static void *dl[NGRP];                             /* ...recorded as display lists */
static u32 dl_size[NGRP];
static float hb_min[3] = { 1e9f, 1e9f, 1e9f }, hb_max[3] = { -1e9f, -1e9f, -1e9f };   /* the head's extent (the box) */
static float view_l = 0, view_t = 0, view_r = 640, view_b = 480;
static GXColor tint = { 255, 255, 255, 255 };
void head3d_tint(u8 r, u8 g, u8 b) { tint.r = r; tint.g = g; tint.b = b; }
static bool zflip;

static const float CAMY = 0.15f, CAMZ = -4.9f;

static unsigned char to8(float v) { v *= 255; return v <= 0 ? 0 : v >= 255 ? 255 : (unsigned char)v; }

static void store(int i) {
    float r, g, b;
    light(&r, &g, &b, i);
    apos[i * 3] = vp[i].x; apos[i * 3 + 1] = vp[i].y; apos[i * 3 + 2] = vp[i].z;
    acol[i] = (u32)to8(r) << 24 | (u32)to8(g) << 16 | (u32)to8(b) << 8 | 0xFF;
}

/* can this triangle ever face him (or his reflection)? (the static parts
 * hardly turn, so those that always face away are left out for good) */
static bool can_face(int k) {
    if (vgrp[tri[k][0]] == G_GLOBE) return true;   /* (it turns: every side faces him in time) */
    static const V3 cams[2] = { { 0, 0.15f, -4.9f }, { 0, -2.05f, -4.9f } };
    for (int c = 0; c < 2; c++)
        for (int j = 0; j < 3; j++) {
            int i = tri[k][j];
            V3 to = norm(sub(vp[i], cams[c]));
            if (dot(norm(vn[i]), to) < 0.15f) return true;
        }
    return false;
}

static void send_tris(const unsigned short *list, int n) {
    for (int start = 0; start < n; start += 20000) {
        int m = n - start < 20000 ? n - start : 20000;
        GX_Begin(GX_TRIANGLES, GX_VTXFMT2, (u16)(m * 3));
        for (int j = start; j < start + m; j++) {
            int k = list[j];
            for (int c = 0; c < 3; c++) { GX_Position1x16(tri[k][c]); GX_Color1x16(tri[k][c]); }
        }
        GX_End();
    }
}

static void vertex_setup(void) {
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX16);
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetArray(GX_VA_POS, apos, 3 * sizeof(f32));
    GX_SetArray(GX_VA_CLR0, acol, sizeof(u32));
}

static void make_static(void) {
    for (int i = 0; i < ns; i++) {
        store(i);
        if (vgrp[i] == G_HEAD) {
            float c[3] = { vp[i].x, vp[i].y, vp[i].z };
            for (int a = 0; a < 3; a++) { if (c[a] < hb_min[a]) hb_min[a] = c[a]; if (c[a] > hb_max[a]) hb_max[a] = c[a]; }
        }
    }
    DCFlushRange(apos, sizeof(f32) * 3 * ns);
    DCFlushRange(acol, sizeof(u32) * ns);
    for (int g = 0; g < NGRP; g++) {
        stat_list[g] = malloc(sizeof(unsigned short) * (nst + 1));
        stat_n[g] = 0;
    }
    for (int k = 0; k < nst; k++) {
        int g = vgrp[tri[k][0]];
        if (g < NGRP && stat_list[g] && can_face(k)) stat_list[g][stat_n[g]++] = (unsigned short)k;
    }
#ifndef HEAD3D_NO_DISPLAY_LISTS
    vertex_setup();
    for (int g = 0; g < NGRP; g++) {
        if (!stat_n[g]) continue;
        u32 cap = ((u32)stat_n[g] * 3 * 4 + 64 * (stat_n[g] / 20000 + 1) + 127) & ~31u;
        dl[g] = memalign(32, cap);
        if (!dl[g]) continue;
        DCInvalidateRange(dl[g], cap);
        GX_BeginDispList(dl[g], cap);
        send_tris(stat_list[g], stat_n[g]);
        for (int k = 0; k < 32; k++) wgPipe->U8 = 0;   /* (no-ops: push the last of it out to memory) */
        dl_size[g] = GX_EndDispList();
        if (!dl_size[g]) { free(dl[g]); dl[g] = NULL; }   /* (sent the ordinary way instead) */
    }
#endif
}

void head3d_build(const HeadPose *ps) {
    static bool made;
    if (!made) {   /* the parts that never change: made, lit and recorded once */
        build(ps, true);
        make_static();
        made = true;
    }
    build(ps, false);
    float t = ps->t, th = ps->think;
    head_rot = rotz(0.07f * th + 0.02f * sinf(t) + 0.03f * ps->chuckle * sinf(t * 18));
    head_bob = 0.02f * sinf(t * 1.1f);
    globe_bob = 0.04f * sinf(t * 1.4f);
    {
        /* a turn every 20 seconds, west to east, on an axis tilted like the Earth's */
        float a = -6.2831853f * t / 20.0f, c = cosf(a), sn = sinf(a);
        M3 spin = { { { c, 0, sn }, { 0, 1, 0 }, { -sn, 0, c } } };
        M3 tilt = rotz(-0.41f);
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
            globe_rot.m[i][j] = tilt.m[i][0] * spin.m[0][j] + tilt.m[i][1] * spin.m[1][j] + tilt.m[i][2] * spin.m[2][j];
        /* its light stays where it is while it turns: lit again with the turned normals */
        for (int i = globe_v0; i < globe_v1; i++) {
            float r, g, b;
            light_n(&r, &g, &b, i, apply(&globe_rot, vn[i]));
            acol[i] = (u32)to8(r) << 24 | (u32)to8(g) << 16 | (u32)to8(b) << 8 | 0xFF;
        }
        if (globe_v1 > globe_v0) DCFlushRange(acol + globe_v0, sizeof(u32) * (globe_v1 - globe_v0));
    }
    pencil_bob = 0.05f * sinf(t * 3);
    show_props = ps->props;
    for (int i = ns; i < nverts; i++) store(i);
    if (nverts > ns) {
        DCFlushRange(apos + ns * 3, sizeof(f32) * 3 * (nverts - ns));
        DCFlushRange(acol + ns, sizeof(u32) * (nverts - ns));
    }
}

void head3d_view(float l, float t, float r, float b, bool depth_flip) {
    view_l = l; view_t = t; view_r = r; view_b = b; zflip = depth_flip;
}

/* the matrix for group g: its own movement, the reflection (if any), then the camera */
static void group_mtx(int g, float mirror_y, Mtx m) {
    /* its movement: p' = A p + o */
    M3 A = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
    V3 o = v3(0, 0, 0);
    if (g == G_HEAD) {
        V3 c0 = v3(0, 0.2f, 0);
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) A.m[i][j] = head_rot.m[i][j] * 1.16f;
        o = add(sub(c0, apply(&A, c0)), v3(0, -0.02f + head_bob, 0));
    } else if (g == G_GLOBE) {   /* turning about its centre, and bobbing */
        V3 c = GLOBE_C;
        A = globe_rot;
        o = add(sub(c, apply(&A, c)), v3(0, globe_bob, 0));
    }
    else if (g == G_PENCIL) o = v3(0, pencil_bob, 0);
    /* the reflection: y -> 2 mirror_y - y */
    if (mirror_y > -100) {
        for (int j = 0; j < 3; j++) A.m[1][j] = -A.m[1][j];
        o.y = 2 * mirror_y - o.y;
    }
    /* the camera: x, y - CAMY, -(z - CAMZ) */
    for (int j = 0; j < 3; j++) { m[0][j] = A.m[0][j]; m[1][j] = A.m[1][j]; m[2][j] = -A.m[2][j]; }
    m[0][3] = o.x; m[1][3] = o.y - CAMY; m[2][3] = -(o.z - CAMZ);
}

void head3d_draw(float x, float y, float scale, float alpha, float mirror_y, HeadBox *box) {
    const float FOV = 560 * scale, NEAR = 2.5f, FAR = 9.0f;
    float hw = (view_r - view_l) / 2, mx = (view_l + view_r) / 2, hh = (view_b - view_t) / 2, my = (view_t + view_b) / 2;
    /* where the head lands (for the scanlines over him) */
    if (box) {
        Mtx m;
        group_mtx(G_HEAD, mirror_y, m);
        box->x0 = box->y0 = 1e9f; box->x1 = box->y1 = -1e9f;
        for (int c = 0; c < 8; c++) {
            float p[3] = { (c & 1) ? hb_max[0] : hb_min[0], (c & 2) ? hb_max[1] : hb_min[1], (c & 4) ? hb_max[2] : hb_min[2] };
            float X = m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3];
            float Y = m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3];
            float Z = -(m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3]);
            if (Z < 0.5f) Z = 0.5f;
            float sx = x + FOV * X / Z, sy = y - FOV * Y / Z;
            if (sx < box->x0) box->x0 = sx;
            if (sx > box->x1) box->x1 = sx;
            if (sy < box->y0) box->y0 = sy;
            if (sy > box->y1) box->y1 = sy;
        }
    }
    /* the perspective: x_layout = x + FOV X / Z, y_layout = y - FOV Y / Z, into
     * the visible part of the layout; depth from NEAR (0) to FAR (1) */
    Mtx44 P;
    memset(P, 0, sizeof(P));
    P[0][0] = FOV / hw; P[0][2] = -(x - mx) / hw;
    P[1][1] = FOV / hh; P[1][2] = (y - my) / hh;
    P[2][2] = -NEAR / (FAR - NEAR); P[2][3] = -(FAR * NEAR) / (FAR - NEAR);
    P[3][2] = -1;
    GX_LoadProjectionMtx(P, GX_PERSPECTIVE);
    vertex_setup();
    GX_InvVtxCache();
    /* colour as lit; see-through by alpha (the reflection) */
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    /* colour: as lit (times the tint, only when there is one) */
    if (tint.r == 255 && tint.g == 255 && tint.b == 255)
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
    else {
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_RASC, GX_CC_KONST, GX_CC_ZERO);
        GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
    }
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevKColor(GX_KCOLOR0, (GXColor){ tint.r, tint.g, tint.b, to8(alpha) });
    GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_RASA, GX_CA_KONST, GX_CA_ZERO);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetZMode(GX_TRUE, zflip ? GX_GEQUAL : GX_LEQUAL, GX_TRUE);
    GX_SetCullMode(GX_CULL_NONE);   /* (every shape is closed: the depth test hides the backs) */
    /* the moving parts, by group */
    static unsigned short dyn[NGRP][MAXT];
    int dn[NGRP] = { 0, 0, 0, 0 };
    for (int k = nst; k < ntris; k++) {
        int g = vgrp[tri[k][0]];
        if (g < NGRP) dyn[g][dn[g]++] = (unsigned short)k;
    }
    for (int g = 0; g < NGRP; g++) {
        if (g != G_HEAD && !show_props) continue;
        Mtx m;
        group_mtx(g, mirror_y, m);
        GX_LoadPosMtxImm(m, GX_PNMTX0);
        GX_SetCurrentMtx(GX_PNMTX0);
        if (dl[g]) GX_CallDispList(dl[g], dl_size[g]);
        else send_tris(stat_list[g], stat_n[g]);
        send_tris(dyn[g], dn[g]);
    }
}

int head3d_triangles(void) { return ntris; }

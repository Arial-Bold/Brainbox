/* holosynth.c - see holosynth.h. The hologram switching on: the beam buzzing
 * on and off a few times as it struggles to start, then a quick upward zip
 * as it locks in, and two soft chimes, a fifth apart, in a little room.
 * Made from scratch at start-up (nothing recorded). */
#include "holosynth.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LEN_S 2.4f
#define PI 3.14159265f

int holo_length(void) { return (int)(LEN_S * HOLO_RATE); }

/* when the beam is on, in seconds from the start (the stutter, then on for good) */
static const float gate_on[5][2] = { { 0.00f, 0.05f }, { 0.12f, 0.16f }, { 0.22f, 0.30f }, { 0.36f, 0.40f }, { 0.46f, 99 } };

float holo_gate(float t) {
    for (int i = 0; i < 5; i++) if (t >= gate_on[i][0] && t < gate_on[i][1]) return 1;
    return 0;
}

static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }

bool holo_render(int16_t *out, int n) {
    float *x = malloc(sizeof(float) * n);
    float *y = malloc(sizeof(float) * n);
    if (!x || !y) { free(x); free(y); return false; }
    const float SR = HOLO_RATE;
    float gate_lp = 0, buzz_lp = 0, ph = 0;
    for (int i = 0; i < n; i++) {
        float t = i / SR, v = 0;
        /* the buzz of the beam, gated (softened so it doesn't click) */
        gate_lp += 0.02f * (holo_gate(t) - gate_lp);
        float sq = sinf(2 * PI * 110 * t) >= 0 ? 1.0f : -1.0f;
        float buzz = sq * 0.3f + sinf(2 * PI * 220 * t) * 0.5f + sinf(2 * PI * 330 * t) * 0.2f;
        buzz_lp += 0.12f * (buzz - buzz_lp);
        float be = gate_lp * clampf((LEN_S - t) / 1.4f, 0, 1) * (0.6f + 0.4f * clampf((0.6f - t) / 0.6f, 0, 1));
        v += buzz_lp * be * 0.22f;
        /* the zip up as it locks in */
        float tt = t - 0.42f;
        float f = 300 * powf(2400.0f / 300.0f, clampf(tt / 0.35f, 0, 1));
        ph += 2 * PI * f / SR;
        if (ph > 2 * PI * 1000) ph -= 2 * PI * 1000;
        if (tt >= 0 && tt < 0.35f) v += sinf(ph) * sinf(PI * clampf(tt / 0.35f, 0, 1)) * 0.25f;
        /* the two chimes */
        static const float chime[2][2] = { { 0.78f, 784.0f }, { 0.93f, 1174.7f } };
        for (int c = 0; c < 2; c++) {
            float ct = t - chime[c][0];
            if (ct < 0) continue;
            float fq = chime[c][1];
            v += (sinf(2 * PI * fq * ct) + 0.25f * sinf(2 * PI * fq * 3.01f * ct)) * expf(-ct / 0.6f) * 0.25f;
        }
        x[i] = v;
    }
    /* a little room: echoes of each sound, fading */
    static const float comb[6][2] = { { 0.029f, 0.5f }, { 0.037f, 0.45f }, { 0.041f, 0.42f }, { 0.053f, 0.38f }, { 0.067f, 0.33f }, { 0.079f, 0.3f } };
    const float mix = 0.45f / 3;
    memcpy(y, x, sizeof(float) * n);
    for (int c = 0; c < 6; c++) {
        int d = (int)(comb[c][0] * SR);
        float g = 1;
        for (int k = 1; k < 12; k++) {
            g *= comb[c][1];
            int off = d * k;
            if (off >= n) break;
            float gm = g * mix;
            for (int i = off; i < n; i++) y[i] += x[i - off] * gm;
        }
    }
    float peak = 1e-6f;
    for (int i = 0; i < n; i++) if (fabsf(y[i]) > peak) peak = fabsf(y[i]);
    float k = 0.85f * 32767 / peak;
    for (int i = 0; i < n; i++) out[i] = (int16_t)lrintf(y[i] * k);
    free(x);
    free(y);
    return true;
}

/* ------------------------------------------------------- the beaming */

/* A shimmer for beaming away and back: a cloud of high, glassy notes that
 * swells and sparkles (twice: going, and coming back), over a soft rising
 * and falling hum. (Its own sound, made here.) */
#define SHIM_S 3.1f
int shimmer_length(void) { return (int)(SHIM_S * HOLO_RATE); }

bool shimmer_render(int16_t *out, int n) {
    float *y = malloc(sizeof(float) * n);
    if (!y) return false;
    const float SR = HOLO_RATE;
    enum { NP = 14 };
    float fq[NP], ph[NP], rate[NP], off[NP];
    unsigned seed = 12345;
    for (int k = 0; k < NP; k++) {
        seed = seed * 1103515245u + 12345u; float r1 = ((seed >> 8) & 0xFFFF) / 65535.0f;
        seed = seed * 1103515245u + 12345u; float r2 = ((seed >> 8) & 0xFFFF) / 65535.0f;
        fq[k] = 1400 + 2600 * r1;            /* glassy highs */
        rate[k] = 7 + 11 * r2;               /* each twinkles at its own speed */
        off[k] = r1 * 6.28f;
        ph[k] = 0;
    }
    float hum_ph = 0;
    for (int i = 0; i < n; i++) {
        float t = i / SR;
        /* two swells: going (0..1.25 s) and coming back (1.85..3.1 s) */
        float a = 0;
        if (t < 1.25f) a = sinf(PI * t / 1.25f);
        else if (t > 1.85f) a = sinf(PI * (t - 1.85f) / 1.25f);
        float v = 0;
        for (int k = 0; k < NP; k++) {
            ph[k] += 2 * PI * fq[k] * (1 + 0.04f * a) / SR;
            if (ph[k] > 2 * PI) ph[k] -= 2 * PI;
            float tw = 0.5f + 0.5f * sinf(2 * PI * rate[k] * t + off[k]);
            v += sinf(ph[k]) * tw * tw;
        }
        v *= a * a / NP;
        /* the hum, rising as it goes and falling as it comes back */
        float hf = t < 1.55f ? 180 + 260 * (t / 1.55f) : 440 - 260 * ((t - 1.55f) / 1.55f);
        hum_ph += 2 * PI * hf / SR;
        if (hum_ph > 2 * PI) hum_ph -= 2 * PI;
        v += (sinf(hum_ph) + 0.3f * sinf(2 * hum_ph)) * a * 0.18f;
        y[i] = v;
    }
    /* a touch of the little room, as the opening has */
    float *x = malloc(sizeof(float) * n);
    if (x) {
        memcpy(x, y, sizeof(float) * n);
        static const float comb[4][2] = { { 0.031f, 0.45f }, { 0.043f, 0.4f }, { 0.057f, 0.35f }, { 0.071f, 0.3f } };
        for (int c = 0; c < 4; c++) {
            int d = (int)(comb[c][0] * SR);
            float g = 1;
            for (int k = 1; k < 10 && d * k < n; k++) {
                g *= comb[c][1];
                for (int i = d * k; i < n; i++) y[i] += x[i - d * k] * g * 0.12f;
            }
        }
        free(x);
    }
    float peak = 1e-6f;
    for (int i = 0; i < n; i++) if (fabsf(y[i]) > peak) peak = fabsf(y[i]);
    float k = 0.7f * 32767 / peak;
    for (int i = 0; i < n; i++) out[i] = (int16_t)lrintf(y[i] * k);
    free(y);
    return true;
}

/* ------------------------------------------------------- the background */

/* Every tone has a whole number of cycles in the loop (frequencies in
 * sixteenths of a hertz, for a 16 second loop), so it joins without a seam;
 * the softening filter is run round the loop twice for the same reason. */
int ambience_length(void) { return AMB_S * AMB_RATE; }

bool ambience_render(int16_t *out, int n) {
    float *y = malloc(sizeof(float) * n);
    if (!y) return false;
    const float SR = AMB_RATE, L = AMB_S;
    /* the chord: G and D, low (two of each a shade apart, so they beat slowly) */
    static const float pad[5][3] = { { 98.0f, 0.5f, 0.3f }, { 98.125f, 0.4f, 2.1f }, { 146.8125f, 0.45f, 4.0f },
                                     { 146.6875f, 0.3f, 1.2f }, { 196.0f, 0.18f, 5.3f } };
    /* the faint glassy notes: pitch, how slowly each fades in and out, where it starts */
    static const float shim[4][3] = { { 659.25f, 0.0625f, 0.4f }, { 783.9375f, 0.125f, 2.2f },
                                      { 880.0f, 0.1875f, 4.1f }, { 1046.5f, 0.25f, 5.5f } };
    for (int i = 0; i < n; i++) {
        float t = i / SR, p = 0, sh = 0;
        for (int k = 0; k < 5; k++) p += pad[k][1] * sinf(2 * PI * pad[k][0] * t + pad[k][2]);
        p *= 0.75f + 0.25f * sinf(2 * PI * t / L);   /* breathing, once a loop */
        for (int k = 0; k < 4; k++) {
            float tw = 0.5f + 0.5f * sinf(2 * PI * shim[k][1] * t + shim[k][2]);
            tw *= tw;
            sh += sinf(2 * PI * shim[k][0] * t) * tw * tw;
        }
        y[i] = 0.75f / 1.5f * p + 0.15f * sinf(2 * PI * 55.0f * t) + 0.05f / 2.0f * sh;
    }
    /* softened twice over (round the loop, twice each) */
    float a = expf(-2 * PI * 900.0f / SR);
    for (int f = 0; f < 2; f++) {
        float z = 0;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < n; i++) { z = a * z + (1 - a) * y[i]; if (pass) y[i] = z; }
    }
    float peak = 1e-6f;
    for (int i = 0; i < n; i++) if (fabsf(y[i]) > peak) peak = fabsf(y[i]);
    float k = 0.9f * 32767 / peak;
    for (int i = 0; i < n; i++) out[i] = (int16_t)lrintf(y[i] * k);
    free(y);
    return true;
}

int amb_bell_length(void) { return 2 * AMB_RATE; }

/* one soft bell note, far off (a ninth as loud as the background at its fullest) */
bool amb_bell_render(int16_t *out, int n, int which) {
    static const float notes[AMB_BELLS] = { 392.0f, 587.3f, 784.0f };
    const float SR = AMB_RATE, f = notes[which % AMB_BELLS];
    float a = expf(-2 * PI * 1200.0f / SR), z = 0, peak = 1e-6f;
    float *y = malloc(sizeof(float) * n);
    if (!y) return false;
    for (int i = 0; i < n; i++) {
        float t = i / SR;
        float v = sinf(2 * PI * f * t) * expf(-t / 0.6f) * (t < 0.03f ? t / 0.03f : 1);
        z = a * z + (1 - a) * v;
        y[i] = z;
        if (fabsf(z) > peak) peak = fabsf(z);
    }
    float k = 0.1f * 32767 / peak;
    for (int i = 0; i < n; i++) out[i] = (int16_t)lrintf(y[i] * k);
    free(y);
    return true;
}

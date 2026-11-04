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

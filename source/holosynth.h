/* holosynth.h - the sound of the hologram switching on (see holosynth.c),
 * and its timing, which the picture follows. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define HOLO_RATE 32000
#define HOLO_LOCK 0.42f    /* seconds in: the zip, as it locks in */
#define HOLO_SET 0.78f     /* the first chime: he's there */

int  holo_length(void);                  /* samples (mono 16-bit at HOLO_RATE) */
bool holo_render(int16_t *out, int n);
float holo_gate(float t);                /* 1 while the beam is on, t seconds in (the stutter) */

/* the beaming shimmer (the hidden "beam me up" moment): going 0..1.25 s, away, back 1.85..3.1 s */
#define SHIMMER_S 3.1f
int  shimmer_length(void);
bool shimmer_render(int16_t *out, int n);

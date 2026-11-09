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

/* the hologram's quiet background while BrainBox is open: a low, warm chord
 * breathing slowly, a faint hum and a few faint glassy notes, rounded soft.
 * One loop (it joins seamlessly), and soft far-off bell notes to play over it
 * now and then (which: 0..AMB_BELLS-1). */
/* (7992 a second, not 8000: so the loop is a whole number of the sound
 * library's 576-sample pieces. It pads a part-filled last piece with
 * silence, which was heard as a gap each time it went round.) */
#define AMB_RATE 7992
#define AMB_S 16
#define AMB_BELLS 3
int  ambience_length(void);
bool ambience_render(int16_t *out, int n);
int  amb_bell_length(void);
bool amb_bell_render(int16_t *out, int n, int which);

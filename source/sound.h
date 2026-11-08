/* sound.h - menu sounds (made in the background at start-up, see sfxsynth.c) played on the
 * DSP's hardware voices. */
#pragma once
#include "sfxsynth.h"
#include <stdbool.h>

void sound_init(void);
void sound_play(Sfx s);
/* The sound has been made (they're made in the background at start-up). */
bool sound_loaded(Sfx s);
void sound_stop(Sfx s);      /* stops it if it's still playing */
void sound_shutdown(void);   /* before launching another program */
/* the hologram switching on (the opening, see holosynth.c) */
bool sound_holo_ready(void);
void sound_play_holo(void);
void sound_play_shimmer(void);   /* (a little secret) */
/* The hologram's quiet background (holosynth.c), once a frame: it starts
 * (fading in) once the opening is over, follows the Menu sounds volume, dips
 * while he talks, and goes silent while the microphone is listening (so it
 * isn't heard by it). Now and then a soft bell note plays over it. */
void sound_ambience_tick(bool listening, bool talking);

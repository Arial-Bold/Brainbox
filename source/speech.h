/* speech.h - Professor Pixel's voice: Flite (the cmu_us_rms voice) on the
 * GameCube itself. A reply is spoken a sentence at a time: while the first
 * sentence plays, the next ones are made. The speech waits in ARAM (the
 * audio memory), so long replies take almost no main memory, and is streamed
 * to the sound chip from there. */
#pragma once
#include <stdbool.h>

void speech_init(void);           /* after sound_init; Flite starts on a thread of its own */
bool speech_ready(void);          /* Flite has started */
void speech_say(const char *text);/* says text (CP-1252), cutting off anything he was saying */
void speech_stop(void);           /* stops talking */
bool speech_busy(void);           /* still making or playing speech */
float speech_level(void);         /* how loud he is right now, 0..1 (for his mouth) */

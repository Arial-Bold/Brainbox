/* speech.h - Professor Scotty's voice: Flite (the cmu_us_awb voice, a Scottish accent) on the
 * GameCube itself. A reply is spoken a sentence at a time: while the first
 * sentence plays, the next ones are made. The speech waits in ARAM (the
 * audio memory), so long replies take almost no main memory, and is streamed
 * to the sound chip from there. Sounds, slang and abbreviations it would
 * get wrong are tidied first (saywords.c). */
#pragma once
#include <stdbool.h>

void speech_init(void);           /* after sound_init; Flite starts on a thread of its own */
bool speech_ready(void);          /* Flite has started, with a voice */
/* the voice to use: a .flitevox file (loaded in the background; speech_ready
 * goes false until it's in). speech_voice_missing: it couldn't be loaded. */
void speech_voice(const char *path);
bool speech_voice_missing(void);
void speech_say(const char *text);/* says text (CP-1252), cutting off anything he was saying */
void speech_stop(void);           /* stops talking */
bool speech_busy(void);           /* still making or playing speech */
float speech_level(void);         /* how loud he is right now, 0..1 (for his mouth) */

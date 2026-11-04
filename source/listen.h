/* listen.h - the GameCube Microphone (in memory card slot A or B): hold a
 * button and talk, and what was said is kept as sound, ready to send to the
 * AI to be written down. */
#pragma once
#include <stdbool.h>

#define LISTEN_RATE 16000          /* the recording, as it's sent (samples a second) */
#define LISTEN_MAX_SECONDS 15

void listen_init(void);
bool listen_present(void);         /* is there a microphone in a slot? (looks again now and then) */
bool listen_start(void);           /* starts recording (false: no microphone) */
void listen_update(void);          /* once a frame while recording: collects what came in */
float listen_level(void);          /* how loud it is just now, 0..1 */
float listen_seconds(void);        /* how long so far */
bool listen_full(void);            /* LISTEN_MAX_SECONDS reached */
bool listen_button(void);          /* the button on the microphone is held */
/* stops; the recording (16-bit, LISTEN_RATE, cleaned up and evened out) and
 * its length in samples (0: too short). false: nothing was said (too short
 * or too quiet). */
bool listen_stop(const short **pcm, int *n);
const char *listen_info(void);     /* about the last recording (for mic.txt) */
bool listen_wav(const short *pcm, int n, unsigned char **data, int *len);   /* as a .wav file (free *data) */

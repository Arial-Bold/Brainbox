/* voicefetch.h - gets a hologram's voice file from GitHub when it isn't
 * beside the app (or won't load), so BrainBox can still talk. */
#pragma once
#include <stdbool.h>

enum { VF_IDLE, VF_WORKING, VF_DONE, VF_FAILED };

/* Starts fetching name (e.g. "voice_scotty.flitevox") to path, in the background. */
void voicefetch_start(const char *name, const char *path);
int voicefetch_state(void);
int voicefetch_percent(void);        /* -1 while the size isn't known */
const char *voicefetch_error(void);

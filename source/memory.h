/* memory.h - what the hologram remembers about the person from one day to
 * the next (added in 1.5): their name, a few things they've told him, and
 * what they asked about lately. Kept in memory.txt beside the app on the SD
 * card, and only ever sent to the AI they chose, with their questions.
 * chat.memory=0 in brainbox.ini turns it off; asking him to forget clears it. */
#pragma once
#include <stdbool.h>

void memory_load(void);                       /* at start-up, once the SD card is ready */
void memory_enable(bool on);
bool memory_on(void);
const char *memory_name(void);                /* their first name, or "" */
/* what he remembers, as a sentence or two for the brain ("" if nothing) */
void memory_context(char *out, int cap);
/* Takes the NOTE: and NAME: lines out of an answer and keeps them. */
void memory_take_notes(char *answer);
void memory_asked(const char *question);      /* a question that was answered (the last few are kept) */
void memory_forget(void);                     /* everything, gone (and the file) */

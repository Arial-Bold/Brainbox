/* saywords.h - helping Flite say what the AI writes: sounds like "Hmm" and
 * "Ahh", slang like "gonna" and "y'know", abbreviations like "e.g." and
 * "Dr.", and a few names it would otherwise get wrong. */
#pragma once
#include <stddef.h>
#include "flite.h"

/* rewrites text (CP-1252) into what Flite should read */
void saywords_tidy(const char *in, char *out, size_t cap);
/* teaches the voice its extra words (once, after the voice is made) */
void saywords_teach(cst_voice *v);

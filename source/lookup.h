/* lookup.h - looking things up for the brain without any account or key
 * (added in 1.5): Wikipedia for facts, Google News for headlines. The
 * lookups block: call them on the AI's thread, never the screen's. */
#pragma once
#include <stdbool.h>

/* The start of the Wikipedia article that best matches query: facts for the
 * brain (title and summary). On failure, false with why filled in. */
bool wiki_lookup(const char *query, char *facts, int cap, char *why, int whycap);

/* The latest headlines (about topic, or the top stories if topic is ""), for
 * the country with this two-letter code ("" = the US): facts for the brain.
 * On failure, false with why filled in. */
bool news_lookup(const char *topic, const char *country, char *facts, int cap, char *why, int whycap);

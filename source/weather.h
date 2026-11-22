/* weather.h - where the person is, the local date and time, and the
 * weather, for the brain to answer from (added in 1.4).
 *
 * Everything comes from free services that need no account or key:
 * Open-Meteo for the weather and for finding a town, and GeoJS (or
 * ipwho.is) to guess the town from the network when Home town is Auto.
 * The lookups block: call them on the AI's thread, never the screen's. */
#pragma once
#include <stdbool.h>

/* Home town: "" for Auto (guessed from the network), or a town as typed
 * ("Detroit", "Paris, France"). Safe to call from any thread. */
void weather_set_home(const char *town);

/* The home town, found if it hasn't been yet (one try per few minutes).
 * name gets e.g. "Detroit, Michigan, United States". False if unknown. */
bool weather_home(char *name, int cap);

/* The town last found for Home town ("" if it hasn't been yet). Doesn't
 * wait or use the network: for Settings to show. */
void weather_home_known(char *name, int cap);

/* "Sunday 22 November 2026, 5:49 pm": local time at home when that's
 * known, otherwise the console's own clock. */
void weather_local_time(char *out, int cap);

/* The weather for place (NULL or "" for home): a few lines of facts for the
 * brain, in the units that place uses. On failure, false with why filled in
 * (for the brain to explain kindly). */
bool weather_report(const char *place, char *facts, int cap, char *why, int whycap);

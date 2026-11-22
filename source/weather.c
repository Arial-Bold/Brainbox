/* weather.c - see weather.h. */
#include "weather.h"
#include "json.h"
#include "net/https.h"
#include "net/netio.h"
#include "net/thr.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <stdarg.h>

/* appends to a fixed buffer, never past its end */
static void addf(char *buf, int cap, int *n, const char *fmt, ...) {
    if (*n >= cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf + *n, cap - *n, fmt, ap);
    va_end(ap);
    if (k > 0) *n = *n + k < cap - 1 ? *n + k : cap - 1;
}

typedef struct {
    bool ok;
    char name[120];      /* "Detroit, Michigan, United States" */
    char cc[4];          /* "US" */
    double lat, lon;
    bool has_offset;
    int offset;          /* seconds east of UTC, from Open-Meteo */
} Place;

static thr_mutex mu, find_mu;   /* (find_mu: one search for the home town at a time) */
static bool mu_made;
static char home_town[64];   /* as set ("" = Auto) */
static int home_gen;         /* bumped when it changes */
static char known[120];      /* the town found for it, for Settings to show ("" = not yet) */

static Place home;           /* (the AI's thread only) */
static int home_for = -1;    /* the home_gen it was found for */
static time_t home_tried;    /* when the last try failed */

static void make_locks(void) { if (!mu_made) { thr_mutex_init(&mu); thr_mutex_init(&find_mu); mu_made = true; } }
static void lock(void) { make_locks(); thr_lock(&mu); }
static void unlock(void) { thr_unlock(&mu); }

void weather_set_home(const char *town) {
    lock();
    if (strcmp(home_town, town ? town : "")) {
        snprintf(home_town, sizeof(home_town), "%s", town ? town : "");
        home_gen++;
        known[0] = 0;
    }
    unlock();
}

/* ---------------------------------------------------------------- helpers */

static void trim(char *s) {
    char *p = s;
    while (*p == ' ') p++;
    memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '.' || s[n - 1] == '?' || s[n - 1] == '!')) s[--n] = 0;
}

/* text (the font's CP-1252) for a URL: as UTF-8, %-escaped */
static void url_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 7 < cap; p++) {
        unsigned c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.') out[o++] = (char)c;
        else if (c < 0x80) o += snprintf(out + o, cap - o, "%%%02X", c);
        else {   /* (Latin-1 letters: the same as Unicode) */
            o += snprintf(out + o, cap - o, "%%%02X%%%02X", 0xC0 | (c >> 6), 0x80 | (c & 63));
        }
    }
    out[o] = 0;
}

static char *get(const char *url) {
    char *body = NULL, err[160];
    int status = https_get(url, "application/json", &body, NULL, 128 * 1024, NULL, err, sizeof(err));
    if (status != 200) { free(body); return NULL; }
    return body;
}

static void join_name(Place *pl, const char *city, const char *region, const char *country) {
    pl->name[0] = 0;
    const char *parts[3] = { city, region, country };
    for (int i = 0; i < 3; i++) {
        if (!parts[i] || !parts[i][0]) continue;
        if (i == 1 && city && !strcasecmp(parts[i], city)) continue;   /* ("Paris, Paris") */
        size_t n = strlen(pl->name);
        snprintf(pl->name + n, sizeof(pl->name) - n, "%s%s", n ? ", " : "", parts[i]);
    }
}

/* ------------------------------------------------------- finding a place */

/* A town by name ("Paris" or "Paris, France" or "Portland, Oregon"), with Open-Meteo */
static bool geocode(const char *query, Place *pl) {
    char q[96], name[96], hint[96] = "", esc[300], url[400];
    snprintf(q, sizeof(q), "%s", query);
    trim(q);
    char *comma = strchr(q, ',');
    if (comma) { *comma = 0; snprintf(hint, sizeof(hint), "%s", comma + 1); trim(hint); }
    snprintf(name, sizeof(name), "%s", q);
    trim(name);
    if (!name[0]) return false;
    url_escape(name, esc, sizeof(esc));
    snprintf(url, sizeof(url), "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=10&language=en&format=json", esc);
    char *body = get(url);
    if (!body) return false;
    int pick = -1;
    for (int i = 0; i < 10; i++) {
        char path[48], nm[80], a1[80] = "", co[80] = "", cc[8] = "";
        snprintf(path, sizeof(path), "results.%d.name", i);
        if (!json_get_string(body, path, nm, sizeof(nm))) break;
        if (!hint[0]) { pick = i; break; }   /* (the first is the best known) */
        snprintf(path, sizeof(path), "results.%d.admin1", i); json_get_string(body, path, a1, sizeof(a1));
        snprintf(path, sizeof(path), "results.%d.country", i); json_get_string(body, path, co, sizeof(co));
        snprintf(path, sizeof(path), "results.%d.country_code", i); json_get_string(body, path, cc, sizeof(cc));
        if (!strncasecmp(a1, hint, strlen(hint)) || !strncasecmp(co, hint, strlen(hint)) || !strcasecmp(cc, hint) ||
            (!strcasecmp(hint, "USA") && !strcasecmp(cc, "US")) || (!strcasecmp(hint, "UK") && !strcasecmp(cc, "GB"))) { pick = i; break; }
        if (pick < 0 && i == 0) pick = -2;   /* (nothing matched yet: the first will do in the end) */
    }
    if (pick == -2) pick = 0;
    bool ok = false;
    if (pick >= 0) {
        char path[48], nm[80] = "", a1[80] = "", co[80] = "";
        snprintf(path, sizeof(path), "results.%d.name", pick); json_get_string(body, path, nm, sizeof(nm));
        snprintf(path, sizeof(path), "results.%d.admin1", pick); json_get_string(body, path, a1, sizeof(a1));
        snprintf(path, sizeof(path), "results.%d.country", pick); json_get_string(body, path, co, sizeof(co));
        snprintf(path, sizeof(path), "results.%d.country_code", pick); json_get_string(body, path, pl->cc, sizeof(pl->cc));
        char plat[48], plon[48];
        snprintf(plat, sizeof(plat), "results.%d.latitude", pick);
        snprintf(plon, sizeof(plon), "results.%d.longitude", pick);
        ok = json_get_number(body, plat, &pl->lat) && json_get_number(body, plon, &pl->lon);
        join_name(pl, nm, a1, co);
        pl->has_offset = false;
        pl->ok = ok;
    }
    free(body);
    return ok;
}

/* The town the network is in (roughly: it's the internet provider's guess) */
static bool from_network(Place *pl) {
    char city[80] = "", region[80] = "", country[80] = "";
    char *body = get("https://get.geojs.io/v1/ip/geo.json");
    if (body && json_get_number(body, "latitude", &pl->lat) && json_get_number(body, "longitude", &pl->lon)) {
        json_get_string(body, "city", city, sizeof(city));
        json_get_string(body, "region", region, sizeof(region));
        json_get_string(body, "country", country, sizeof(country));
        json_get_string(body, "country_code", pl->cc, sizeof(pl->cc));
        free(body);
    } else {
        free(body);
        body = get("https://ipwho.is/");
        if (!body || !strstr(body, "\"success\":true") || !json_get_number(body, "latitude", &pl->lat) ||
            !json_get_number(body, "longitude", &pl->lon)) { free(body); return false; }
        json_get_string(body, "city", city, sizeof(city));
        json_get_string(body, "region", region, sizeof(region));
        json_get_string(body, "country", country, sizeof(country));
        json_get_string(body, "country_code", pl->cc, sizeof(pl->cc));
        free(body);
    }
    join_name(pl, city, region, country);
    if (!pl->name[0]) snprintf(pl->name, sizeof(pl->name), "your area");
    pl->has_offset = false;
    pl->ok = true;
    return true;
}

/* ------------------------------------------------------------ the weather */

static const char *sky(int code) {
    switch (code) {
    case 0: return "clear sky";
    case 1: return "mainly clear";
    case 2: return "partly cloudy";
    case 3: return "overcast";
    case 45: case 48: return "fog";
    case 51: return "light drizzle";
    case 53: return "drizzle";
    case 55: return "heavy drizzle";
    case 56: case 57: return "freezing drizzle";
    case 61: return "light rain";
    case 63: return "rain";
    case 65: return "heavy rain";
    case 66: case 67: return "freezing rain";
    case 71: return "light snow";
    case 73: return "snow";
    case 75: return "heavy snow";
    case 77: return "snow grains";
    case 80: return "light rain showers";
    case 81: return "rain showers";
    case 82: return "heavy rain showers";
    case 85: return "light snow showers";
    case 86: return "heavy snow showers";
    case 95: return "thunderstorms";
    case 96: case 99: return "thunderstorms with hail";
    }
    return "unsettled weather";
}

static bool fahrenheit(const char *cc) { return !strcasecmp(cc, "US") || !strcasecmp(cc, "LR") || !strcasecmp(cc, "MM") || !strcasecmp(cc, "PR") || !strcasecmp(cc, "GU"); }
static bool mph(const char *cc) { return fahrenheit(cc) || !strcasecmp(cc, "GB"); }

static int temp(double c, const char *cc) { return (int)lround(fahrenheit(cc) ? c * 9 / 5 + 32 : c); }
static int speed(double kmh, const char *cc) { return (int)lround(mph(cc) ? kmh / 1.609344 : kmh); }

/* days since 1970 for a civil date (and the weekday from it) */
static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
static const char *const WEEKDAYS[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *weekday(const char *ymd) {
    int y, m, d;
    if (sscanf(ymd, "%d-%d-%d", &y, &m, &d) != 3) return "";
    long days = days_from_civil(y, m, d);
    return WEEKDAYS[((days % 7) + 11) % 7];   /* (1970-01-01 was a Thursday) */
}

/* The forecast for pl: into facts (or just the time zone, if facts is NULL) */
static bool forecast(Place *pl, char *facts, int cap) {
    char url[600];
    if (!facts)
        snprintf(url, sizeof(url), "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=weather_code&timezone=auto&forecast_days=1",
                 pl->lat, pl->lon);
    else
        snprintf(url, sizeof(url), "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                 "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m"
                 "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
                 "&timezone=auto&forecast_days=3", pl->lat, pl->lon);
    char *body = get(url);
    if (!body) return false;
    double off;
    if (json_get_number(body, "utc_offset_seconds", &off)) { pl->offset = (int)off; pl->has_offset = true; }
    if (!facts) { free(body); return pl->has_offset; }

    const char *cc = pl->cc, *tu = fahrenheit(cc) ? "F" : "C", *su = mph(cc) ? "mph" : "km/h";
    double t, feel, hum, code, wind;
    if (!json_get_number(body, "current.temperature_2m", &t) || !json_get_number(body, "current.weather_code", &code)) { free(body); return false; }
    if (!json_get_number(body, "current.apparent_temperature", &feel)) feel = t;
    if (!json_get_number(body, "current.relative_humidity_2m", &hum)) hum = -1;
    if (!json_get_number(body, "current.wind_speed_10m", &wind)) wind = -1;
    int n = 0;
    addf(facts, cap, &n, "Weather for %s, from Open-Meteo just now. Right now: %d degrees %s",
                     pl->name, temp(t, cc), tu);
    if (fabs(feel - t) >= 2) addf(facts, cap, &n, " (feels like %d)", temp(feel, cc));
    addf(facts, cap, &n, ", %s", sky((int)code));
    if (wind >= 0) addf(facts, cap, &n, ", wind %d %s", speed(wind, cc), su);
    if (hum >= 0) addf(facts, cap, &n, ", humidity %d%%", (int)hum);
    addf(facts, cap, &n, ".");
    static const char *const label[3] = { "Today", "Tomorrow", "" };
    for (int i = 0; i < 3 && n < cap - 1; i++) {
        char path[64], day[16] = "";
        double hi, lo, dc, rain = -1;
        snprintf(path, sizeof(path), "daily.time.%d", i);
        json_get_string(body, path, day, sizeof(day));
        snprintf(path, sizeof(path), "daily.temperature_2m_max.%d", i);
        if (!json_get_number(body, path, &hi)) break;
        snprintf(path, sizeof(path), "daily.temperature_2m_min.%d", i);
        if (!json_get_number(body, path, &lo)) break;
        snprintf(path, sizeof(path), "daily.weather_code.%d", i);
        if (!json_get_number(body, path, &dc)) dc = -1;
        snprintf(path, sizeof(path), "daily.precipitation_probability_max.%d", i);
        json_get_number(body, path, &rain);
        char when[40];
        if (label[i][0]) snprintf(when, sizeof(when), "%s (%s)", label[i], weekday(day));
        else snprintf(when, sizeof(when), "%s", weekday(day));
        addf(facts, cap, &n, " %s: high %d, low %d", when, temp(hi, cc), temp(lo, cc));
        if (dc >= 0) addf(facts, cap, &n, ", %s", sky((int)dc));
        if (rain >= 0) addf(facts, cap, &n, ", %d%% chance of rain or snow", (int)rain);
        addf(facts, cap, &n, ".");
    }
    free(body);
    return true;
}

/* ------------------------------------------------------------- the calls */

static bool find_home_locked(void);
static bool find_home(void) {
    make_locks();
    thr_lock(&find_mu);
    bool ok = find_home_locked();
    thr_unlock(&find_mu);
    return ok;
}
static bool find_home_locked(void) {
    lock();
    char town[64];
    snprintf(town, sizeof(town), "%s", home_town);
    int gen = home_gen;
    unlock();
    if (home_for == gen && home.ok) return true;
    if (home_for == gen && home_tried && time(NULL) - home_tried < 300) return false;   /* (tried lately) */
    Place pl;
    memset(&pl, 0, sizeof(pl));
    bool ok = town[0] ? geocode(town, &pl) : from_network(&pl);
    if (ok) forecast(&pl, NULL, 0);   /* (its time zone, for the clock) */
    home = pl;
    home.ok = ok;
    home_for = gen;
    home_tried = ok ? 0 : time(NULL);
    lock();
    if (gen == home_gen) snprintf(known, sizeof(known), "%s", ok ? home.name : "");
    unlock();
    return ok;
}

bool weather_home(char *name, int cap) {
    if (!find_home()) { if (cap) name[0] = 0; return false; }
    snprintf(name, cap, "%s", home.name);
    return true;
}

void weather_local_time(char *out, int cap) {
    static const char *const MONTHS[12] = { "January", "February", "March", "April", "May", "June", "July",
                                            "August", "September", "October", "November", "December" };
    time_t now;
    if (home.ok && home.has_offset && netio_time_synced()) now = netio_now() + home.offset;
    else now = time(NULL);   /* (the console's clock: local time, as set on the console) */
    struct tm tm;
    gmtime_r(&now, &tm);
    int h12 = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    if (home.ok && fahrenheit(home.cc))   /* (the US way round) */
        snprintf(out, cap, "%s, %s %d, %d, %d:%02d %s", WEEKDAYS[tm.tm_wday], MONTHS[tm.tm_mon], tm.tm_mday,
                 tm.tm_year + 1900, h12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
    else
        snprintf(out, cap, "%s %d %s %d, %02d:%02d", WEEKDAYS[tm.tm_wday], tm.tm_mday, MONTHS[tm.tm_mon],
                 tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
}

bool weather_report(const char *place, char *facts, int cap, char *why, int whycap) {
    Place pl;
    memset(&pl, 0, sizeof(pl));
    if (place && place[0]) {
        if (!geocode(place, &pl)) {
            snprintf(why, whycap, "couldn't find a place called \"%s\" to look up", place);
            return false;
        }
    } else {
        if (!find_home()) {
            lock();
            bool typed = home_town[0] != 0;
            unlock();
            snprintf(why, whycap, typed ? "couldn't find the town set as Home town in Settings"
                                        : "doesn't know where the person is; they can name a town, or set Home town in Settings");
            return false;
        }
        pl = home;
    }
    if (!forecast(&pl, facts, cap)) {
        snprintf(why, whycap, "the weather service didn't answer just now");
        return false;
    }
    return true;
}

void weather_home_known(char *name, int cap) {
    lock();
    snprintf(name, cap, "%s", known);
    unlock();
}

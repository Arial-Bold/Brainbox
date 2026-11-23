/* lookup.c - see lookup.h. */
#include "lookup.h"
#include "json.h"
#include "net/https.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* text (the font's CP-1252) for a URL: as UTF-8, %-escaped */
static void url_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 7 < cap; p++) {
        unsigned c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.') out[o++] = (char)c;
        else if (c < 0x80) o += snprintf(out + o, cap - o, "%%%02X", c);
        else o += snprintf(out + o, cap - o, "%%%02X%%%02X", 0xC0 | (c >> 6), 0x80 | (c & 63));
    }
    out[o] = 0;
}

static char *get(const char *url, size_t max) {
    char *body = NULL, err[160];
    int status = https_get(url, NULL, &body, NULL, max, NULL, err, sizeof(err));
    if (status != 200) { free(body); return NULL; }
    return body;
}

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '"') p++;
    memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && strchr(" .?!\"\n", s[n - 1])) s[--n] = 0;
}

/* ------------------------------------------------------------- Wikipedia */

bool wiki_lookup(const char *query, char *facts, int cap, char *why, int whycap) {
    char q[160], esc[480], url[640];
    snprintf(q, sizeof(q), "%s", query);
    trim(q);
    if (!q[0]) { snprintf(why, whycap, "wasn't sure what to look up"); return false; }
    url_escape(q, esc, sizeof(esc));
    snprintf(url, sizeof(url), "https://en.wikipedia.org/w/rest.php/v1/search/page?q=%s&limit=3", esc);
    char *found = get(url, 64 * 1024);
    if (!found) { snprintf(why, whycap, "couldn't reach Wikipedia just now"); return false; }
    bool ok = false;
    for (int i = 0; i < 3 && !ok; i++) {
        char path[32], key[200];
        snprintf(path, sizeof(path), "pages.%d.key", i);
        if (!json_get_string(found, path, key, sizeof(key))) break;
        /* (the key back in UTF-8 for the address: it came out in CP-1252) */
        url_escape(key, esc, sizeof(esc));
        snprintf(url, sizeof(url), "https://en.wikipedia.org/api/rest_v1/page/summary/%s", esc);
        char *page = get(url, 64 * 1024);
        if (!page) continue;
        char type[32] = "", title[160] = "", desc[200] = "";
        static char extract[2000];
        json_get_string(page, "type", type, sizeof(type));
        if (strcmp(type, "disambiguation") && json_get_string(page, "extract", extract, sizeof(extract)) && extract[0]) {
            json_get_string(page, "title", title, sizeof(title));
            json_get_string(page, "description", desc, sizeof(desc));
            snprintf(facts, cap, "Wikipedia's article \"%s\"%s%s%s: %s", title, desc[0] ? " (" : "", desc, desc[0] ? ")" : "", extract);
            ok = true;
        }
        free(page);
    }
    free(found);
    if (!ok) snprintf(why, whycap, "couldn't find a Wikipedia article about %s", q);
    return ok;
}

/* ------------------------------------------------------------------ news */

/* a feed's text: CDATA unwrapped, entities and tags taken out, into CP-1252 */
static void feed_text(const char *s, const char *e, char *out, size_t cap) {
    static char raw[600];
    size_t o = 0;
    const char *cd = strstr(s, "<![CDATA[");
    if (cd && cd < e) {
        s = cd + 9;
        const char *ce = strstr(s, "]]>");
        if (ce && ce < e) e = ce;
    }
    for (const char *p = s; p < e && o + 1 < sizeof(raw); p++) {
        if (*p == '<') { while (p < e && *p != '>') p++; continue; }
        if (*p == '&') {
            static const struct { const char *ent; char ch; } ents[] = {
                { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' }, { "&#39;", '\'' }, { "&nbsp;", ' ' } };
            bool done = false;
            for (unsigned k = 0; k < sizeof(ents) / sizeof(ents[0]); k++) {
                size_t n = strlen(ents[k].ent);
                if (!strncmp(p, ents[k].ent, n)) { raw[o++] = ents[k].ch; p += n - 1; done = true; break; }
            }
            if (done) continue;
        }
        raw[o++] = *p;
    }
    raw[o] = 0;
    utf8_to_cp1252(raw, out, cap);
}

bool news_lookup(const char *topic, const char *country, char *facts, int cap, char *why, int whycap) {
    char t[120], esc[400], url[600], cc[4];
    snprintf(t, sizeof(t), "%s", topic ? topic : "");
    trim(t);
    snprintf(cc, sizeof(cc), "%s", country && country[0] ? country : "US");
    for (char *p = cc; *p; p++) *p = (char)toupper((unsigned char)*p);
    /* (English-language editions; the rest of the world gets the US one) */
    static const char *const editions[] = { "US", "GB", "CA", "AU", "IE", "NZ", "IN", "ZA", "SG", "PH", "NG", "KE" };
    bool known = false;
    for (unsigned k = 0; k < sizeof(editions) / sizeof(editions[0]); k++) if (!strcmp(cc, editions[k])) known = true;
    if (!known) snprintf(cc, sizeof(cc), "US");
    if (t[0]) {
        url_escape(t, esc, sizeof(esc));
        snprintf(url, sizeof(url), "https://news.google.com/rss/search?q=%s&hl=en-%s&gl=%s&ceid=%s:en", esc, cc, cc, cc);
    } else
        snprintf(url, sizeof(url), "https://news.google.com/rss?hl=en-%s&gl=%s&ceid=%s:en", cc, cc, cc);
    const char *source = "Google News";
    char *feed = get(url, 640 * 1024);
    if (!feed && !t[0]) { feed = get("https://feeds.bbci.co.uk/news/rss.xml", 640 * 1024); source = "BBC News"; }   /* (top stories: the BBC's, if Google's didn't come) */
    if (!feed) { snprintf(why, whycap, "couldn't reach the news just now"); return false; }
    int n = snprintf(facts, cap, "The latest headlines%s%s%s, from %s just now:", t[0] ? " about \"" : "", t, t[0] ? "\"" : "", source);
    int count = 0;
    const char *p = feed;
    while (count < 8 && (p = strstr(p, "<item")) != NULL) {
        const char *end = strstr(p, "</item>");
        if (!end) break;
        const char *ts = strstr(p, "<title>");
        if (ts && ts < end) {
            ts += 7;
            const char *te = strstr(ts, "</title>");
            if (te && te < end) {
                char title[300];
                feed_text(ts, te, title, sizeof(title));
                if (title[0] && n < cap - 1) {
                    int k = snprintf(facts + n, cap - n, " %d. %s.", count + 1, title);
                    if (k > 0) n = n + k < cap - 1 ? n + k : cap - 1;
                    count++;
                }
            }
        }
        p = end + 7;
    }
    free(feed);
    if (!count) {
        snprintf(why, whycap, t[0] ? "couldn't find any news about %s" : "couldn't read the news just now", t);
        return false;
    }
    return true;
}

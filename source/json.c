/* json.c - see json.h. */
#include "json.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------- building */

static void grow(Buf *b, size_t more) {
    if (b->failed || b->len + more + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap : 1024;
    while (nc < b->len + more + 1) nc *= 2;
    char *n = realloc(b->s, nc);
    if (!n) { b->failed = true; return; }
    b->s = n;
    b->cap = nc;
}

void buf_add(Buf *b, const char *s) {
    size_t n = strlen(s);
    grow(b, n);
    if (b->failed) return;
    memcpy(b->s + b->len, s, n + 1);
    b->len += n;
}

void buf_add_mem(Buf *b, const void *data, size_t n) {
    grow(b, n);
    if (b->failed) return;
    memcpy(b->s + b->len, data, n);
    b->len += n;
    b->s[b->len] = 0;
}

void buf_add_base64(Buf *b, const void *data, size_t n) {
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    grow(b, (n + 2) / 3 * 4);
    if (b->failed) return;
    const unsigned char *p = data;
    char *o = b->s + b->len;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        unsigned v = p[i] << 16 | p[i + 1] << 8 | p[i + 2];
        *o++ = A[v >> 18]; *o++ = A[(v >> 12) & 63]; *o++ = A[(v >> 6) & 63]; *o++ = A[v & 63];
    }
    if (i < n) {
        unsigned v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0);
        *o++ = A[v >> 18]; *o++ = A[(v >> 12) & 63];
        *o++ = i + 1 < n ? A[(v >> 6) & 63] : '=';
        *o++ = '=';
    }
    *o = 0;
    b->len = (size_t)(o - b->s);
}

void buf_addf(Buf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    buf_add(b, tmp);
}

/* CP-1252's 0x80-0x9F, as Unicode */
static const unsigned short cp1252_hi[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };

void buf_add_json_string(Buf *b, const char *text) {
    buf_add(b, "\"");
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        char tmp[8];
        unsigned c = *p;
        if (c == '"' || c == '\\') { tmp[0] = '\\'; tmp[1] = (char)c; tmp[2] = 0; }
        else if (c == '\n') strcpy(tmp, "\\n");
        else if (c < 0x20) snprintf(tmp, sizeof(tmp), "\\u%04x", c);
        else if (c < 0x80) { tmp[0] = (char)c; tmp[1] = 0; }
        else {
            unsigned u = (c >= 0x80 && c < 0xA0) ? cp1252_hi[c - 0x80] : c;
            if (!u) u = '?';
            if (u < 0x800) { tmp[0] = (char)(0xC0 | (u >> 6)); tmp[1] = (char)(0x80 | (u & 63)); tmp[2] = 0; }
            else { tmp[0] = (char)(0xE0 | (u >> 12)); tmp[1] = (char)(0x80 | ((u >> 6) & 63)); tmp[2] = (char)(0x80 | (u & 63)); tmp[3] = 0; }
        }
        buf_add(b, tmp);
    }
    buf_add(b, "\"");
}

void buf_free(Buf *b) { free(b->s); memset(b, 0, sizeof(*b)); }

/* -------------------------------------------------------------- reading */

static const char *ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }

/* past one value (string, number, object, array, true/false/null); NULL if broken */
static const char *skip(const char *p) {
    p = ws(p);
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++;
        return *p ? p + 1 : NULL;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = open == '{' ? '}' : ']';
        p = ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (open == '{') {
                p = skip(p);   /* the key */
                if (!p) return NULL;
                p = ws(p);
                if (*p != ':') return NULL;
                p++;
            }
            p = skip(p);
            if (!p) return NULL;
            p = ws(p);
            if (*p == ',') { p++; continue; }
            if (*p == close) return p + 1;
            return NULL;
        }
    }
    if (!*p) return NULL;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return p;
}

/* the value for one step of a path, inside the value at p */
static const char *step(const char *p, const char *key, size_t klen) {
    p = ws(p);
    if (*p == '[') {
        int want = atoi(key), i = 0;
        p = ws(p + 1);
        if (*p == ']') return NULL;
        for (;;) {
            if (i == want) return ws(p);
            p = skip(p);
            if (!p) return NULL;
            p = ws(p);
            if (*p != ',') return NULL;
            p = ws(p + 1);
            i++;
        }
    }
    if (*p != '{') return NULL;
    p = ws(p + 1);
    if (*p == '}') return NULL;
    for (;;) {
        if (*p != '"') return NULL;
        const char *k = p + 1;
        const char *ke = skip(p);
        if (!ke) return NULL;
        bool match = (size_t)(ke - 1 - k) == klen && !memcmp(k, key, klen);
        p = ws(ke);
        if (*p != ':') return NULL;
        p = ws(p + 1);
        if (match) return p;
        p = skip(p);
        if (!p) return NULL;
        p = ws(p);
        if (*p != ',') return NULL;
        p = ws(p + 1);
    }
}

/* Unicode to the font's characters (CP-1252), with fallbacks */
static int to_cp1252(unsigned u) {
    if (u < 0x80 || (u >= 0xA0 && u < 0x100)) return (int)u;
    for (int i = 0; i < 32; i++) if (cp1252_hi[i] == u) return 0x80 + i;
    switch (u) {
    case 0x2010: case 0x2011: case 0x2012: case 0x2212: return '-';
    case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: return ' ';
    case 0x2032: return '\'';
    case 0x2033: return '"';
    }
    return u >= 0x1F000 ? -1 : '?';   /* emoji and the like are dropped */
}

bool json_get_string(const char *doc, const char *path, char *out, size_t cap) {
    if (!doc || !cap) return false;
    out[0] = 0;
    const char *p = doc;
    while (*path) {
        size_t n = strcspn(path, ".");
        p = step(p, path, n);
        if (!p) return false;
        path += n;
        if (*path == '.') path++;
    }
    p = ws(p);
    if (*p != '"') return false;
    size_t o = 0;
    for (p++; *p && *p != '"' && o + 1 < cap; p++) {
        unsigned u;
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': u = '\n'; break;
            case 't': u = ' '; break;
            case 'r': continue;
            case 'b': case 'f': continue;
            case 'u': {
                char hex[5] = { 0 };
                for (int i = 0; i < 4 && p[1]; i++) hex[i] = *++p;
                u = (unsigned)strtol(hex, NULL, 16);
                if (u >= 0xD800 && u < 0xDC00 && p[1] == '\\' && p[2] == 'u') {   /* a surrogate pair */
                    char lo[5] = { 0 };
                    for (int i = 0; i < 4 && p[3 + i]; i++) lo[i] = p[3 + i];
                    unsigned l = (unsigned)strtol(lo, NULL, 16);
                    u = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00);
                    p += 6;
                }
                break;
            }
            default: u = (unsigned char)*p;
            }
        } else {
            unsigned c = (unsigned char)*p;
            if (c < 0x80) u = c;
            else if ((c & 0xE0) == 0xC0 && p[1]) { u = ((c & 31) << 6) | (p[1] & 63); p += 1; }
            else if ((c & 0xF0) == 0xE0 && p[1] && p[2]) { u = ((c & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 2; }
            else if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
                u = ((c & 7) << 18) | ((p[1] & 63) << 12) | ((p[2] & 63) << 6) | (p[3] & 63);
                p += 3;
            } else u = '?';
        }
        int ch = to_cp1252(u);
        if (ch >= 0) out[o++] = (char)ch;
    }
    out[o] = 0;
    return true;
}

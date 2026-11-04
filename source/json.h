/* json.h - just enough JSON for the AI services: building a request
 * (escaping text) and finding one string in an answer by its path. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct { char *s; size_t len, cap; bool failed; } Buf;   /* a growing string */
void buf_add(Buf *b, const char *s);
void buf_addf(Buf *b, const char *fmt, ...);
void buf_add_json_string(Buf *b, const char *text);   /* "text", escaped (text is CP-1252, sent as UTF-8) */
void buf_free(Buf *b);

/* The string at path in doc ("candidates.0.content.parts.0.text": object
 * keys and array positions), unescaped and turned into the font's characters
 * (CP-1252). False if it isn't there or isn't a string. */
bool json_get_string(const char *doc, const char *path, char *out, size_t cap);

/* https.h - a small HTTPS client: GET a page into memory or a file
 * (following redirects), or POST a JSON body and read the answer, with
 * Content-Length or chunked bodies. GameCube only. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { volatile uint32_t done, total; } HttpsProgress;   /* total 0 = unknown */

/* Returns the final HTTP status (body in *body, malloc'd and NUL-terminated,
 * at most max bytes), or -1 with err filled in. */
int https_get(const char *url, const char *accept, char **body, size_t *len, size_t max, volatile int *cancel,
              char *err, size_t errlen);

/* Saves the page at url to path (written as path.part, then renamed). */
bool https_download(const char *url, const char *path, HttpsProgress *pg, volatile int *cancel, char *err,
                    size_t errlen);

/* POSTs body (JSON) to url. headers: extra header lines, each ending in
 * \r\n (or NULL). Returns the HTTP status (the answer in *resp, malloc'd and
 * NUL-terminated, at most max bytes), or -1 with err filled in. */
int https_post(const char *url, const char *headers, const char *body, size_t body_len, char **resp, size_t *resp_len,
               size_t max, volatile int *cancel, char *err, size_t errlen);

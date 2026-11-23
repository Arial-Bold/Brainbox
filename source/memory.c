/* memory.c - see memory.h. */
#include "memory.h"
#include "net/storage.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define NOTES 20      /* things they've told him (the oldest go first) */
#define NOTE_MAX 120
#define RECENT 3      /* their last few questions */
#define RECENT_MAX 90

static bool on = true;
static char name[40];
static char notes[NOTES][NOTE_MAX];
static int nnotes;
static char recent[RECENT][RECENT_MAX];
static int nrecent;

static void file_path(char *out, size_t cap) { storage_path(out, cap, "memory.txt"); }

static void save(void) {
    if (!storage_ok()) return;
    static char out[NOTES * (NOTE_MAX + 8) + RECENT * (RECENT_MAX + 10) + 200];
    int n = snprintf(out, sizeof(out), "# What BrainBox remembers about you (delete this file, or ask him to forget, to clear it)\n");
    if (name[0]) n += snprintf(out + n, sizeof(out) - n, "name=%s\n", name);
    for (int i = 0; i < nnotes && n < (int)sizeof(out); i++) n += snprintf(out + n, sizeof(out) - n, "note=%s\n", notes[i]);
    for (int i = 0; i < nrecent && n < (int)sizeof(out); i++) n += snprintf(out + n, sizeof(out) - n, "recent=%s\n", recent[i]);
    if (n > (int)sizeof(out)) n = (int)sizeof(out);
    char path[300];
    file_path(path, sizeof(path));
    storage_write_atomic(path, out, (size_t)n);
}

static void clean(char *s) {
    for (char *p = s; *p; p++) if (*p == '\n' || *p == '\r' || *p == '=') *p = ' ';
    char *p = s;
    while (*p == ' ' || *p == '-' || *p == ':') p++;
    memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '.')) s[--n] = 0;
}

void memory_load(void) {
    nnotes = nrecent = 0;
    name[0] = 0;
    if (!storage_ok()) return;
    char path[300];
    file_path(path, sizeof(path));
    size_t len = 0;
    char *data = storage_is_file(path) ? storage_read(path, &len) : NULL;
    if (!data) return;
    char *save_p = NULL;
    for (char *line = strtok_r(data, "\n", &save_p); line; line = strtok_r(NULL, "\n", &save_p)) {
        char *cr = strchr(line, '\r');
        if (cr) *cr = 0;
        if (!strncmp(line, "name=", 5)) snprintf(name, sizeof(name), "%s", line + 5);
        else if (!strncmp(line, "note=", 5) && nnotes < NOTES) snprintf(notes[nnotes++], NOTE_MAX, "%s", line + 5);
        else if (!strncmp(line, "recent=", 7) && nrecent < RECENT) snprintf(recent[nrecent++], RECENT_MAX, "%s", line + 7);
    }
    free(data);
}

void memory_enable(bool o) { on = o; }
bool memory_on(void) { return on; }
const char *memory_name(void) { return on ? name : ""; }

void memory_context(char *out, int cap) {
    out[0] = 0;
    if (!on || (!name[0] && !nnotes && !nrecent)) return;
    int n = snprintf(out, cap, " You remember this about the person from earlier conversations (bring it up only when it fits, never as a list):");
    if (name[0] && n < cap) n += snprintf(out + n, cap - n, " their name is %s.", name);
    for (int i = 0; i < nnotes && n < cap; i++) n += snprintf(out + n, cap - n, " %s.", notes[i]);
    if (nrecent && n < cap) {
        n += snprintf(out + n, cap - n, " Lately they asked about:");
        for (int i = 0; i < nrecent && n < cap; i++) n += snprintf(out + n, cap - n, "%s \"%s\"", i ? ";" : "", recent[i]);
        if (n < cap) snprintf(out + n, cap - n, ".");
    }
}

static bool add_note(const char *s) {
    char t[NOTE_MAX];
    snprintf(t, sizeof(t), "%s", s);
    clean(t);
    if (strlen(t) < 3) return false;
    for (int i = 0; i < nnotes; i++) if (!strcasecmp(notes[i], t)) return false;   /* (already known) */
    if (nnotes == NOTES) { memmove(notes, notes + 1, sizeof(notes[0]) * (NOTES - 1)); nnotes--; }
    snprintf(notes[nnotes++], NOTE_MAX, "%s", t);
    return true;
}

void memory_take_notes(char *answer) {
    bool changed = false;
    for (;;) {
        char *a = strstr(answer, "NOTE:"), *b = strstr(answer, "NAME:");
        char *m = a && (!b || a < b) ? a : b;
        if (!m) break;
        bool is_name = m == b;
        char *end = strchr(m, '\n');
        size_t len = end ? (size_t)(end - m) : strlen(m);
        char item[NOTE_MAX + 8];
        size_t k = len - 5 < sizeof(item) - 1 ? len - 5 : sizeof(item) - 1;
        memcpy(item, m + 5, k);
        item[k] = 0;
        if (on) {
            if (is_name) {
                clean(item);
                char *sp = strchr(item, ' ');   /* (just the first name) */
                if (sp) *sp = 0;
                if (item[0] && strcasecmp(item, name)) { snprintf(name, sizeof(name), "%s", item); changed = true; }
            } else if (add_note(item)) changed = true;
        }
        /* out of the answer (with the line break before it) */
        char *from = m;
        while (from > answer && (from[-1] == '\n' || from[-1] == ' ')) from--;
        memmove(from, end ? end : m + len, strlen(end ? end : m + len) + 1);
    }
    size_t n = strlen(answer);
    while (n && (answer[n - 1] == '\n' || answer[n - 1] == ' ')) answer[--n] = 0;
    if (changed) save();
}

void memory_asked(const char *q) {
    if (!on || !q || !q[0]) return;
    char t[RECENT_MAX];
    snprintf(t, sizeof(t), "%s", q);
    clean(t);
    for (char *p = t; *p; p++) if (*p == '"') *p = '\'';
    if (nrecent == RECENT) { memmove(recent, recent + 1, sizeof(recent[0]) * (RECENT - 1)); nrecent--; }
    snprintf(recent[nrecent++], RECENT_MAX, "%s", t);
    save();
}

void memory_forget(void) {
    name[0] = 0;
    nnotes = nrecent = 0;
    if (!storage_ok()) return;
    char path[300];
    file_path(path, sizeof(path));
    remove(path);
}

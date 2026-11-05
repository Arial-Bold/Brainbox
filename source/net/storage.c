/* storage.c - see storage.h. */
#include "storage.h"
#include <fat.h>
#include <sdcard/gcsd.h>
#include <ogc/dvd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static bool mounted;
static char app_dir[256] = "sd:/apps/brainbox";
static char program[300];

bool storage_is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
bool storage_is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && !S_ISDIR(st.st_mode); }

bool storage_init(void) {
    DISC_INTERFACE *devs[] = { &__io_gcsd2, &__io_gcsdb, &__io_gcsda, &__io_gcode };
    for (unsigned i = 0; i < sizeof(devs) / sizeof(devs[0]) && !mounted; i++)
        if (fatMountSimple("sd", devs[i])) mounted = true;
    if (mounted) {
        mkdir("sd:/apps", 0777);
        mkdir(app_dir, 0777);
    }
    return mounted;
}

/* argv[0] from the loader: "sd:/apps/brainbox/default.dol", or another device
 * name for the same card ("sdb:/...", "fat:/..."): the part after ":/" is
 * what's kept */
void storage_set_program(const char *argv0) {
    if (!argv0 || !*argv0) return;
    const char *p = strstr(argv0, ":/");
    if (!p) return;
    snprintf(program, sizeof(program), "sd:/%s", p + 2);
    char *slash = strrchr(program, '/');
    if (slash && slash > program + 3) {
        size_t n = (size_t)(slash - program);
        if (n < sizeof(app_dir)) { memcpy(app_dir, program, n); app_dir[n] = 0; }
    }
}

bool storage_ok(void) { return mounted; }
const char *storage_root(void) { return "sd:/"; }
const char *storage_app_dir(void) { return app_dir; }
const char *storage_program_file(void) { return program[0] && storage_is_file(program) ? program : NULL; }
void storage_path(char *out, size_t cap, const char *name) { snprintf(out, cap, "%s/%s", app_dir, name); }

char *storage_read(const char *path, size_t *len_out) {
    if (len_out) *len_out = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 4 * 1024 * 1024) { fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = 0;
    if (len_out) *len_out = got;
    return b;
}

bool storage_write_atomic(const char *path, const void *data, size_t len) {
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = fflush(f) == 0 && ok;
    fsync(fileno(f));
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(tmp); return false; }
    remove(path);
    return rename(tmp, path) == 0;
}

char *storage_read_other_cacert(size_t *len) {
    static const char *others[] = { "sd:/apps/gcmm/cacert.pem", "sd:/apps/cubebrowser/gcbrowser/cacert.pem",
                                    "sd:/apps/flashcube/cacert.pem", "sd:/apps/cubeverse/cacert.pem", "sd:/cacert.pem" };
    for (unsigned i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        char *p = storage_read(others[i], len);
        if (p) return p;
    }
    return NULL;
}

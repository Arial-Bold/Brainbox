/* voicefetch.c - gets a hologram's voice file from GitHub when it isn't
 * beside the app (or won't load).
 *
 * The voices are files beside the app, not part of it. An update made from
 * inside an older version only brings the new program, so the voice files
 * can be missing afterwards: this fetches the one that's needed from the
 * project on GitHub, the same place updates come from. */
#include "voicefetch.h"
#include "net/https.h"
#include "net/net.h"
#include "net/netio.h"
#include "net/thr.h"
#include "net/update.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static volatile int state = VF_IDLE;
static HttpsProgress pg;
static volatile int cancel_flag;
static char url[300], path[300], err[200];

static void *worker(void *arg) {
    (void)arg;
    net_start();
    for (int i = 0; i < 600 && net_state() == NET_STARTING; i++) thr_sleep_ms(50);
    if (net_state() != NET_UP) {
        snprintf(err, sizeof(err), "No network: %s", net_message());
        state = VF_FAILED;
        return NULL;
    }
    if (!netio_time_synced()) netio_sync_clock();   /* (certificates are checked against the date) */
    remove(path);   /* (a broken copy, if there was one) */
    state = https_download(url, path, &pg, &cancel_flag, err, sizeof(err)) ? VF_DONE : VF_FAILED;
    return NULL;
}

void voicefetch_start(const char *name, const char *to) {
    if (state == VF_WORKING) return;
    snprintf(url, sizeof(url), "https://raw.githubusercontent.com/%s/HEAD/dist/%s", UPDATE_REPO, name);
    snprintf(path, sizeof(path), "%s", to);
    err[0] = 0;
    pg.done = pg.total = 0;
    cancel_flag = 0;
    state = VF_WORKING;
    thr_t t;
    if (thr_create(&t, worker, NULL, 64 * 1024, 60)) thr_detach(t);
    else { snprintf(err, sizeof(err), "Couldn't start the download."); state = VF_FAILED; }
}

int voicefetch_state(void) { return state; }
int voicefetch_percent(void) { return pg.total ? (int)((uint64_t)pg.done * 100 / pg.total) : -1; }
const char *voicefetch_error(void) { return err; }

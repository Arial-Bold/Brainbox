/* keyweb.h - the sign-in page: a tiny web server on the home network. The
 * phone (from the QR code) or computer opens it, follows the link to make a
 * key on the AI company's own site, pastes the key and presses Send; the
 * key comes to the GameCube. Runs on a thread of its own. */
#pragma once
#include <stdbool.h>
#include "ai.h"

void keyweb_open(AiBrain b);           /* starts the network and the server (once); the page is for b */
void keyweb_close(void);               /* keys are refused until keyweb_open again */
bool keyweb_ready(void);               /* answering */
const char *keyweb_url(void);          /* "http://192.168.1.42/" once ready */
const char *keyweb_message(void);      /* the network's state, or a problem */
bool keyweb_take(char *key, int cap);  /* a key that arrived (once) */

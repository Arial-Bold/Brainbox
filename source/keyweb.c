/* keyweb.c - see keyweb.h. The server is GCMM exFAT's Network transfer one. */
#include "keyweb.h"
#include "net/net.h"
#include "net/thr.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <network.h>

static thr_t thread;
static bool started;
static volatile bool accepting, ready;
static volatile AiBrain for_brain;
static char url[64], msg[128] = "Starting the network\x85";
static char got_key[200];
static volatile bool have_key;

static int wait_fd(int fd, bool wr, int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    int r = net_select(fd + 1, wr ? NULL : &set, wr ? &set : NULL, NULL, &tv);
    return r < 0 ? -1 : r > 0;
}

static bool send_all(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len > 0) {
        if (wait_fd(fd, true, 15000) != 1) return false;
        int n = net_send(fd, p, len > 8192 ? 8192 : (int)len, 0);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static void send_page(int fd, int code, const char *body) {
    char h[256];
    int n = snprintf(h, sizeof(h), "HTTP/1.0 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n"
                     "Cache-Control: no-cache\r\nContent-Length: %u\r\n\r\n", code, code == 200 ? "OK" : "Error", (unsigned)strlen(body));
    send_all(fd, h, (size_t)n);
    send_all(fd, body, strlen(body));
}

static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else if (*s == '+') *o++ = ' ';
        else *o++ = *s;
    }
    *o = 0;
}

static const char *STYLE =
    "<meta name=viewport content='width=device-width,initial-scale=1'><style>"
    "body{margin:0;background:#0b0b1e;color:#eef1f6;font:16px/1.45 -apple-system,Segoe UI,Roboto,sans-serif}"
    "main{max-width:520px;margin:0 auto;padding:24px 18px}h1{font-size:22px;margin:0 0 4px;color:#9fdcff}"
    "p{color:#c9d2ee}.card{background:#2a228c;border:1px solid #afafff66;border-radius:12px;padding:16px;margin:16px 0}"
    "a{color:#ffe278}input{width:100%;box-sizing:border-box;padding:12px;border-radius:8px;border:1px solid #afafff;"
    "background:#14102e;color:#fff;font:15px monospace}button{margin-top:12px;width:100%;padding:12px;border:0;"
    "border-radius:8px;background:#e8cf4a;color:#281e08;font-weight:700;font-size:16px}</style>";

static void page(int fd, const char *note) {
    AiBrain b = for_brain;
    static char body[3000];
    snprintf(body, sizeof(body),
             "<!doctype html><html><head><title>Professor Pixel: sign in</title>%s</head><body><main>"
             "<h1>Lend Professor Pixel a brain</h1><p>He'll use <b>%s</b>.</p>%s"
             "<div class=card><b>1.</b> Make an API key at <a href='%s' target=_blank>%s</a> "
             "(sign in there with your own account).</div>"
             "<div class=card><b>2.</b> Paste the key here and press Send.<form method=post action='/key'>"
             "<input name=key autocomplete=off autocapitalize=off spellcheck=false placeholder='Your API key'>"
             "<button>Send to the GameCube</button></form></div>"
             "<p>The key is kept on your GameCube's SD card and only ever sent to %s.</p></main></body></html>",
             STYLE, ai_name(b), note ? note : "", ai_key_page(b), ai_key_page(b), ai_name(b));
    send_page(fd, 200, body);
}

static void handle(int fd) {
    enum { CAP = 4096 };
    char *req = malloc(CAP + 1);
    if (!req) return;
    int len = 0;
    char *end = NULL;
    long clen = 0;
    while (len < CAP) {
        if (wait_fd(fd, false, 10000) != 1) break;
        int n = net_recv(fd, req + len, CAP - len, 0);
        if (n <= 0) break;
        len += n;
        req[len] = 0;
        if (!end && (end = strstr(req, "\r\n\r\n")) != NULL) {
            for (char *h = strstr(req, "\r\n"); h && h < end; h = strstr(h + 2, "\r\n"))
                if (!strncasecmp(h + 2, "Content-Length:", 15)) clen = atol(h + 17);
        }
        if (end && (long)(len - (end + 4 - req)) >= clen) break;
    }
    if (!end) { free(req); return; }
    char method[8] = "", target[200] = "";
    sscanf(req, "%7s %199s", method, target);
    if (!strcmp(method, "POST") && !strcmp(target, "/key")) {
        char *body = end + 4, k[200] = "";
        char *p = strstr(body, "key=");
        if (p) {
            snprintf(k, sizeof(k), "%s", p + 4);
            char *amp = strchr(k, '&');
            if (amp) *amp = 0;
            url_decode(k);
        }
        /* keep only the characters keys are made of */
        char clean[200];
        int o = 0;
        for (char *q = k; *q && o < (int)sizeof(clean) - 1; q++)
            if (isalnum((unsigned char)*q) || *q == '-' || *q == '_' || *q == '.') clean[o++] = *q;
        clean[o] = 0;
        if (!accepting) page(fd, "<div class=card>Professor Pixel isn't waiting for a key right now.</div>");
        else if (o < 20) page(fd, "<div class=card>That doesn't look like a whole key: please paste it again.</div>");
        else {
            snprintf(got_key, sizeof(got_key), "%s", clean);
            have_key = true;
            page(fd, "<div class=card><b>Thank you!</b> The key is on its way to the GameCube.</div>");
        }
    } else {
        page(fd, NULL);
    }
    free(req);
}

static int open_server(int port) {
    int s = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u16)port);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (net_bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0 || net_listen(s, 4) < 0) { net_close(s); return -1; }
    return s;
}

static void *server_main(void *arg) {
    (void)arg;
    net_start();
    while (net_state() == NET_STARTING) thr_sleep_ms(100);
    if (net_state() != NET_UP) {
        snprintf(msg, sizeof(msg), "%s", net_message()[0] ? net_message() : "No network. Is the network adapter connected?");
        return NULL;
    }
    int port = 80, s = open_server(port);
    if (s < 0) { port = 8080; s = open_server(port); }
    if (s < 0) { snprintf(msg, sizeof(msg), "Couldn't start the sign-in page."); return NULL; }
    if (port == 80) snprintf(url, sizeof(url), "http://%s/", net_ip());
    else snprintf(url, sizeof(url), "http://%s:%d/", net_ip(), port);
    snprintf(msg, sizeof(msg), "Waiting for your key\x85");
    ready = true;
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int c = net_accept(s, (struct sockaddr *)&from, &fl);
        if (c < 0) { thr_sleep_ms(50); continue; }
        handle(c);
        net_close(c);
    }
    return NULL;
}

void keyweb_open(AiBrain b) {
    for_brain = b;
    accepting = true;
    have_key = false;
    if (started) return;
    started = true;
    if (!thr_create(&thread, server_main, NULL, 64 * 1024, 50)) snprintf(msg, sizeof(msg), "Couldn't start the sign-in page.");
}
void keyweb_close(void) { accepting = false; }
bool keyweb_ready(void) { return ready; }
const char *keyweb_url(void) { return url; }
const char *keyweb_message(void) { return msg; }
bool keyweb_take(char *k, int cap) {
    if (!have_key) return false;
    have_key = false;
    snprintf(k, (size_t)cap, "%s", got_key);
    return true;
}

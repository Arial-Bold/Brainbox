/* ai.c - see ai.h. */
#include "ai.h"
#include "json.h"
#include "net/https.h"
#include "net/net.h"
#include "net/netio.h"
#include "net/thr.h"
#include "net/storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ANSWER_MAX 4000
#define TURNS 12                     /* questions and answers kept for context */

static const char *SYSTEM =
    "You are Professor Scotty, a kindly, cheerful, slightly absent-minded old professor who lives as a "
    "hologram inside a Nintendo GameCube. Your replies are read aloud by a simple speech synthesizer and "
    "shown in a small speech bubble, so: answer in one to four short sentences of plain spoken English; "
    "no lists, headings, markdown (no asterisks or underscores, not even around titles), emoji or web addresses; write numbers and symbols the way they are "
    "said aloud when that is clearer. Be warm, curious and encouraging, with an old professor's gentle "
    "humor; now and then begin with 'Ah!' or 'Hmm.' You're a Scot: now and then a light Scottish word like 'aye', 'wee' or 'och' fits, but don't overdo it. If a question needs a long answer, give the heart of "
    "it and offer to say more. You are an AI character, not a real person.";

typedef struct { bool user; char *text; } Turn;
static Turn turns[TURNS];
static int nturns;

static AiBrain brain = AI_GEMINI;
static char key[200], model[80];
static volatile AiState state;
static char answer[ANSWER_MAX], question[1200];
static thr_t th;
static bool testing;

static const char *names[AI_COUNT] = { "Google Gemini", "Claude", "ChatGPT", "Groq" };
static const char *ids[AI_COUNT] = { "gemini", "claude", "openai", "groq" };
static const char *pages[AI_COUNT] = { "https://aistudio.google.com/apikey", "https://console.anthropic.com/settings/keys",
                                       "https://platform.openai.com/api-keys", "https://console.groq.com/keys" };
static const char *models[AI_COUNT] = { "gemini-flash-latest", "claude-haiku-5-5", "gpt-5-mini", "llama-3.3-70b-versatile" };

const char *ai_name(AiBrain b) { return b < AI_COUNT ? names[b] : ""; }
const char *ai_key_page(AiBrain b) { return b < AI_COUNT ? pages[b] : ""; }
const char *ai_id(AiBrain b) { return b < AI_COUNT ? ids[b] : ""; }
const char *ai_default_model(AiBrain b) { return b < AI_COUNT ? models[b] : ""; }
AiBrain ai_from_id(const char *id) {
    for (int i = 0; i < AI_COUNT; i++) if (id && !strcasecmp(id, ids[i])) return (AiBrain)i;
    return AI_COUNT;
}

void ai_set(AiBrain b, const char *k, const char *m) {
    brain = b < AI_COUNT ? b : AI_GEMINI;
    snprintf(key, sizeof(key), "%s", k ? k : "");
    snprintf(model, sizeof(model), "%s", m && *m ? m : models[brain]);
}

static void forget(void) {
    for (int i = 0; i < nturns; i++) free(turns[i].text);
    nturns = 0;
}
void ai_forget(void) { if (state != AI_BUSY) forget(); }

static void remember(bool user, const char *text) {
    if (nturns == TURNS) {   /* the oldest question and answer go */
        free(turns[0].text);
        free(turns[1].text);
        memmove(turns, turns + 2, sizeof(Turn) * (TURNS - 2));
        nturns -= 2;
    }
    turns[nturns].user = user;
    turns[nturns].text = strdup(text);
    if (turns[nturns].text) nturns++;
}

/* --------------------------------------------------------- the requests */

static void messages(Buf *b, const char *now, bool gemini) {
    int from = testing ? nturns : 0;
    for (int i = from; i <= nturns; i++) {
        bool user = i == nturns ? true : turns[i].user;
        const char *text = i == nturns ? now : turns[i].text;
        if (b->len && b->s[b->len - 1] != '[') buf_add(b, ",");
        if (gemini) {
            buf_addf(b, "{\"role\":\"%s\",\"parts\":[{\"text\":", user ? "user" : "model");
            buf_add_json_string(b, text);
            buf_add(b, "}]}");
        } else {
            buf_addf(b, "{\"role\":\"%s\",\"content\":", user ? "user" : "assistant");
            buf_add_json_string(b, text);
            buf_add(b, "}");
        }
    }
}

/* Gemini: answer straight away, without "thinking" first (much quicker, and
 * his answers are short). Turned off for good if a model won't take it. */
static bool gemini_quick = true;
static const char *QUICK = ",\"thinkingConfig\":{\"thinkingBudget\":0}";
static bool thinking_refused(int status, const char *msg) {
    return status == 400 && (strstr(msg, "hinking") || strstr(msg, "thinking_budget") || strstr(msg, "budget"));
}

/* The request for one model: url, headers and body; where the answer is */
static const char *build(Buf *b_, char *url, size_t ucap, char *headers, size_t hcap, const char *use_model) {
    Buf b = { 0 };
    const char *path;
    switch (brain) {
    case AI_GEMINI:
        snprintf(url, ucap, "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent", use_model);
        snprintf(headers, hcap, "x-goog-api-key: %s\r\n", key);
        buf_add(&b, "{\"systemInstruction\":{\"parts\":[{\"text\":");
        buf_add_json_string(&b, SYSTEM);
        buf_add(&b, "}]},\"contents\":[");
        messages(&b, question, true);
        buf_addf(&b, "],\"generationConfig\":{\"maxOutputTokens\":2048%s}}", gemini_quick ? QUICK : "");
        path = "candidates.0.content.parts.0.text";
        break;
    case AI_CLAUDE:
        snprintf(url, ucap, "https://api.anthropic.com/v1/messages");
        snprintf(headers, hcap, "x-api-key: %s\r\nanthropic-version: 2023-06-01\r\n", key);
        buf_addf(&b, "{\"model\":\"%s\",\"max_tokens\":600,\"system\":", use_model);
        buf_add_json_string(&b, SYSTEM);
        buf_add(&b, ",\"messages\":[");
        messages(&b, question, false);
        buf_add(&b, "]}");
        path = "content.0.text";
        break;
    default:   /* ChatGPT, and Groq (which works the same way) */
        snprintf(url, ucap, brain == AI_GROQ ? "https://api.groq.com/openai/v1/chat/completions" : "https://api.openai.com/v1/chat/completions");
        snprintf(headers, hcap, "Authorization: Bearer %s\r\n", key);
        buf_addf(&b, "{\"model\":\"%s\",\"max_completion_tokens\":2048,\"messages\":[{\"role\":\"system\",\"content\":", use_model);
        buf_add_json_string(&b, SYSTEM);
        buf_add(&b, "}");
        messages(&b, question, false);   /* (each with a comma before it) */
        buf_add(&b, "]}");
        path = "choices.0.message.content";
        break;
    }
    *b_ = b;
    return path;
}

/* the models to try, in turn: the one chosen, then others in case it has been renamed or retired */
static int candidates(const char **out) {
    static const char *more[AI_COUNT][3] = {
        { "gemini-2.5-flash", "gemini-3.1-flash-lite", "gemini-2.5-flash-lite" },
        { "claude-sonnet-5-5", "claude-haiku-4-5", "claude-sonnet-4-5" },
        { "gpt-4.1-mini", "gpt-4o-mini", "gpt-5-nano" },
        { "openai/gpt-oss-120b", "llama-3.1-8b-instant", "openai/gpt-oss-20b" } };
    int n = 0;
    out[n++] = model;
    for (int i = 0; i < 3; i++) if (strcmp(more[brain][i], model)) out[n++] = more[brain][i];
    return n;
}

/* keeps the last problem's answer from the service on the SD card (problem.txt beside the app) */
static void note_problem(const char *use_model, int status, const char *resp) {
    char path[300];
    storage_path(path, sizeof(path), "problem.txt");
    Buf n = { 0 };
    buf_addf(&n, "%s, model %s: HTTP %d\n\n", names[brain], use_model, status);
    if (resp) { size_t l = strlen(resp); if (l > 6000) l = 6000; char *c = strndup(resp, l); if (c) { buf_add(&n, c); free(c); } }
    if (!n.failed) storage_write_atomic(path, n.s, n.len);
    buf_free(&n);
}

/* the network (it starts once); false (with the problem as the answer) if it isn't there */
static bool network(void) {
    net_start();
    while (net_state() == NET_STARTING) thr_sleep_ms(50);
    if (net_state() == NET_UP) {
        /* the date, from the network: certificates can't be checked without it
         * (the console's own clock is often years out) */
        netio_sync_clock();
        return true;
    }
    snprintf(answer, sizeof(answer), "%s", net_message()[0] ? net_message() : "There's no network. Is the network adapter connected?");
    state = AI_FAILED;
    return false;
}

/* the service is busy or down for a moment (or this model's allowance is used up): worth another try */
static bool busy_status(int status) { return status == 429 || status == 500 || status == 502 || status == 503 || status == 504; }

/* markdown the AI sometimes uses anyway (*Fallen*, **bold**, # headings, `code`), taken out for the bubble and the voice */
static void plain_text(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '*' || *p == '`' || *p == '#') continue;
        /* _like this_ (but not snake_case) */
        if (*p == '_' && (p == s || p[-1] == ' ' || !p[1] || p[1] == ' ' || p[1] == '.' || p[1] == ',')) continue;
        *o++ = *p;
    }
    *o = 0;
    /* and no doubled spaces left behind */
    o = s;
    for (char *p = s; *p; p++) { if (*p == ' ' && o > s && o[-1] == ' ') continue; *o++ = *p; }
    *o = 0;
}

static void run_question(void) {
    char err[200] = "";
    const char *models_[5];
    int nm = candidates(models_);
    bool gone = false;   /* the usual model has gone (as opposed to just being busy) */
    for (int pass = 0, mi = 0; mi < nm; mi++) {
        const char *use_model = models_[mi];
        Buf b;
        char url[300], headers[400];
        const char *path = build(&b, url, sizeof(url), headers, sizeof(headers), use_model);
        if (b.failed) { snprintf(answer, sizeof(answer), "Out of memory."); state = AI_FAILED; buf_free(&b); return; }
        char *resp = NULL;
        int status = https_post(url, headers, b.s, b.len, &resp, NULL, 256 * 1024, NULL, err, sizeof(err));
        buf_free(&b);
        if (status < 0) {
            if (strstr(err, "didn't answer") || strstr(err, "too long"))
                snprintf(answer, sizeof(answer), "%s took too long to answer. It may be very busy: ask me again in a moment!", names[brain]);
            else snprintf(answer, sizeof(answer), "%s", err);
            state = AI_FAILED;
            return;
        }
        if (status == 200 && json_get_string(resp, path, answer, sizeof(answer)) && answer[0]) {
            plain_text(answer);
            if (gone && strcmp(use_model, model)) snprintf(model, sizeof(model), "%s", use_model);   /* (the usual one has gone: keep this one) */
            if (!testing) { remember(true, question); remember(false, answer); }
            free(resp);
            state = AI_DONE;
            return;
        }
        char msg[300] = "";
        json_get_string(resp, "error.message", msg, sizeof(msg));
        note_problem(use_model, status, resp);
        free(resp);
        if (brain == AI_GEMINI && gemini_quick && thinking_refused(status, msg)) { gemini_quick = false; mi--; continue; }
        /* a model that isn't there (any more): try the next one */
        bool no_model = status == 404 || ((status == 400 || status == 403) && (strstr(msg, "model") || strstr(msg, "Model")) && !strstr(msg, "API key"));
        if (no_model) gone = true;
        if (no_model && mi + 1 < nm) continue;
        /* busy (or out of its allowance): another model, then one more go round after a moment */
        if (busy_status(status)) {
            if (mi + 1 < nm) continue;
            if (pass++ == 0) { thr_sleep_ms(2000); mi = -1; continue; }
        }
        if (status == 401 || status == 403 || (status == 400 && strstr(msg, "API key")))
            snprintf(answer, sizeof(answer), "%s didn't accept the key. (%s)", names[brain], msg[0] ? msg : "error");
        else if (status >= 500)
            snprintf(answer, sizeof(answer), "%s is very busy. Ask me again in a moment!", names[brain]);
        else if (status == 429)
            snprintf(answer, sizeof(answer), "%s says that's too many questions for now: try again in a minute. (%s)", names[brain], msg);
        else if (status == 200)
            snprintf(answer, sizeof(answer), "%s sent back an empty answer.", names[brain]);
        else
            snprintf(answer, sizeof(answer), "%s answered with error %d. %s", names[brain], status, msg);
        state = AI_FAILED;
        return;
    }
    state = AI_FAILED;
    return;
}

static void *ask_main(void *arg) {
    (void)arg;
    if (network()) run_question();
    return NULL;
}

/* ------------------------------------------------------------ listening */

static AiBrain ear;
static char ear_key[200], ear_model[80];
static const short *sound;
static int nsound, sound_rate;
static char heard[1200];
static volatile bool heard_ready, heard_nothing;

/* the recording as a .wav file (16-bit, mono) */
static void add_wav(Buf *b, bool base64) {
    size_t data = (size_t)nsound * 2, total = 44 + data;
    unsigned char *w = malloc(total);
    if (!w) { b->failed = true; return; }
    #define LE32(p, v) do { (p)[0] = (v) & 255; (p)[1] = ((v) >> 8) & 255; (p)[2] = ((v) >> 16) & 255; (p)[3] = ((v) >> 24) & 255; } while (0)
    memcpy(w, "RIFF", 4); LE32(w + 4, (unsigned)(total - 8)); memcpy(w + 8, "WAVEfmt ", 8);
    LE32(w + 16, 16u); w[20] = 1; w[21] = 0; w[22] = 1; w[23] = 0;
    LE32(w + 24, (unsigned)sound_rate); LE32(w + 28, (unsigned)sound_rate * 2); w[32] = 2; w[33] = 0; w[34] = 16; w[35] = 0;
    memcpy(w + 36, "data", 4); LE32(w + 40, (unsigned)data);
    for (int i = 0; i < nsound; i++) { w[44 + i * 2] = sound[i] & 255; w[45 + i * 2] = (sound[i] >> 8) & 255; }
    #undef LE32
    if (base64) buf_add_base64(b, w, total);
    else buf_add_mem(b, w, total);
    free(w);
}

/* writes down what was said; false (with the problem as the answer) if it couldn't */
static bool transcribe(void) {
    char url[300], headers[500], err[200] = "";
    const char *path;
    static const char *BOUNDARY = "BrainBoxSaid7d1f0c3a";
    const char *try_models[5];
    int nm = 0;
    if (ear == AI_GEMINI) {
        static const char *more[3] = { "gemini-2.5-flash", "gemini-2.5-flash-lite", "gemini-3.1-flash-lite" };
        try_models[nm++] = ear_model[0] ? ear_model : models[AI_GEMINI];
        for (int i = 0; i < 3; i++) if (strcmp(try_models[0], more[i])) try_models[nm++] = more[i];
        path = "candidates.0.content.parts.0.text";
    } else if (ear == AI_GROQ) {
        try_models[nm++] = "whisper-large-v3-turbo";
        try_models[nm++] = "whisper-large-v3";
        path = "text";
    } else {
        try_models[nm++] = "gpt-4o-mini-transcribe";
        try_models[nm++] = "whisper-1";
        path = "text";
    }
    for (int pass = 0, mi = 0; mi < nm; mi++) {
        Buf b = { 0 };
        if (ear == AI_GEMINI) {
            snprintf(url, sizeof(url), "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent", try_models[mi]);
            snprintf(headers, sizeof(headers), "x-goog-api-key: %s\r\n", ear_key);
            buf_add(&b, "{\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":");
            buf_add_json_string(&b, "Write down exactly what the person says in this recording, as plain text, with normal "
                                    "punctuation. Reply with only their words. If no one speaks, reply with just: -");
            buf_add(&b, "},{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"");
            add_wav(&b, true);
            buf_addf(&b, "\"}}]}],\"generationConfig\":{\"maxOutputTokens\":400%s}}", gemini_quick ? QUICK : "");
        } else {
            snprintf(url, sizeof(url), ear == AI_GROQ ? "https://api.groq.com/openai/v1/audio/transcriptions" : "https://api.openai.com/v1/audio/transcriptions");
            snprintf(headers, sizeof(headers), "Content-Type: multipart/form-data; boundary=%s\r\nAuthorization: Bearer %s\r\n", BOUNDARY, ear_key);
            buf_addf(&b, "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n", BOUNDARY, try_models[mi]);
            buf_addf(&b, "--%s\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nen\r\n", BOUNDARY);
            buf_addf(&b, "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"question.wav\"\r\nContent-Type: audio/wav\r\n\r\n", BOUNDARY);
            add_wav(&b, false);
            buf_addf(&b, "\r\n--%s--\r\n", BOUNDARY);
        }
        if (b.failed) { buf_free(&b); snprintf(answer, sizeof(answer), "Out of memory."); return false; }
        char *resp = NULL;
        int status = https_post(url, headers, b.s, b.len, &resp, NULL, 64 * 1024, NULL, err, sizeof(err));
        buf_free(&b);
        if (status < 0) { snprintf(answer, sizeof(answer), "%s", err); return false; }
        if (status == 200 && json_get_string(resp, path, heard, sizeof(heard))) {
            free(resp);
            /* tidy: no spaces or line breaks round it */
            char *p = heard;
            while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '"') p++;
            memmove(heard, p, strlen(p) + 1);
            size_t l = strlen(heard);
            while (l && (heard[l - 1] == ' ' || heard[l - 1] == '\n' || heard[l - 1] == '\r' || heard[l - 1] == '"')) heard[--l] = 0;
            for (p = heard; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
            if (!strcmp(heard, "-") || !strcmp(heard, ".") || !heard[0]) heard[0] = 0;
            return true;
        }
        char msg[300] = "";
        json_get_string(resp, "error.message", msg, sizeof(msg));
        AiBrain was = brain; brain = ear; note_problem(try_models[mi], status, resp); brain = was;
        free(resp);
        if (ear == AI_GEMINI && gemini_quick && thinking_refused(status, msg)) { gemini_quick = false; mi--; continue; }
        bool no_model = status == 404 || ((status == 400 || status == 403) && (strstr(msg, "model") || strstr(msg, "Model")) && !strstr(msg, "API key"));
        if (no_model && mi + 1 < nm) continue;
        if (busy_status(status)) {
            if (mi + 1 < nm) continue;
            if (pass++ == 0) { thr_sleep_ms(2000); mi = -1; continue; }
        }
        if (status == 401 || status == 403 || (status == 400 && strstr(msg, "API key")))
            snprintf(answer, sizeof(answer), "%s didn't accept the key for listening. (%s)", names[ear], msg[0] ? msg : "error");
        else if (status >= 500)
            snprintf(answer, sizeof(answer), "%s is very busy, so I couldn't hear you properly. Try me again in a moment!", names[ear]);
        else if (status == 429)
            snprintf(answer, sizeof(answer), "%s says that's too much listening for now: try again in a minute. (%s)", names[ear], msg);
        else
            snprintf(answer, sizeof(answer), "%s couldn't write down what you said: error %d. %s", names[ear], status, msg);
        return false;
    }
    return false;
}

static void *hear_main(void *arg) {
    (void)arg;
    if (!network()) return NULL;
    if (!transcribe()) { state = AI_FAILED; return NULL; }
    if (!heard[0]) { heard_nothing = true; answer[0] = 0; state = AI_FAILED; return NULL; }
    snprintf(question, sizeof(question), "%s", heard);
    heard_ready = true;
    run_question();
    return NULL;
}

static bool start_with(void *(*fn)(void *)) {
    if (state == AI_BUSY) return false;
    if (state != AI_IDLE) thr_join(th);   /* the last question's thread has finished */
    state = AI_BUSY;
    answer[0] = 0;
    heard_ready = heard_nothing = false;
    if (!thr_create(&th, fn, NULL, 64 * 1024, 50)) {
        snprintf(answer, sizeof(answer), "Couldn't start asking.");
        state = AI_FAILED;
        return false;
    }
    return true;
}
static bool start(void) { return start_with(ask_main); }

/* the network and the clock, made ready in the background while the
 * opening plays (so the first question doesn't wait for them) */
static void *warm_main(void *arg) {
    (void)arg;
    net_start();
    while (net_state() == NET_STARTING) thr_sleep_ms(50);
    if (net_state() == NET_UP) netio_sync_clock();
    return NULL;
}
void ai_warm_up(void) {
    static thr_t wt;
    static bool started;
    if (started) return;
    started = thr_create(&wt, warm_main, NULL, 32 * 1024, 45);
}

void ai_ask(const char *q) {
    if (state == AI_BUSY) return;
    snprintf(question, sizeof(question), "%s", q);
    testing = false;
    start();
}

bool ai_test(void) {
    if (state == AI_BUSY) return false;
    snprintf(question, sizeof(question), "Please introduce yourself in one short, cheerful sentence.");
    testing = true;
    return start();
}

AiState ai_state(void) { return state; }
const char *ai_answer(void) { return answer; }
const char *ai_model(void) { return model; }

void ai_hear(const short *pcm, int n, int rate, AiBrain e, const char *k, const char *m) {
    if (state == AI_BUSY) return;
    sound = pcm; nsound = n; sound_rate = rate;
    ear = e < AI_COUNT ? e : AI_GEMINI;
    snprintf(ear_key, sizeof(ear_key), "%s", k ? k : "");
    snprintf(ear_model, sizeof(ear_model), "%s", m ? m : "");
    heard[0] = 0;
    testing = false;
    start_with(hear_main);
}
const char *ai_heard(void) { return heard_ready ? heard : NULL; }
bool ai_heard_nothing(void) { return heard_nothing; }
bool ai_can_hear(AiBrain b) { return b == AI_GEMINI || b == AI_OPENAI || b == AI_GROQ; }

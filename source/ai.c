/* ai.c - see ai.h. */
#include "ai.h"
#include "json.h"
#include "net/https.h"
#include "net/net.h"
#include "net/thr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ANSWER_MAX 4000
#define TURNS 12                     /* questions and answers kept for context */

static const char *SYSTEM =
    "You are Professor Pixel, a kindly, cheerful, slightly absent-minded old professor who lives as a "
    "hologram inside a Nintendo GameCube. Your replies are read aloud by a simple speech synthesizer and "
    "shown in a small speech bubble, so: answer in one to four short sentences of plain spoken English; "
    "no lists, headings, markdown, emoji or web addresses; write numbers and symbols the way they are "
    "said aloud when that is clearer. Be warm, curious and encouraging, with an old professor's gentle "
    "humor; now and then begin with 'Ah!' or 'Hmm.' If a question needs a long answer, give the heart of "
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

static const char *names[AI_COUNT] = { "Google Gemini", "Claude", "ChatGPT" };
static const char *ids[AI_COUNT] = { "gemini", "claude", "openai" };
static const char *pages[AI_COUNT] = { "https://aistudio.google.com/apikey", "https://console.anthropic.com/settings/keys",
                                       "https://platform.openai.com/api-keys" };
static const char *models[AI_COUNT] = { "gemini-flash-latest", "claude-haiku-5-5", "gpt-5-mini" };

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

static void *ask_main(void *arg) {
    (void)arg;
    char err[200] = "";
    /* the network first (it starts once) */
    net_start();
    while (net_state() == NET_STARTING) thr_sleep_ms(50);
    if (net_state() != NET_UP) {
        snprintf(answer, sizeof(answer), "%s", net_message()[0] ? net_message() : "There's no network. Is the network adapter connected?");
        state = AI_FAILED;
        return NULL;
    }
    Buf b = { 0 };
    char url[300], headers[400];
    const char *path, *err_path = "error.message";
    switch (brain) {
    case AI_GEMINI:
        snprintf(url, sizeof(url), "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent", model);
        snprintf(headers, sizeof(headers), "x-goog-api-key: %s\r\n", key);
        buf_add(&b, "{\"systemInstruction\":{\"parts\":[{\"text\":");
        buf_add_json_string(&b, SYSTEM);
        buf_add(&b, "}]},\"contents\":[");
        messages(&b, question, true);
        buf_add(&b, "],\"generationConfig\":{\"maxOutputTokens\":2048}}");
        path = "candidates.0.content.parts.0.text";
        break;
    case AI_CLAUDE:
        snprintf(url, sizeof(url), "https://api.anthropic.com/v1/messages");
        snprintf(headers, sizeof(headers), "x-api-key: %s\r\nanthropic-version: 2023-06-01\r\n", key);
        buf_addf(&b, "{\"model\":\"%s\",\"max_tokens\":600,\"system\":", model);
        buf_add_json_string(&b, SYSTEM);
        buf_add(&b, ",\"messages\":[");
        messages(&b, question, false);
        buf_add(&b, "]}");
        path = "content.0.text";
        break;
    default:
        snprintf(url, sizeof(url), "https://api.openai.com/v1/chat/completions");
        snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", key);
        buf_addf(&b, "{\"model\":\"%s\",\"max_completion_tokens\":2048,\"messages\":[{\"role\":\"system\",\"content\":", model);
        buf_add_json_string(&b, SYSTEM);
        buf_add(&b, "}");
        messages(&b, question, false);   /* (each with a comma before it) */
        buf_add(&b, "]}");
        path = "choices.0.message.content";
        break;
    }
    if (b.failed) { snprintf(answer, sizeof(answer), "Out of memory."); state = AI_FAILED; buf_free(&b); return NULL; }
    char *resp = NULL;
    int status = https_post(url, headers, b.s, b.len, &resp, NULL, 256 * 1024, NULL, err, sizeof(err));
    buf_free(&b);
    if (status < 0) {
        snprintf(answer, sizeof(answer), "%s", err);
        state = AI_FAILED;
        return NULL;
    }
    if (status == 200 && json_get_string(resp, path, answer, sizeof(answer)) && answer[0]) {
        if (!testing) { remember(true, question); remember(false, answer); }
        state = AI_DONE;
    } else {
        char msg[300] = "";
        json_get_string(resp, err_path, msg, sizeof(msg));
        if (status == 401 || status == 403 || (status == 400 && strstr(msg, "API key")))
            snprintf(answer, sizeof(answer), "%s didn't accept the key. (%s)", names[brain], msg[0] ? msg : "error");
        else if (status == 429)
            snprintf(answer, sizeof(answer), "%s says that's too many questions for now: try again in a minute. (%s)", names[brain], msg);
        else if (status == 200)
            snprintf(answer, sizeof(answer), "%s sent back an empty answer.", names[brain]);
        else
            snprintf(answer, sizeof(answer), "%s answered with error %d. %s", names[brain], status, msg);
        state = AI_FAILED;
    }
    free(resp);
    return NULL;
}

static bool start(void) {
    if (state == AI_BUSY) return false;
    if (state != AI_IDLE) thr_join(th);   /* the last question's thread has finished */
    state = AI_BUSY;
    answer[0] = 0;
    if (!thr_create(&th, ask_main, NULL, 64 * 1024, 50)) {
        snprintf(answer, sizeof(answer), "Couldn't start asking.");
        state = AI_FAILED;
        return false;
    }
    return true;
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

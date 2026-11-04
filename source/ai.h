/* ai.h - Professor Pixel's "brain": Google Gemini, Claude (Anthropic) or
 * ChatGPT (OpenAI), reached directly over HTTPS with the person's own API
 * key. The conversation so far goes with each question, so he remembers what
 * was said (the last several turns). A question runs on a thread of its own. */
#pragma once
#include <stdbool.h>

typedef enum { AI_GEMINI, AI_CLAUDE, AI_OPENAI, AI_COUNT } AiBrain;
typedef enum { AI_IDLE, AI_BUSY, AI_DONE, AI_FAILED } AiState;

const char *ai_name(AiBrain b);          /* "Google Gemini", "Claude", "ChatGPT" */
const char *ai_key_page(AiBrain b);      /* where to make a key */
const char *ai_id(AiBrain b);            /* "gemini", "claude", "openai" (settings, the web page) */
AiBrain ai_from_id(const char *id);      /* AI_COUNT if unknown */

void ai_set(AiBrain b, const char *key, const char *model);   /* model "" = the usual one */
const char *ai_default_model(AiBrain b);

void ai_ask(const char *question);       /* starts asking (CP-1252 text) */
bool ai_test(void);                      /* starts a quick check that the key works */
AiState ai_state(void);
const char *ai_answer(void);             /* when AI_DONE: the reply (CP-1252), or the problem when AI_FAILED */
void ai_forget(void);                    /* a new conversation */

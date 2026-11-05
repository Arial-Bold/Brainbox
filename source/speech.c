/* speech.c - see speech.h.
 *
 * Three parts:
 *   - the speaking thread: splits the text into sentences, makes each one
 *     with Flite and copies it into a ring in ARAM;
 *   - the player thread: copies the next piece from ARAM into a queue of
 *     small chunks in main memory (silence when there's nothing to say),
 *     noting how loud each one is;
 *   - one sound voice, streamed: each time the sound library has finished
 *     a chunk it asks (in the audio interrupt) for the next one in the queue,
 *     as Flashcube's menu music does.
 * The ARAM copies are made in small pieces with interrupts off, and the copy
 * engine's "finished" flag is put back as the sound library expects, as in
 * CubeBrowser's and Flashcube's ARAM code. */
#include "speech.h"
#include "ui.h"
#include "net/thr.h"
#include "flite.h"
#include "saywords.h"
#include <gccore.h>
#include <aesndlib.h>
#include <ogc/aram.h>
#include <ogc/machine/processor.h>
#include <ogc/lwp_watchdog.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cst_voice *register_cmu_us_awb(const char *voxdir);
void usenglish_init(cst_voice *v);
cst_lexicon *cmulex_init(void);
extern jmp_buf *cst_errjmp;

#define RATE 16000
#define CHUNK 1152                         /* samples: 72 ms */
#define NCHUNKS 8
#define RING_BASE 0x400000u                /* ARAM: 4 MB in, */
#define RING_SAMPLES (4u * 1024 * 1024)    /* 8 MB of speech: over four minutes */

static cst_voice *voice;
static volatile bool ready;

/* Flite's speed-up for this voice: the last few of the fine details of each
 * sound are left out (0 = all of them, as now). 5 makes him about 1.7 times
 * quicker to make, for a little less detail in his voice. */
#define VOICE_SPEEDUP 0

/* Holding back the start: if he's being made slower than he speaks, he'd run
 * out partway and pause. So the start waits until enough is made that the
 * rest will keep up (worked out from how quickly the first pieces came), but
 * never more than a few seconds. */
#define HOLD_MAX_MS 4000
static volatile bool holding;
static volatile u32 hold_at;

/* ------------------------------------------------------------------ ARAM */

#define DSP_CSR (*(volatile u16 *)0xCC00500A)
#define PIECE 4096u
static bool wait_idle(void) {
    for (int k = 0; k < 400000; k++) if (!AR_GetDMAStatus()) return true;
    return false;
}
static bool dma(u32 dir, void *mram, u32 aram, u32 len) {
    for (u32 off = 0; off < len; off += PIECE) {
        u32 n = len - off < PIECE ? len - off : PIECE;
        u32 level;
        _CPU_ISR_Disable(level);
        bool ok = wait_idle();
        if (ok) {
            bool flagged = (DSP_CSR & 0x0020) != 0;
            AR_StartDMA(dir, (u32)MEM_VIRTUAL_TO_PHYSICAL((char *)mram + off), aram + off, n);
            ok = wait_idle();
            if (ok && !flagged) DSP_CSR = (u16)((DSP_CSR & (u16)~(0x0080 | 0x0008)) | 0x0020);
        }
        _CPU_ISR_Restore(level);
        if (!ok) return false;
    }
    return true;
}

/* the ring, in samples (positions only ever grow; the ARAM address wraps) */
static volatile u32 wpos, rpos;
static volatile u32 gen;                   /* bumped to cut off what's being said */

static void ring_io(bool write, s16 *buf, u32 pos, u32 n) {   /* n: a multiple of 16 */
    while (n) {
        u32 at = pos % RING_SAMPLES, k = RING_SAMPLES - at < n ? RING_SAMPLES - at : n;
        DCFlushRange(buf, k * 2);
        dma(write ? AR_MRAMTOARAM : AR_ARAMTOMRAM, buf, RING_BASE + at * 2, k * 2);
        if (!write) DCInvalidateRange(buf, k * 2);
        buf += k; pos += k; n -= k;
    }
}

/* --------------------------------------------------------------- playing */

static s16 chunks[NCHUNKS][CHUNK] __attribute__((aligned(32)));
static s16 quiet[CHUNK] __attribute__((aligned(32)));
static float levels[NCHUNKS];
static volatile u32 filled, handed;
static volatile float level_now;
static AESNDPB *pb;

static void voice_cb(AESNDPB *p, u32 st) {
    if (st == VOICE_STATE_RUNNING) { for (int k = 0; k < 20000 && AR_GetDMAStatus(); k++) {} }
    else if (st == VOICE_STATE_STREAM) {
        if (filled > handed) {
            level_now = levels[handed % NCHUNKS];
            AESND_SetVoiceBuffer(p, chunks[handed % NCHUNKS], CHUNK * 2);
            handed++;
        } else {
            level_now = 0;
            AESND_SetVoiceBuffer(p, quiet, CHUNK * 2);
        }
    }
}

static volatile bool making;               /* the speaking thread is busy */

static void *player_main(void *arg) {
    (void)arg;
    static s16 tmp[CHUNK] __attribute__((aligned(32)));
    for (;;) {
        while (filled < handed + NCHUNKS - 2) {
            int k = (int)(filled % NCHUNKS);
            u32 have = wpos - rpos;
            if (holding && ticks_to_millisecs(gettime()) - hold_at > HOLD_MAX_MS + 500) holding = false;   /* (just in case) */
            if (!holding && (have >= CHUNK || (have > 0 && !making))) {
                u32 n = have >= CHUNK ? CHUNK : ((have + 15) & ~15u);
                ring_io(false, tmp, rpos, n);
                if (n < CHUNK) memset(tmp + n, 0, (CHUNK - n) * 2);
                rpos += have >= CHUNK ? CHUNK : have;
                memcpy(chunks[k], tmp, CHUNK * 2);
                double e = 0;
                for (int i = 0; i < CHUNK; i += 4) e += (double)tmp[i] * tmp[i];
                float rms = sqrtf((float)(e / (CHUNK / 4))) / 6000.0f;
                levels[k] = rms > 1 ? 1 : rms;
            } else {
                memcpy(chunks[k], quiet, CHUNK * 2);
                levels[k] = 0;
            }
            DCFlushRange(chunks[k], CHUNK * 2);
            filled++;
        }
        /* (0x100 is the sound as made; the top setting is a quarter louder) */
        u16 vol = (u16)(ui_set.voice_volume * 320 / UI_VOLUME_MAX);
        AESND_SetVoiceVolume(pb, vol, vol);
        thr_sleep_ms(15);
    }
    return NULL;
}

/* --------------------------------------------------------------- talking */

static char job[4096];
static volatile u32 job_gen;
static thr_mutex job_lock;

/* the next sentence of text from *p into out (moves *p on); false at the end */
static bool next_sentence(const char **p, char *out, size_t cap, bool first) {
    const char *s = *p;
    while (*s == ' ' || *s == '\n') s++;
    if (!*s) return false;
    size_t n = 0;
    for (;;) {
        char c = s[n];
        if (!c) break;
        if (c == '\n') { n++; break; }
        n++;
        if ((c == '.' || c == '!' || c == '?') && (s[n] == ' ' || s[n] == '\n' || !s[n])) {
            if (n > 24) break;   /* (very short ones ride along with the next) */
        }
        if (n > 220 && (c == ',' || c == ';' || c == ' ')) break;   /* long: break at a comma or a space */
        if (n > 110 && (c == ',' || c == ';' || c == ':')) break;   /* (pieces kept short, so the next is ready in time) */
        /* the first piece short, so he starts talking sooner (the rest is
         * made while it plays): at a comma, or after a few words */
        if (first && ((n > 18 && (c == ',' || c == ';' || c == ':')) || (n > 48 && c == ' '))) break;
        if (n >= cap - 1) break;
    }
    memcpy(out, s, n);
    out[n] = 0;
    *p = s + n;
    return true;
}

/* Writing him into the ring as he's made. Flite hands over each little bit
 * of a sentence as soon as it's made (its "streaming"), so he starts saying a
 * sentence while the end of it is still being made, rather than going quiet
 * until all of it is ready: that was the pause partway through a sentence. */
static u32 cur_gen;                         /* the job being made */
static u32 made;                            /* samples of it made so far */
static size_t total_chars, done_chars;      /* its length, and the part already made */
static s16 acc[4096] __attribute__((aligned(32)));
static int acc_n;

/* enough made yet that the rest keeps up? (see HOLD_MAX_MS) */
static void hold_check(void) {
    if (!holding) return;
    float spent = (ticks_to_millisecs(gettime()) - hold_at) / 1000.0f;
    float secs = made / (float)RATE;
    float speed = spent > 0.05f ? secs / spent : 99;   /* seconds of him made a second */
    float per_char = done_chars ? secs / done_chars : 0.07f;   /* (about 14 letters a second, until it's known) */
    float left = total_chars * per_char - secs;   /* seconds of him still to make */
    if (left < 0) left = 0;
    float need = speed < 1 ? left * (1 / speed - 1) * 1.15f + 0.3f : 0;
    if (secs >= need || spent * 1000 >= HOLD_MAX_MS) holding = false;
}

/* into the ring: what's gathered (a multiple of 16 samples; all of it, padded with quiet, if last) */
static void flush(bool last) {
    int k = last ? (acc_n + 15) & ~15 : acc_n & ~15;
    if (k <= 0) return;
    for (int j = acc_n; j < k; j++) acc[j] = 0;
    /* room in the ring? (he never gets four minutes ahead, but just in case)
     * And once he's several seconds ahead, it rests: the rest of the app,
     * the controls and the AI, get their turn. */
    while (cur_gen == gen && (wpos + (u32)k - rpos > RING_SAMPLES - CHUNK * NCHUNKS || (!holding && wpos - rpos > RATE * 5))) thr_sleep_ms(20);
    if (cur_gen != gen) { acc_n = 0; return; }
    ring_io(true, acc, wpos, (u32)k);
    wpos += (u32)k;
    made += (u32)k;
    int rest = acc_n - k;
    if (rest > 0) memmove(acc, acc + k, rest * 2); else rest = 0;
    acc_n = rest;
    hold_check();
}

static void put(const s16 *x, int n) {
    while (n > 0) {
        int k = (int)(sizeof(acc) / 2) - acc_n;
        if (k > n) k = n;
        memcpy(acc + acc_n, x, k * 2);
        acc_n += k; x += k; n -= k;
        if (acc_n >= 2048) flush(false);   /* (a little over a tenth of a second at a time) */
        if (cur_gen != gen) return;
    }
}

static int streamed(const cst_wave *w, int start, int size, int last, cst_audio_streaming_info *asi) {
    (void)last; (void)asi;
    if (cur_gen != gen) return CST_AUDIO_STREAM_STOP;   /* (cut off: stop making the rest) */
    put(w->samples + start, size);
    return cur_gen == gen ? CST_AUDIO_STREAM_CONT : CST_AUDIO_STREAM_STOP;
}

static void *talk_main(void *arg) {
    (void)arg;
    /* Flite first */
    jmp_buf jb;
    cst_errjmp = &jb;
    if (!setjmp(jb)) {
        flite_init();
        flite_add_lang("eng", usenglish_init, cmulex_init);
        flite_add_lang("usenglish", usenglish_init, cmulex_init);
        voice = register_cmu_us_awb(NULL);
        saywords_teach(voice);   /* the words it would get wrong */
        if (voice) {
            if (VOICE_SPEEDUP) feat_set_int(voice->features, "mlsa_speed_param", VOICE_SPEEDUP);
            cst_audio_streaming_info *asi = new_audio_streaming_info();
            asi->min_buffsize = 1024;
            asi->asc = streamed;
            feat_set(voice->features, "streaming_info", audio_streaming_info_val(asi));
        }
    }
    cst_errjmp = NULL;
    ready = voice != NULL;
    static s16 gap[RATE / 6];   /* a short pause after each sentence */
    memset(gap, 0, sizeof(gap));
    u32 done_gen = 0;
    for (;;) {
        if (!ready || job_gen == done_gen) { thr_sleep_ms(20); continue; }
        thr_lock(&job_lock);
        u32 g = job_gen;
        static char text[4096];
        saywords_tidy(job, text, sizeof(text));   /* ("Hmm" said as a hum, not spelled out; "e.g." as "for example"...) */
        thr_unlock(&job_lock);
        done_gen = g;
        cur_gen = g;
        making = true;
        const char *p = text;
        total_chars = strlen(text);
        done_chars = 0;
        made = 0;
        acc_n = 0;
        hold_at = ticks_to_millisecs(gettime());
        holding = true;
        char sentence[400];
        bool first = true;
        /* a moment of quiet first, so the start of the first word isn't lost
         * (some TVs and adapters take a moment to wake their sound up) */
        {
            static s16 lead[RATE / 8] __attribute__((aligned(32)));
            for (int j = 0; j < RATE / 8; j++) lead[j] = (s16)((j * 7919 % 5) - 2);   /* (the faintest hiss, not silence) */
            if (wpos == rpos) { ring_io(true, lead, wpos, RATE / 8 & ~15u); wpos += RATE / 8 & ~15u; }
        }
        while (g == gen && next_sentence(&p, sentence, sizeof(sentence), first)) {
            first = false;
            cst_wave *w = NULL;
            cst_errjmp = &jb;
            if (!setjmp(jb)) w = flite_text_to_wave(sentence, voice);   /* (into the ring as it's made: streamed) */
            cst_errjmp = NULL;
            if (w) delete_wave(w);
            if (g != gen) break;
            done_chars = (size_t)(p - text);
            put(gap, sizeof(gap) / 2);
        }
        if (g == gen) flush(true);
        acc_n = 0;
        holding = false;
        making = false;
    }
    return NULL;
}

/* ------------------------------------------------------------------ calls */

void speech_init(void) {
    if (!AR_CheckInit()) AR_Init(NULL, 0);
    /* between sentences: the faintest hiss, not digital silence (some TVs and
     * HDMI adapters mute themselves during silence and clip the next word) */
    for (int i = 0; i < CHUNK; i++) quiet[i] = (s16)((i * 7919 % 5) - 2);
    DCFlushRange(quiet, sizeof(quiet));
    thr_mutex_init(&job_lock);
    pb = AESND_AllocateVoice(voice_cb);
    if (pb) {
        u32 isr;
        _CPU_ISR_Disable(isr);
        AESND_PlayVoice(pb, VOICE_MONO16, quiet, CHUNK * 2, RATE, 0, true);
        AESND_SetVoiceStream(pb, true);
        _CPU_ISR_Restore(isr);
    }
    static thr_t t1, t2;
    thr_create(&t1, player_main, NULL, 16 * 1024, 70);
    /* Flite nests deeply: a big stack, and a low priority (drawing comes first) */
    static u8 stack[512 * 1024] __attribute__((aligned(32)));
    /* (above the main thread and the background work, so he isn't left
     * waiting for the next sentence mid-answer; below the drawing and the
     * player, which keep the picture and the sound smooth) */
    LWP_CreateThread(&t2, talk_main, NULL, stack, sizeof(stack), 66);
}

bool speech_ready(void) { return ready; }

void speech_say(const char *text) {
    thr_lock(&job_lock);
    snprintf(job, sizeof(job), "%s", text);
    gen++;
    job_gen = gen;
    rpos = wpos;   /* drop what's queued */
    thr_unlock(&job_lock);
}

void speech_stop(void) {
    thr_lock(&job_lock);
    gen++;
    rpos = wpos;
    thr_unlock(&job_lock);
}

bool speech_busy(void) { return making || wpos != rpos || level_now > 0; }
float speech_level(void) { return level_now; }

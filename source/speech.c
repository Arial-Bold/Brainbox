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
            if (have >= CHUNK || (have > 0 && !making)) {
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
    }
    cst_errjmp = NULL;
    ready = voice != NULL;
    static s16 stage[16384] __attribute__((aligned(32)));
    u32 done_gen = 0;
    for (;;) {
        if (!ready || job_gen == done_gen) { thr_sleep_ms(20); continue; }
        thr_lock(&job_lock);
        u32 g = job_gen;
        static char text[4096];
        saywords_tidy(job, text, sizeof(text));   /* ("Hmm" said as a hum, not spelled out; "e.g." as "for example"...) */
        thr_unlock(&job_lock);
        done_gen = g;
        making = true;
        const char *p = text;
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
            if (!setjmp(jb)) w = flite_text_to_wave(sentence, voice);
            cst_errjmp = NULL;
            if (!w) continue;
            int n = cst_wave_num_samples(w);
            /* a short pause after each sentence */
            int pause = RATE / 6;
            for (int i = 0; i < n + pause && g == gen;) {
                int k = n + pause - i;
                if (k > (int)(sizeof(stage) / 2)) k = sizeof(stage) / 2;
                k &= ~15;
                if (k <= 0) break;
                /* room in the ring? (he never gets four minutes ahead, but just in case) */
                while (g == gen && wpos + (u32)k - rpos > RING_SAMPLES - CHUNK * NCHUNKS) thr_sleep_ms(20);
                for (int j = 0; j < k; j++) stage[j] = i + j < n ? w->samples[i + j] : 0;
                if (g != gen) break;
                ring_io(true, stage, wpos, (u32)k);
                wpos += (u32)k;
                i += k;
            }
            delete_wave(w);
        }
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
    LWP_CreateThread(&t2, talk_main, NULL, stack, sizeof(stack), 40);
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

/* sound.c - plays the menu sounds (synthesised at start-up by sfxsynth.c) on
 * the DSP's hardware voices through libogc2's AESND library. From CubeBrowser,
 * by way of Flashcube. */
#include "sound.h"
#include "holosynth.h"
#include "ui.h"
#include "net/thr.h"
#include <gccore.h>
#include <aesndlib.h>
#include <ogc/aram.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define RATE SFX_RATE

static s16 *buffers[SFX_COUNT];
static u32 sizes[SFX_COUNT];
static bool ready;

static void synth(int id) {
	int n = sfx_length(id);
	if (n <= 0) return;
	u32 bytes = ((u32)n * 2 + 31) & ~31u;
	s16 *buf = memalign(32, bytes);
	if (!buf) return;
	memset(buf, 0, bytes);
	if (!sfx_render(id, buf, n)) { free(buf); return; }
	DCFlushRange(buf, bytes);
	sizes[id] = bytes;
	__sync_synchronize();   /* the size is in place before the sound counts as ready */
	buffers[id] = buf;
}

/* The sounds are made on a thread of their own while the splash screen
 * shows, the ones used most first. */
static const Sfx synth_order[] = {
	SFX_HOVER, SFX_HOVER2, SFX_HOVER3, SFX_SELECT, SFX_BACK, SFX_CONFIRM, SFX_CANCEL, SFX_ERROR,
};
static thr_t synth_th;
static bool synth_started;
static volatile bool synth_quit;

/* the hologram switching on (the opening): made first, it's needed first */
static s16 *holo_buf;
static u32 holo_bytes;
static AESNDPB *holo_voice;
static void synth_holo(void) {
	int n = holo_length();
	u32 bytes = ((u32)n * 2 + 31) & ~31u;
	s16 *buf = memalign(32, bytes);
	if (!buf) return;
	memset(buf, 0, bytes);
	if (!holo_render(buf, n)) { free(buf); return; }
	DCFlushRange(buf, bytes);
	holo_bytes = bytes;
	__sync_synchronize();
	holo_buf = buf;
}

static s16 *shim_buf;
static u32 shim_bytes;
static void synth_shimmer(void) {
	int n = shimmer_length();
	u32 bytes = ((u32)n * 2 + 31) & ~31u;
	s16 *buf = memalign(32, bytes);
	if (!buf) return;
	memset(buf, 0, bytes);
	if (!shimmer_render(buf, n)) { free(buf); return; }
	DCFlushRange(buf, bytes);
	shim_bytes = bytes;
	__sync_synchronize();
	shim_buf = buf;
}

/* the background, and its bell notes */
static s16 *amb_buf, *bell_buf[AMB_BELLS];
static u32 amb_bytes, bell_bytes;
static AESNDPB *amb_voice, *bell_voice;
static s16 *synth_pcm(int n, u32 *bytes_out) {
	u32 bytes = ((u32)n * 2 + 31) & ~31u;
	s16 *buf = memalign(32, bytes);
	if (buf) memset(buf, 0, bytes);
	*bytes_out = bytes;
	return buf;
}
static void synth_ambience(void) {
	u32 bytes;
	for (int k = 0; k < AMB_BELLS && !synth_quit; k++) {
		s16 *b = synth_pcm(amb_bell_length(), &bytes);
		if (!b) return;
		amb_bell_render(b, amb_bell_length(), k);
		DCFlushRange(b, bytes);
		bell_bytes = bytes;
		__sync_synchronize();
		bell_buf[k] = b;
	}
	s16 *b = synth_pcm(ambience_length(), &bytes);
	if (!b) return;
	if (!ambience_render(b, ambience_length())) { free(b); return; }
	DCFlushRange(b, bytes);
	amb_bytes = bytes;
	__sync_synchronize();
	amb_buf = b;
}

static void *synth_main(void *arg) {
	(void)arg;
	synth_holo();
	/* (the opening's sound is made first, ahead of everything else, so the
	 * opening isn't kept waiting; the rest are made in the background) */
	LWP_SetThreadPriority(LWP_GetSelf(), 30);   /* (behind his voice, too) */
	for (unsigned k = 0; k < sizeof(synth_order) / sizeof(synth_order[0]) && !synth_quit; k++) synth(synth_order[k]);
	if (!synth_quit) synth_shimmer();
	if (!synth_quit) synth_ambience();
	return NULL;
}

bool sound_loaded(Sfx s) { return s >= 0 && s < SFX_COUNT && buffers[s] != NULL; }

/* Before each of a voice's copies to sound memory, wait for any copy still
 * in progress: the sound library copies every voice through one shared buffer. */
static void sfx_cb(AESNDPB *pb, u32 st) {
	(void)pb;
	if (st == VOICE_STATE_RUNNING)
		for (int k = 0; k < 20000 && AR_GetDMAStatus(); k++) {}
}

/* A few voices of their own; each plays one sound once and stops itself. */
#define SFX_VOICES 6
static AESNDPB *sfx_voice[SFX_VOICES];
static u64 sfx_busy_until[SFX_VOICES];
static Sfx sfx_what[SFX_VOICES];   /* the sound each voice played last */
static int sfx_next;

void sound_init(void) {
	AESND_Init();
	AESND_Pause(false);
	for (int i = 0; i < SFX_VOICES; i++) sfx_voice[i] = AESND_AllocateVoice(sfx_cb);
	holo_voice = AESND_AllocateVoice(sfx_cb);
	amb_voice = AESND_AllocateVoice(sfx_cb);
	bell_voice = AESND_AllocateVoice(sfx_cb);
	ready = true;
	synth_started = thr_create(&synth_th, synth_main, NULL, 32 * 1024, 110);
	if (!synth_started) synth_holo();
	if (!synth_started) for (unsigned k = 0; k < sizeof(synth_order) / sizeof(synth_order[0]); k++) synth(synth_order[k]);
}

void sound_shutdown(void) {
	if (synth_started) { synth_quit = true; thr_join(synth_th); synth_started = false; }
	if (!ready) return;
	ready = false;
	AESND_Pause(true);
	AESND_Reset();
}

void sound_play(Sfx s) {
	/* moves take turns between three sounds, like the system menu */
	static int move_turn;
	if (s == SFX_HOVER) { s = (Sfx)(SFX_HOVER + move_turn); move_turn = (move_turn + 1) % 3; }
	if (!ready || ui_set.sound_volume <= 0 || !buffers[s]) return;
	/* prefer a voice whose last sound has finished */
	u64 now = gettime();
	int v = -1;
	for (int k = 0; k < SFX_VOICES && v < 0; k++) {
		int i = (sfx_next + k) % SFX_VOICES;
		if (sfx_voice[i] && now >= sfx_busy_until[i]) v = i;
	}
	if (v < 0) return;
	sfx_next = (v + 1) % SFX_VOICES;
	u16 vol = (u16)(ui_set.sound_volume * 255 / UI_VOLUME_MAX);
	AESND_SetVoiceVolume(sfx_voice[v], vol, vol);
	AESND_PlayVoice(sfx_voice[v], VOICE_MONO16, buffers[s], sizes[s], RATE, 0, false);
	sfx_busy_until[v] = now + millisecs_to_ticks((sizes[s] / 2) * 1000 / RATE + 30);
	sfx_what[v] = s;
}

void sound_stop(Sfx s) {
	if (!ready) return;
	u64 now = gettime();
	for (int i = 0; i < SFX_VOICES; i++)
		if (sfx_voice[i] && sfx_what[i] == s && now < sfx_busy_until[i]) {
			AESND_SetVoiceStop(sfx_voice[i], true);
			sfx_busy_until[i] = now;
		}
}

bool sound_holo_ready(void) { return holo_buf != NULL; }

void sound_play_holo(void) {
	if (!ready || !holo_buf || !holo_voice || ui_set.voice_volume <= 0) return;
	u16 vol = (u16)(ui_set.voice_volume * 255 / UI_VOLUME_MAX);   /* (as loud as his voice) */
	AESND_SetVoiceVolume(holo_voice, vol, vol);
	AESND_PlayVoice(holo_voice, VOICE_MONO16, holo_buf, holo_bytes, HOLO_RATE, 0, false);
}

void sound_play_shimmer(void) {
	if (!ready || !shim_buf || !holo_voice || ui_set.voice_volume <= 0) return;
	u16 vol = (u16)(ui_set.voice_volume * 255 / UI_VOLUME_MAX);
	AESND_SetVoiceVolume(holo_voice, vol, vol);
	AESND_PlayVoice(holo_voice, VOICE_MONO16, shim_buf, shim_bytes, HOLO_RATE, 0, false);
}

/* ------------------------------------------------------- the background */

void sound_ambience_tick(bool listening, bool talking) {
	static bool playing;
	static float level;      /* 0..1: faded in, dipped, quiet */
	static u64 next_bell;
	if (!ready || !amb_buf || !amb_voice) return;
	u64 now = gettime();
	if (!playing) {
		if (!ui_intro_done()) return;
		AESND_SetVoiceVolume(amb_voice, 0, 0);
		AESND_PlayVoice(amb_voice, VOICE_MONO16, amb_buf, amb_bytes, AMB_RATE, 0, true);   /* (round and round) */
		playing = true;
		level = 0;
		next_bell = now + secs_to_ticks(7 + rand() % 6);
	}
	/* where it's heading: silent while listening, low while he talks */
	float target = listening ? 0 : talking ? 0.35f : 1;
	float rate = listening ? 0.25f : target > level ? 0.012f : 0.05f;   /* (in slowly: about three seconds the first time) */
	level += (target - level) * rate;
	/* as loud as in the sample at Menu sounds 4; louder above, quieter below; off with them */
	float base = ui_set.sound_volume * 0.125f;
	if (base > 1) base = 1;
	u16 vol = (u16)(255 * base * level);
	AESND_SetVoiceVolume(amb_voice, vol, vol);
	/* now and then, a soft bell note far off (not while he talks or listens) */
	if (now >= next_bell) {
		next_bell = now + secs_to_ticks(7 + rand() % 6);
		int which = rand() % AMB_BELLS;
		if (bell_voice && bell_buf[which] && !listening && !talking && level > 0.8f && vol > 0) {
			AESND_SetVoiceVolume(bell_voice, vol, vol);
			AESND_PlayVoice(bell_voice, VOICE_MONO16, bell_buf[which], bell_bytes, AMB_RATE, 0, false);
		}
	}
}

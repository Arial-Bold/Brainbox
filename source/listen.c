/* listen.c - see listen.h. Uses libogc2's microphone driver (ogc/mic.h): the
 * driver fills a ring of samples from the microphone; once a frame we copy
 * what's new into the recording. At the end the recording is cleaned up (the
 * hum taken out, the loudness evened out) and brought to LISTEN_RATE. */
#include "listen.h"
#include <gccore.h>
#include <ogc/mic.h>
#include <ogc/lwp_watchdog.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MIC_HZ 22050
#define RING_BYTES (32 * 1024)                     /* about 0.7 s of slack */
#define REC_MAX (MIC_HZ * LISTEN_MAX_SECONDS)

static s16 ring[RING_BYTES / 2] ATTRIBUTE_ALIGN(32);
static s16 *rec;                                   /* the recording, at MIC_HZ */
static short *out;                                 /* ...and as it's sent */
static int nrec, rd, ring_samples;
static int chan = -1;                              /* the slot the microphone is in (-1: none found) */
static volatile bool mounted;
static bool recording;
static float level;
static u64 last_look;
static int gain = 15;
static bool other[2];                              /* something else is in that slot (an SD card adapter, a memory card) */

static void detached(s32 c, s32 result) { (void)c; (void)result; mounted = false; chan = -1; }

void listen_init(void) {
	MIC_Init();
	rec = malloc(REC_MAX * sizeof(s16));
	out = malloc((size_t)LISTEN_RATE * LISTEN_MAX_SECONDS * sizeof(short) + 64);
}

static bool mount(void) {
	if (mounted && chan >= 0 && MIC_IsAttached(chan)) return true;
	mounted = false;
	for (int c = 0; c < 2; c++) {
		/* (a slot with something else in it is left alone until that comes out) */
		if (other[c]) { if (EXI_ProbeEx(c) < 0) other[c] = false; else continue; }
		s32 r = MIC_ProbeEx(c);
		if (r == MIC_RESULT_WRONGDEVICE) other[c] = true;
		if (r != MIC_RESULT_READY) continue;
		if (MIC_Mount(c, ring, sizeof(ring), detached) != MIC_RESULT_READY) continue;
		if (MIC_SetParams(c, 128, MIC_HZ, gain) != MIC_RESULT_READY) { MIC_Unmount(c); continue; }
		s32 sz = 0;
		MIC_GetRingBufferSize(c, &sz);
		ring_samples = sz / 2;
		if (ring_samples <= 0) { MIC_Unmount(c); continue; }
		chan = c;
		mounted = true;
		return true;
	}
	chan = -1;
	return false;
}

bool listen_present(void) {
	if (!rec || !out) return false;
	if (mounted && chan >= 0 && MIC_IsAttached(chan)) return true;
	/* (looking takes the memory card slots for a moment: not too often) */
	u64 now = ticks_to_millisecs(gettime());
	if (last_look && now - last_look < 1500) return false;
	last_look = now;
	return mount();
}

bool listen_start(void) {
	if (!rec || !out || !mount()) return false;
	nrec = 0;
	level = 0;
	if (MIC_Start(chan) != MIC_RESULT_READY) { MIC_Unmount(chan); mounted = false; chan = -1; return false; }
	rd = MIC_GetCurrentTop(chan);
	if (rd < 0) rd = 0;
	recording = true;
	return true;
}

void listen_update(void) {
	if (!recording || chan < 0) return;
	int top = MIC_GetCurrentTop(chan);
	if (top < 0) return;
	DCInvalidateRange(ring, sizeof(ring));
	int got = 0;
	double sum = 0, mean = 0;
	int first = nrec;
	while (rd != top && nrec < REC_MAX) {
		rec[nrec++] = ring[rd];
		if (++rd >= ring_samples) rd = 0;
		got++;
	}
	if (got > 0) {
		/* how loud (round its own middle, whatever the microphone's resting level) */
		for (int i = first; i < nrec; i++) mean += rec[i];
		mean /= got;
		bool offset = fabs(mean) > 16000;   /* (unsigned samples: the middle is 0x8000) */
		if (offset) { mean = 0; for (int i = first; i < nrec; i++) mean += (s16)(rec[i] ^ 0x8000); mean /= got; }
		for (int i = first; i < nrec; i++) {
			double v = (offset ? (s16)(rec[i] ^ 0x8000) : rec[i]) - mean;
			sum += v * v;
		}
		float rms = sqrtf((float)(sum / got)) / 3000.0f;
		if (rms > 1) rms = 1;
		level = rms > level ? rms : level * 0.8f + rms * 0.2f;
	} else level *= 0.9f;
}

float listen_level(void) { return level; }
float listen_seconds(void) { return nrec / (float)MIC_HZ; }
bool listen_full(void) { return nrec >= REC_MAX; }

bool listen_stop(const short **pcm, int *n) {
	*pcm = out;
	*n = 0;
	if (!recording) return false;
	listen_update();
	MIC_Stop(chan);
	recording = false;
	level = 0;
	if (nrec < MIC_HZ / 4) return false;   /* a quick tap: nothing said */

	/* resting level: signed or unsigned samples */
	double mean = 0;
	for (int i = 0; i < nrec; i++) mean += rec[i];
	mean /= nrec;
	if (fabs(mean) > 16000) for (int i = 0; i < nrec; i++) rec[i] ^= 0x8000;

	/* the first few hundredths of a second are often a click */
	int skip = MIC_HZ / 40;
	for (int i = 0; i < skip && i < nrec; i++) rec[i] = rec[skip < nrec ? skip : nrec - 1];

	/* take the hum out (a gentle high-pass), and soften the top end before
	 * bringing it down to LISTEN_RATE */
	float hp_x = rec[0], hp_y = 0, lp = 0, peak = 0;
	float *f = (float *)malloc(nrec * sizeof(float));
	if (!f) return false;
	for (int i = 0; i < nrec; i++) {
		float x = rec[i];
		hp_y = 0.995f * (hp_y + x - hp_x);
		hp_x = x;
		lp += 0.55f * (hp_y - lp);
		f[i] = lp;
		float a = fabsf(lp);
		if (a > peak) peak = a;
	}
	if (peak < 300) { free(f); return false; }   /* too quiet: nothing said */
	float g = 26000.0f / peak;
	if (g > 12) g = 12;
	int m = (int)((long long)nrec * LISTEN_RATE / MIC_HZ);
	for (int j = 0; j < m; j++) {
		float pos = j * (float)MIC_HZ / LISTEN_RATE;
		int i = (int)pos;
		float t = pos - i;
		float v = i + 1 < nrec ? f[i] * (1 - t) + f[i + 1] * t : f[nrec - 1];
		v *= g;
		if (v > 32767) v = 32767;
		if (v < -32768) v = -32768;
		out[j] = (short)v;
	}
	free(f);
	*n = m;
	return true;
}

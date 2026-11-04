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
#include <stdio.h>

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

/* The microphone's samples are 16-bit, but which way round their bytes come,
 * and whether silence is 0 or 0x8000, isn't certain: the recording is tried
 * each way and the one that sounds like sound (smooth, not hiss) is used. */
static int fmt = -1;        /* bit 0: bytes swapped; bit 1: unsigned (silence at 0x8000) */
static char info[200];

static inline int decode(s16 raw, int f) {
	u16 v = (u16)raw;
	if (f & 1) v = (u16)(v << 8 | v >> 8);
	if (f & 2) v ^= 0x8000;
	return (s16)v;
}

/* how rough a stretch is: small for real sound (it changes gently from one
 * sample to the next), about 1 or more for hiss or a wrong reading */
static float roughness(const s16 *x, int n, int f) {
	if (n < 8) return 9;
	double mean = 0;
	for (int i = 0; i < n; i++) mean += decode(x[i], f);
	mean /= n;
	double steps = 0, size = 0;
	int prev = decode(x[0], f);
	for (int i = 1; i < n; i++) {
		int v = decode(x[i], f);
		steps += fabs((double)(v - prev));
		size += fabs(v - mean);
		prev = v;
	}
	return size > 1 ? (float)(steps / size) : 9;
}

/* how far from silence it sits, on average */
static double offness(const s16 *x, int n, int f) {
	double a = 0;
	for (int i = 0; i < n; i++) a += fabs((double)decode(x[i], f));
	return n ? a / n : 0;
}

static int best_format(const s16 *x, int n) {
	int best = 0;
	float br = 1e9f;
	for (int order = 0; order < 2; order++) {
		/* signed or unsigned: the one whose silence sits at 0 */
		int f = offness(x, n, order) <= offness(x, n, order | 2) ? order : order | 2;
		/* which way round the bytes are: the one that's smooth, not hiss */
		float r = roughness(x, n, f);
		if (r < br) { br = r; best = f; }
	}
	return best;
}

void listen_update(void) {
	if (!recording || chan < 0) return;
	int top = MIC_GetCurrentTop(chan);
	if (top < 0) return;
	DCInvalidateRange(ring, sizeof(ring));
	int got = 0;
	int first = nrec;
	while (rd != top && nrec < REC_MAX) {
		rec[nrec++] = ring[rd];
		if (++rd >= ring_samples) rd = 0;
		got++;
	}
	if (got > 32) {
		/* how loud (round its own middle) */
		int f = fmt >= 0 ? fmt : best_format(rec + first, got);
		double mean = 0, sum = 0;
		for (int i = first; i < nrec; i++) mean += decode(rec[i], f);
		mean /= got;
		for (int i = first; i < nrec; i++) { double v = decode(rec[i], f) - mean; sum += v * v; }
		float rms = sqrtf((float)(sum / got)) / 1500.0f;
		if (rms > 1) rms = 1;
		level = rms > level ? rms : level * 0.8f + rms * 0.2f;
	} else if (!got) level *= 0.9f;
}

float listen_level(void) { return level; }
float listen_seconds(void) { return nrec / (float)MIC_HZ; }
bool listen_full(void) { return nrec >= REC_MAX; }
const char *listen_info(void) { return info; }

bool listen_button(void) {
	if (!mounted || chan < 0) return false;
	u32 b = 0;
	if (MIC_GetButton(chan, &b) != MIC_RESULT_READY) return false;
	return b != 0;
}

bool listen_stop(const short **pcm, int *n) {
	*pcm = out;
	*n = 0;
	info[0] = 0;
	if (!recording) return false;
	listen_update();
	MIC_Stop(chan);
	recording = false;
	level = 0;
	if (nrec < MIC_HZ / 4) { snprintf(info, sizeof(info), "too short (%d samples)", nrec); return false; }   /* a quick tap */

	/* which way round the samples are (decided once, from a good long recording) */
	int skip = MIC_HZ / 40;   /* (the first few hundredths of a second are often a click) */
	int f = best_format(rec + skip, nrec - skip);
	float rough = roughness(rec + skip, nrec - skip, f);
	if (nrec > MIC_HZ && rough < 0.6f) fmt = f;

	/* take the hum out (a gentle high-pass), and soften the top end before
	 * bringing it down to LISTEN_RATE */
	float *x = (float *)malloc(nrec * sizeof(float));
	if (!x) return false;
	for (int i = 0; i < nrec; i++) x[i] = (float)decode(rec[i < skip ? skip : i], f);
	float hp_x = x[0], hp_y = 0, lp = 0, peak = 0;
	double energy = 0;
	for (int i = 0; i < nrec; i++) {
		hp_y = 0.995f * (hp_y + x[i] - hp_x);
		hp_x = x[i];
		lp += 0.55f * (hp_y - lp);
		x[i] = lp;
		if (i > MIC_HZ / 10) {   /* (once the filter has settled) */
			float a = fabsf(lp);
			if (a > peak) peak = a;
			energy += (double)lp * lp;
		}
	}
	float rms = sqrtf((float)(energy / (nrec > MIC_HZ / 10 ? nrec - MIC_HZ / 10 : 1)));
	/* even it out: the loud parts to about three quarters of full */
	float g = peak > 1 ? 24000.0f / peak : 1;
	if (g > 60) g = 60;
	int m = (int)((long long)nrec * LISTEN_RATE / MIC_HZ);
	for (int j = 0; j < m; j++) {
		float pos = j * (float)MIC_HZ / LISTEN_RATE;
		int i = (int)pos;
		float t = pos - i;
		float v = i + 1 < nrec ? x[i] * (1 - t) + x[i + 1] * t : x[nrec - 1];
		v *= g;
		if (v > 32767) v = 32767;
		if (v < -32768) v = -32768;
		out[j] = (short)v;
	}
	free(x);
	*n = m;
	snprintf(info, sizeof(info), "%.1f s, slot %c, format %d (roughness %.2f), peak %.0f, average %.0f, boost x%.1f, overruns %u",
	         nrec / (float)MIC_HZ, 'A' + chan, f, rough, peak, rms, g, (unsigned)MIC_GetErrorCount(chan));
	return peak >= 120;   /* (quieter: nothing said) */
}

/* the recording as it's sent, as a .wav file (for checking on a computer) */
bool listen_wav(const short *pcm, int n, unsigned char **data, int *len) {
	int total = 44 + n * 2;
	unsigned char *w = malloc(total);
	if (!w) return false;
	#define LE32(p, v) do { (p)[0] = (v) & 255; (p)[1] = ((v) >> 8) & 255; (p)[2] = ((v) >> 16) & 255; (p)[3] = ((v) >> 24) & 255; } while (0)
	memcpy(w, "RIFF", 4); LE32(w + 4, (unsigned)(total - 8)); memcpy(w + 8, "WAVEfmt ", 8);
	LE32(w + 16, 16u); w[20] = 1; w[21] = 0; w[22] = 1; w[23] = 0;
	LE32(w + 24, (unsigned)LISTEN_RATE); LE32(w + 28, (unsigned)LISTEN_RATE * 2); w[32] = 2; w[33] = 0; w[34] = 16; w[35] = 0;
	memcpy(w + 36, "data", 4); LE32(w + 40, (unsigned)(n * 2));
	for (int i = 0; i < n; i++) { w[44 + i * 2] = pcm[i] & 255; w[45 + i * 2] = (pcm[i] >> 8) & 255; }
	#undef LE32
	*data = w;
	*len = total;
	return true;
}

/****************************************************************************
* ui.c - see ui.h
*
* Drawn in a 640x480 layout. The projection widens what's visible for 16:9
* (TVs that stretch the picture) and pulls everything in from the edges for
* overscan, so the screens themselves never need to know about either.
*
* The stage: a deep indigo room with a grid floor, a glowing pedestal and a
* beam of light, and the professor floating in it (pixel3d.c), with his
* reflection on the floor, hologram scanlines, sparkles and a glow: the bright
* parts of each frame are copied out at half size and added back, softened.
* The panels, rows, hints and description bar are the System look of the
* other apps (GCMM exFAT, Flashcube).
****************************************************************************/
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "ui.h"
#include "ui_assets.h"
#include "pixel3d.h"
#include "speech.h"
#include "net/storage.h"

/* ================================================================ settings */

UiSettings ui_set = { UI_RES_AUTO, 0, 1, 0, 8, 4, 1, 3 /* Groq */, "", { "", "", "", "" }, { "", "", "", "" }, 0, 1, 0 };
static char settings_file[300];
static const char *brain_ids[UI_BRAINS] = { "gemini", "claude", "openai", "groq" };

void ui_settings_path(const char *path) { snprintf(settings_file, sizeof(settings_file), "%s", path); }

const char *ui_resolution_name(int r) {
	switch (r) {
	case UI_RES_480I: return "480i";
	case UI_RES_480P: return "480p";
	default: return "Auto";
	}
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void ui_settings_load(void) {
	if (!settings_file[0]) return;
	FILE *f = fopen(settings_file, "r");
	if (!f) return;
	char line[400];
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *val = eq + 1;
		char *nl = strpbrk(val, "\r\n");
		if (nl) *nl = 0;
		int v = atoi(val);
		if (!strcmp(line, "display.resolution")) ui_set.resolution = clampi(v, 0, UI_RES_COUNT - 1);
		else if (!strcmp(line, "display.widescreen")) ui_set.widescreen = v ? 1 : 0;
		else if (!strcmp(line, "display.full_width")) ui_set.full_width = v ? 1 : 0;
		else if (!strcmp(line, "display.overscan")) ui_set.overscan = clampi(v, 0, UI_OVERSCAN_MAX);
		else if (!strcmp(line, "display.depth_flip")) ui_set.depth_flip = v ? 1 : 0;
		else if (!strcmp(line, "display.glow")) ui_set.glow = v ? 1 : 0;
		else if (!strcmp(line, "display.fps")) ui_set.fps = v ? 1 : 0;
		else if (!strcmp(line, "sound.voice")) ui_set.voice_volume = clampi(v, 0, UI_VOLUME_MAX);
		else if (!strcmp(line, "sound.menu")) ui_set.sound_volume = clampi(v, 0, UI_VOLUME_MAX);
		else if (!strcmp(line, "chat.captions")) ui_set.captions = v ? 1 : 0;
		else if (!strcmp(line, "exit.launcher")) snprintf(ui_set.launcher, sizeof(ui_set.launcher), "%s", val);
		else if (!strcmp(line, "brain")) { for (int i = 0; i < UI_BRAINS; i++) if (!strcmp(val, brain_ids[i])) ui_set.brain = i; }
		else for (int i = 0; i < UI_BRAINS; i++) {
			char k[40];
			snprintf(k, sizeof(k), "%s.key", brain_ids[i]);
			if (!strcmp(line, k)) snprintf(ui_set.key[i], UI_KEY_MAX, "%s", val);
			snprintf(k, sizeof(k), "%s.model", brain_ids[i]);
			if (!strcmp(line, k)) snprintf(ui_set.model[i], sizeof(ui_set.model[i]), "%s", val);
		}
	}
	fclose(f);
	ui_settings_changed();
}

bool ui_settings_save(void) {
	if (!settings_file[0]) return false;
	static char out[2400];
	int n = snprintf(out, sizeof(out),
		"# Professor Pixel's settings\n"
		"display.resolution=%d\ndisplay.widescreen=%d\ndisplay.full_width=%d\ndisplay.overscan=%d\n"
		"sound.voice=%d\nsound.menu=%d\nchat.captions=%d\nbrain=%s\nexit.launcher=%s\n",
		ui_set.resolution, ui_set.widescreen, ui_set.full_width, ui_set.overscan,
		ui_set.voice_volume, ui_set.sound_volume, ui_set.captions, brain_ids[clampi(ui_set.brain, 0, UI_BRAINS - 1)], ui_set.launcher);
	for (int i = 0; i < UI_BRAINS; i++)
		n += snprintf(out + n, sizeof(out) - n, "%s.key=%s\n%s.model=%s\n", brain_ids[i], ui_set.key[i], brain_ids[i], ui_set.model[i]);
	if (ui_set.depth_flip) n += snprintf(out + n, sizeof(out) - n, "display.depth_flip=1\n");
	if (!ui_set.glow) n += snprintf(out + n, sizeof(out) - n, "display.glow=0\n");
	if (ui_set.fps) n += snprintf(out + n, sizeof(out) - n, "display.fps=1\n");
	return storage_write_atomic(settings_file, out, (size_t)n);
}

/* ============================================================ the scene */

#define TXT 120
typedef struct { char label[48]; char value[TXT]; char tag[20]; bool gold; } Pair;

static struct {
	int screen;
	char status1[TXT], status2[TXT];
	char hints[UI_HINTS_MAX][32];
	int hint_count;
	char toast[96];
	u32 toast_at;
	/* the stage */
	int side;
	float think, brows, fog;
	bool props;
	u32 chuckle_at;
	int cap_kind;
	char cap[160];
	float listen;      /* the microphone's loudness, 0..1 */
	float listen_shown;
	char typing[400];
	bool masked;
	char you[400], him[4000];
	int him_scroll, him_more;
	/* panels */
	int overlay;
	char overlay_title[64];
	Pair orows[UI_OVERLAY_ROWS];
	int orow_count, osel;
	char note_title[48], note[200];
	unsigned char qr[177 * 177];
	int qr_size;
	char addr[64], sign_state[400];
} sc;

static mutex_t lock = LWP_MUTEX_NULL;
static void L(void) { LWP_MutexLock(lock); }
static void U(void) { LWP_MutexUnlock(lock); }
static void cpy(char *dst, const char *src, size_t n) { snprintf(dst, n, "%s", src ? src : ""); }

static u32 now_ms(void) { return ticks_to_millisecs(gettime()); }

void ui_screen(int s) { L(); sc.screen = s; U(); }
int ui_get_screen(void) { return sc.screen; }
void ui_status(const char *a, const char *b) { L(); cpy(sc.status1, a, TXT); cpy(sc.status2, b, TXT); U(); }
void ui_hints(const char *const *h, int n) {
	L();
	sc.hint_count = n > UI_HINTS_MAX ? UI_HINTS_MAX : n;
	for (int i = 0; i < sc.hint_count; i++) cpy(sc.hints[i], h[i], sizeof(sc.hints[i]));
	U();
}
void ui_toast(const char *m) { L(); cpy(sc.toast, m, sizeof(sc.toast)); sc.toast_at = now_ms(); U(); }
void ui_layout(int side) { L(); sc.side = side; U(); }
void ui_pose(float think, float brows, bool props) { L(); sc.think = think; sc.brows = brows; sc.props = props; U(); }
void ui_chuckle(void) { L(); sc.chuckle_at = now_ms(); U(); }
void ui_fog(float a) { L(); sc.fog = a; U(); }
void ui_caption(int kind, const char *t) { L(); sc.cap_kind = kind; cpy(sc.cap, t, sizeof(sc.cap)); U(); }
void ui_listen_level(float lv) { sc.listen = lv; }
void ui_typing(const char *t, bool masked) { L(); cpy(sc.typing, t, sizeof(sc.typing)); sc.masked = masked; U(); }
void ui_chat(const char *you, const char *him) { L(); cpy(sc.you, you, sizeof(sc.you)); cpy(sc.him, him, sizeof(sc.him)); sc.him_scroll = 0; U(); }
void ui_chat_scroll(int lines) {
	L();
	if (lines < 0) sc.him_scroll = 0;
	else if (lines > 0 && sc.him_more > 0) sc.him_scroll += lines;
	else if (lines > 0) sc.him_scroll = sc.him_scroll;   /* (already at the end) */
	U();
}
int ui_chat_more(void) { return sc.him_more; }
void ui_overlay(int o, const char *t) {
	L();
	sc.overlay = o;
	cpy(sc.overlay_title, t, sizeof(sc.overlay_title));
	sc.orow_count = 0;
	sc.osel = 0;
	memset(sc.orows, 0, sizeof(sc.orows));
	sc.note_title[0] = sc.note[0] = 0;
	U();
}
void ui_overlay_row(int r, const char *l, const char *v) {
	if (r < 0 || r >= UI_OVERLAY_ROWS) return;
	L();
	cpy(sc.orows[r].label, l, sizeof(sc.orows[r].label));
	if (v) cpy(sc.orows[r].value, v, TXT); else sc.orows[r].value[0] = 1;
	U();
}
void ui_overlay_tag(int r, const char *t, bool gold) {
	if (r < 0 || r >= UI_OVERLAY_ROWS) return;
	L(); cpy(sc.orows[r].tag, t, sizeof(sc.orows[r].tag)); sc.orows[r].gold = gold; U();
}
void ui_overlay_note(const char *t, const char *x) { L(); cpy(sc.note_title, t, sizeof(sc.note_title)); cpy(sc.note, x, sizeof(sc.note)); U(); }
void ui_overlay_select(int r) { L(); sc.osel = r; U(); }
void ui_overlay_rows(int n) { L(); sc.orow_count = n; U(); }
void ui_signin(const unsigned char *qr, int size, const char *addr, const char *state) {
	L();
	if (qr && size > 0 && size <= 177) { memcpy(sc.qr, qr, (size_t)size * size); sc.qr_size = size; } else sc.qr_size = 0;
	cpy(sc.addr, addr, sizeof(sc.addr));
	cpy(sc.sign_state, state, sizeof(sc.sign_state));
	U();
}

/* ================================================================== video */

#define FIFO_SIZE (256 * 1024)
static void *fifo;
static u32 *xfb[2];
static int fb;
static GXRModeObj mode;
static GXRModeObj *rmode = &mode;
static volatile bool video_change, running;
static lwp_t thread = LWP_THREAD_NULL;

GXRModeObj *ui_video_mode(void) { return rmode; }

/* The GPU draws 640 pixels across. Full width stretches that to 704, the
 * whole width of the TV picture (as games do); Standard leaves it at 640. */
static void pick_mode(void) {
	bool pal = VIDEO_GetCurrentTvMode() == VI_PAL;
	GXRModeObj *base;
	if (ui_set.resolution == UI_RES_480P && VIDEO_HaveComponentCable())
		base = pal ? &TVEurgb60Hz480Prog : &TVNtsc480Prog;
	else if (ui_set.resolution == UI_RES_480I)
		base = pal ? &TVPal528IntDf : &TVNtsc480IntDf;
	else if (VIDEO_HaveComponentCable())
		base = pal ? &TVEurgb60Hz480Prog : &TVNtsc480Prog;
	else
		base = VIDEO_GetPreferredMode(NULL);
	mode = *base;
	if (ui_set.full_width) {
		mode.viWidth = 704;
		mode.viXOrigin = (720 - 704) / 2;
	} else {
		mode.viWidth = 640;
		mode.viXOrigin = (720 - 640) / 2;
	}
}

static void setup_gx(void) {
	f32 yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
	u32 xfb_h = GX_SetDispCopyYScale(yscale);
	GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
	GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopyDst(rmode->fbWidth, xfb_h);
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
	GX_SetFieldMode(rmode->field_rendering, (rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE);
	GX_SetPixelFmt(rmode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	GX_SetDispCopyGamma(GX_GM_1_0);
}

static void configure_vi(void) {
	VIDEO_Configure(rmode);
	VIDEO_ClearFrameBuffer(rmode, xfb[0], COLOR_BLACK);
	VIDEO_ClearFrameBuffer(rmode, xfb[1], COLOR_BLACK);
	VIDEO_SetNextFramebuffer(xfb[0]);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
	fb = 1;
}

void ui_settings_changed(void) { video_change = true; }

/* ============================================================== drawing */

typedef struct { u8 r, g, b, a; } Color;
static inline Color rgba(u8 r, u8 g, u8 b, u8 a) { return (Color){ r, g, b, a }; }
static inline Color alpha(Color c, float k) { if (k < 0) k = 0; if (k > 1) k = 1; c.a = (u8)(c.a * k); return c; }
static inline float clamp01(float t) { return t < 0 ? 0 : t > 1 ? 1 : t; }
static Color mix(Color a, Color b, float t) {
	t = clamp01(t);
	return rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

static const Color TEXT = { 238, 241, 246, 255 }, SOFT = { 201, 210, 238, 255 }, TITLE = { 115, 146, 255, 255 };
static const Color GOLD = { 232, 207, 74, 255 }, GOLD_HI = { 255, 244, 150, 255 }, DIM = { 124, 134, 146, 255 };

/* the visible part of the layout */
static float vl, vt, vr, vb;
static void view_bounds(void) {
	float w = ui_set.widescreen ? 640.0f * 4.0f / 3.0f : 640.0f, h = 480.0f;
	float k = 1.0f - ui_set.overscan / 100.0f;
	w /= k; h /= k;
	vl = 320 - w / 2; vr = 320 + w / 2; vt = 240 - h / 2; vb = 240 + h / 2;
}
static float pixel_w(void) { return (vr - vl) / rmode->fbWidth; }
static float pixel_h(void) { return (vb - vt) / rmode->efbHeight; }

enum { M_NONE, M_COLOR, M_TEX, M_ALPHA };
static int cur_mode = M_NONE;
static void mode_color(void) {
	if (cur_mode == M_COLOR) return;
	cur_mode = M_COLOR;
	GX_SetNumTexGens(0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
}
static void mode_tex(GXTexObj *t, bool alpha_only) {
	GX_LoadTexObj(t, GX_TEXMAP0);
	int m = alpha_only ? M_ALPHA : M_TEX;
	if (cur_mode == m) return;
	cur_mode = m;
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	if (alpha_only) {
		/* text: the vertex colour, with the font's coverage as alpha */
		GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
		GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
		GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	} else {
		GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	}
}

static inline void vtx(float x, float y, Color c, float s, float t) {
	GX_Position2f32(x, y);
	GX_Color4u8(c.r, c.g, c.b, c.a);
	GX_TexCoord2f32(s, t);
}

static void rect_v(float x, float y, float w, float h, Color top, Color bot) {
	if (!(w > 0) || !(h > 0)) return;
	mode_color();
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	vtx(x, y, top, 0, 0); vtx(x + w, y, top, 0, 0); vtx(x + w, y + h, bot, 0, 0); vtx(x, y + h, bot, 0, 0);
	GX_End();
}
static void rect_h(float x, float y, float w, float h, Color l, Color r) {
	if (!(w > 0) || !(h > 0)) return;
	mode_color();
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	vtx(x, y, l, 0, 0); vtx(x + w, y, r, 0, 0); vtx(x + w, y + h, r, 0, 0); vtx(x, y + h, l, 0, 0);
	GX_End();
}
static void rect(float x, float y, float w, float h, Color c) { rect_v(x, y, w, h, c, c); }
static void tri(float x0, float y0, float x1, float y1, float x2, float y2, Color c) {
	mode_color();
	GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 3);
	vtx(x0, y0, c, 0, 0); vtx(x1, y1, c, 0, 0); vtx(x2, y2, c, 0, 0);
	GX_End();
}
static void circle(float cx, float cy, float r, Color c) {
	mode_color();
	GX_Begin(GX_TRIANGLEFAN, GX_VTXFMT0, 26);
	vtx(cx, cy, c, 0, 0);
	for (int i = 0; i <= 24; i++) {
		float a = 6.2831853f * i / 24;
		vtx(cx + cosf(a) * r, cy + sinf(a) * r, c, 0, 0);
	}
	GX_End();
}

#define RR_SEG 4
#define RR_PTS (4 * (RR_SEG + 1))
static int rr_points(float x, float y, float w, float h, float r, float *px, float *py) {
	if (r > w / 2) r = w / 2;
	if (r > h / 2) r = h / 2;
	if (r < 0) r = 0;
	const float cx[4] = { x + w - r, x + w - r, x + r, x + r }, cy[4] = { y + r, y + h - r, y + h - r, y + r };
	int n = 0;
	for (int c = 0; c < 4; c++) {
		float a0 = (-90.0f + 90.0f * c) * 3.14159265f / 180.0f;
		for (int i = 0; i <= RR_SEG; i++) {
			float a = a0 + 3.14159265f * 0.5f * i / RR_SEG;
			px[n] = cx[c] + cosf(a) * r; py[n] = cy[c] + sinf(a) * r; n++;
		}
	}
	return n;
}
static void rrect_v(float x, float y, float w, float h, float r, Color top, Color bot) {
	if (!(w > 0) || !(h > 0)) return;
	float px[RR_PTS], py[RR_PTS];
	int n = rr_points(x, y, w, h, r, px, py);
	mode_color();
	GX_Begin(GX_TRIANGLEFAN, GX_VTXFMT0, n + 2);
	vtx(x + w / 2, y + h / 2, mix(top, bot, 0.5f), 0, 0);
	for (int i = 0; i <= n; i++) { int k = i % n; vtx(px[k], py[k], mix(top, bot, (py[k] - y) / h), 0, 0); }
	GX_End();
}
static void outline(float x, float y, float w, float h, float r, float t, Color c) {
	if (!(w > 0) || !(h > 0)) return;
	float ux = pixel_w() * 1.02f, uy = pixel_h() * 1.02f;   /* never thinner than a pixel */
	float tx = t < ux ? ux : t, ty = t < uy ? uy : t;
	if (2 * tx >= w || 2 * ty >= h) { rrect_v(x, y, w, h, r, c, c); return; }
	float ox[RR_PTS], oy[RR_PTS], ix[RR_PTS], iy[RR_PTS];
	int n = rr_points(x, y, w, h, r, ox, oy);
	float ri = r - (tx > ty ? tx : ty);
	rr_points(x + tx, y + ty, w - 2 * tx, h - 2 * ty, ri > 0 ? ri : 0, ix, iy);
	mode_color();
	GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT0, (n + 1) * 2);
	for (int i = 0; i <= n; i++) { int k = i % n; vtx(ox[k], oy[k], c, 0, 0); vtx(ix[k], iy[k], c, 0, 0); }
	GX_End();
}
static void shadow(float x, float y, float w, float h, float r, float spread, Color c) {
	for (int i = 4; i >= 1; i--) {
		float s = spread * i / 4;
		Color k = alpha(c, 0.28f);
		rrect_v(x - s, y - s, w + 2 * s, h + 2 * s, r + s, k, k);
	}
}

/* ---------------------------------------------------------- textures */

static GXTexObj tex_font_small, tex_font_large;

static void tex_init(GXTexObj *t, const void *data, int w, int h, u8 fmt) {
	DCFlushRange((void *)data, GX_GetTexBufferSize(w, h, fmt, GX_FALSE, 0));
	GX_InitTexObj(t, (void *)data, w, h, fmt, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(t, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
}

static void image(GXTexObj *t, float x, float y, float w, float h, float tw, float th, Color c) {
	mode_tex(t, false);
	float s1 = w > 0 ? 1.0f : 0, t1 = 1.0f;
	(void)tw; (void)th;
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	vtx(x, y, c, 0, 0); vtx(x + w, y, c, s1, 0); vtx(x + w, y + h, c, s1, t1); vtx(x, y + h, c, 0, t1);
	GX_End();
}
/* ---------------------------------------------------------------- text */

static const UiFont *font_for(float size, GXTexObj **t) {
	if (size <= 16) { *t = &tex_font_small; return &font_small; }
	*t = &tex_font_large; return &font_large;
}

static float text_width(const char *s, float size) {
	GXTexObj *t;
	const UiFont *f = font_for(size, &t);
	float k = size / f->px, w = 0;
	for (; *s; s++) w += f->glyphs[(u8)*s].adv16 / 16.0f * k;
	return w;
}

/* Text, vertically centred on y. align: 0 left, 1 centre, 2 right. max_w > 0
 * shortens it with an ellipsis to fit. A soft dark drop sits under it. */
static void text(float x, float y, const char *s, float size, Color c, int align, float max_w) {
	if (!s || !*s) return;
	char buf[160];
	snprintf(buf, sizeof(buf), "%s", s);
	if (max_w > 0 && text_width(buf, size) > max_w) {
		size_t n = strlen(buf);
		while (n > 0) {
			buf[--n] = 0;
			char tmp[164];
			snprintf(tmp, sizeof(tmp), "%s\x85", buf);
			if (text_width(tmp, size) <= max_w) { snprintf(buf, sizeof(buf), "%s", tmp); break; }
		}
	}
	GXTexObj *t;
	const UiFont *f = font_for(size, &t);
	float k = size / f->px;
	float w = text_width(buf, size);
	float px = align == 1 ? x - w / 2 : align == 2 ? x - w : x;
	/* the baseline, so that capitals sit centred on y */
	float base = y + (f->ascent - f->descent) * 0.5f * k * 0.9f;
	mode_tex(t, true);
	for (int pass = 0; pass < 2; pass++) {
		Color col = pass ? c : rgba(0, 0, 0, (u8)(c.a * 0.55f));
		float off = pass ? 0 : 1.0f;
		float pen = px;
		for (const char *p = buf; *p; p++) {
			const UiGlyph *g = &f->glyphs[(u8)*p];
			if (g->w && g->h && *p != ' ') {
				float gx = pen + g->left * k + off, gy = base + g->top * k + off;
				float s0 = (float)g->x / f->w, t0 = (float)g->y / f->h, s1 = (float)(g->x + g->w) / f->w, t1 = (float)(g->y + g->h) / f->h;
				GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
				vtx(gx, gy, col, s0, t0); vtx(gx + g->w * k, gy, col, s1, t0);
				vtx(gx + g->w * k, gy + g->h * k, col, s1, t1); vtx(gx, gy + g->h * k, col, s0, t1);
				GX_End();
			}
			pen += g->adv16 / 16.0f * k;
		}
	}
}

/* --------------------------------------------------------- pieces of the look */

static void panel(float x, float y, float w, float h) {
	shadow(x, y + 6, w, h, 6, 8, rgba(0, 0, 0, 120));
	outline(x - 3, y - 3, w + 6, h + 6, 6, 2, rgba(200, 208, 220, 140));
	outline(x - 1, y - 1, w + 2, h + 2, 5, 1, rgba(10, 13, 16, 255));
	rrect_v(x, y, w, h, 4, rgba(26, 46, 50, 240), rgba(14, 22, 26, 247));
	float h1 = h * 0.35f;
	rect_v(x, y + 3, w, h1, rgba(30, 54, 57, 0), rgba(40, 72, 74, 150));
	rect_v(x, y + 3 + h1, w, h * 0.4f, rgba(40, 72, 74, 150), rgba(30, 54, 57, 0));
	float e = fminf(14, fminf(w, h) * 0.3f);
	rect_h(x, y + 3, e, h - 6, rgba(0, 0, 0, 70), rgba(0, 0, 0, 0));
	rect_h(x + w - e, y + 3, e, h - 6, rgba(0, 0, 0, 0), rgba(0, 0, 0, 70));
	rect(x + 4, y + 1, w - 8, 1, rgba(255, 255, 255, 46));
	outline(x, y, w, h, 4, 1, rgba(255, 255, 255, 30));
}

static void frost(float x, float y, float w, float h) {
	rect_v(x, y, w, h, rgba(76, 94, 114, 205), rgba(40, 54, 70, 215));
	rect(x + 1, y + 1, w - 2, 1, rgba(255, 255, 255, 40));
	outline(x, y, w, h, 2, 1, rgba(215, 225, 240, 153));
}

static void gold_ring(float x, float y, float w, float h) {
	outline(x - 3, y - 3, w + 6, h + 6, 0, 3, GOLD);
	outline(x, y, w, h, 0, 1, rgba(106, 90, 16, 200));
}

static void desc_bar(float x, float y, float w, float h) {
	outline(x - 1, y - 1, w + 2, h + 2, 4, 1, rgba(11, 11, 32, 255));
	rrect_v(x, y, w, h, 3, rgba(42, 34, 140, 255), rgba(59, 44, 182, 255));
	outline(x, y, w, h, 3, 1, rgba(175, 175, 255, 153));
}

static void rule(float x, float y, float w) { rect(x, y, w, 1, rgba(200, 210, 230, 70)); }

/* ------------------------------------------------------- the background
 * As Flashcube's: near black, a 24px grid of 2px lines lined up on the
 * middle of the screen, and a soft dark vignette towards the edges. */

static void mesh(int cols, int rows, const float *xy, const Color *c) {
	mode_color();
	for (int r = 0; r < rows; r++) {
		GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT0, (cols + 1) * 2);
		for (int k = 0; k <= cols; k++)
			for (int d = 0; d < 2; d++) {
				int i = (r + d) * (cols + 1) + k;
				vtx(xy[i * 2], xy[i * 2 + 1], c[i], 0, 0);
			}
		GX_End();
	}
}


/* the description bar along the bottom, with a progress bar at its right when there is one */
static void status_bar(void) {
	desc_bar(22, 374, 596, 52);
	float tw = 572;
	text(38, 391, sc.status1, 13, TEXT, 0, tw);
	text(38, 410, sc.status2, 11, rgba(200, 205, 240, 255), 0, tw);
}

/* ------------------------------------------------- the controls (hints)
 * As CubeVerse draws them: a dark pill for each button (A green, B red, Z
 * purple), the Control Stick as a little stick, three dots, then what it does. */
#define HINT_Y 452

static void hint_stick(float x, float cy, float d) {
	float r = d / 2, cx = x + r;
	circle(cx, cy + 1, r + 1, rgba(0, 0, 0, 150));
	circle(cx, cy, r, rgba(0x4a, 0x4a, 0x52, 255));
	circle(cx, cy, r * 0.78f, rgba(0x2a, 0x2a, 0x30, 255));
	float nr = d * 0.30f;
	circle(cx, cy + 1, nr, rgba(0, 0, 0, 128));
	for (int i = 4; i >= 0; i--)
		circle(cx, cy, nr * (i + 1) / 5.0f, (i & 1) ? rgba(0x9a, 0x9b, 0xa6, 255) : rgba(0xd6, 0xd7, 0xde, 255));
}

static float hint_one(const char *h, float x, float cy, bool draw) {
	char key[8] = "";
	const char *colon = strchr(h, ':');
	const char *label = colon ? colon + 1 : h;
	if (colon && colon - h < 7) { memcpy(key, h, colon - h); key[colon - h] = 0; }
	float w = 0;
	if (!strcmp(key, "S")) {
		if (draw) hint_stick(x, cy, 20);
		w = 20;
	} else if (key[0]) {
		const char *t = !strcmp(key, "ST") ? "START" : key;
		Color tc = rgba(0xd8, 0xda, 0xe6, 255), ring = rgba(255, 255, 255, 36);
		bool glow = false;
		if (!strcmp(key, "A")) { tc = rgba(0x5f, 0xe3, 0x9a, 255); ring = rgba(95, 227, 154, 97); glow = true; }
		if (!strcmp(key, "B")) { tc = rgba(0xff, 0x5c, 0x7c, 255); ring = rgba(255, 92, 124, 97); glow = true; }
		if (!strcmp(key, "Z")) { tc = rgba(0xb9, 0xa2, 0xff, 255); ring = rgba(185, 162, 255, 97); glow = true; }
		if (!strcmp(key, "KB")) t = "Keyboard";
		if (!strcmp(key, "MIC")) { t = "Mic button"; tc = rgba(0x7c, 0xd8, 0xff, 255); ring = rgba(124, 216, 255, 97); glow = true; }
		float pw = text_width(t, 11) + 12;
		if (pw < 20) pw = 20;
		if (draw) {
			float y = cy - 10;
			if (glow) rrect_v(x - 2, y - 2, pw + 4, 24, 12, alpha(ring, 0.45f), alpha(ring, 0.45f));
			rrect_v(x, y, pw, 20, 10, rgba(0x3c, 0x3c, 0x46, 255), rgba(0x08, 0x08, 0x0a, 255));
			outline(x, y, pw, 20, 10, 1, ring);
			text(x + pw / 2, cy, t, 11, tc, 1, 0);
		}
		w = pw;
	}
	if (key[0]) {
		if (draw) for (int i = 0; i < 3; i++) circle(x + w + 4 + i * 4, cy + 1, 1.1f, rgba(0xc9, 0xcb, 0xd6, 255));
		w += 16;
	}
	if (draw) text(x + w, cy, label, 12, rgba(0xdf, 0xe4, 0xff, 255), 0, 0);
	return w + text_width(label, 12);
}

static void hints_row(void) {
	if (sc.hint_count <= 0) return;
	float total = 0, gap = 30;
	for (int i = 0; i < sc.hint_count; i++) total += hint_one(sc.hints[i], 0, 0, false);
	if (total + gap * (sc.hint_count - 1) > 580) gap = fmaxf(10, (580 - total) / (sc.hint_count - 1));
	float x = 320 - (total + gap * (sc.hint_count - 1)) / 2;
	for (int i = 0; i < sc.hint_count; i++) x += hint_one(sc.hints[i], x, HINT_Y, true) + gap;
}

/* the main screen's pictures */
/* a message across the top, fading in and out over about three seconds */
static void toast_draw(void) {
	if (!sc.toast[0]) return;
	float t = (now_ms() - sc.toast_at) / 1000.0f;
	float a = clamp01(t / 0.2f) * clamp01((3.2f - t) / 0.4f);
	if (a <= 0) return;
	float w = text_width(sc.toast, 13) + 40, x = 320 - w / 2, y = 22;   /* clear of the top edge (TVs cut a little off) */
	outline(x - 1, y - 1, w + 2, 34 + 2, 4, 1, rgba(11, 11, 32, (u8)(255 * a)));
	rrect_v(x, y, w, 34, 3, rgba(42, 34, 140, (u8)(240 * a)), rgba(59, 44, 182, (u8)(240 * a)));
	outline(x, y, w, 34, 3, 1, rgba(175, 175, 255, (u8)(153 * a)));
	text(320, y + 17, sc.toast, 13, rgba(0xec, 0xeb, 0xff, (u8)(255 * a)), 1, 0);
}


/* ============================================================== the stage */

#define HORIZON 372.0f
static float ease(float x) { x = clamp01(x); return x * x * (3 - 2 * x); }

/* a soft round glow: bright in the middle, nothing at the edge */
static void soft_blob(float cx, float cy, float rx, float ry, Color c) {
	mode_color();
	GX_Begin(GX_TRIANGLEFAN, GX_VTXFMT0, 26);
	vtx(cx, cy, c, 0, 0);
	Color e = alpha(c, 0);
	for (int i = 0; i <= 24; i++) {
		float a = 6.2831853f * i / 24;
		vtx(cx + cosf(a) * rx, cy + sinf(a) * ry, e, 0, 0);
	}
	GX_End();
}

/* The floor grid: drawn once into a texture, with soft (anti-aliased) lines
 * of the right thickness everywhere, then laid over the floor with the GPU's
 * filtering, so the lines hold steady and never break up into dots on the
 * TV (thin lines drawn as triangles step and shimmer). */
#define FLOOR_TW 1024
#define FLOOR_TH 144
#define FLOOR_SPAN 1280.0f   /* layout units across the texture (centred on him) */
static u8 floor_tex_data[FLOOR_TW * FLOOR_TH] ATTRIBUTE_ALIGN(32);
static GXTexObj floor_tex;
static bool floor_ready;

static void floor_make(void) {
	const float bottom = 480.0f, depth = bottom - HORIZON, tx = FLOOR_SPAN / FLOOR_TW;
	/* the lines across: further apart as they come nearer */
	float cy[6], ca[6];
	for (int k = 0; k < 6; k++) {
		float d = 1.0f / (1 + (5 - k) * 0.9f);
		cy[k] = HORIZON + 6 + (depth - 6) * d + 1;
		ca[k] = 0.15f + 0.6f * d;
	}
	for (int y = 0; y < FLOOR_TH; y++) {
		float Y = HORIZON + y + 0.5f, s = (Y - HORIZON) / depth;
		float spacing = 30 + 90 * s, half = 1.0f + 0.5f * s, fade = 0.8f * (s > 1.1f ? 1.1f : s);
		float across = 0;
		for (int k = 0; k < 6; k++) {
			float c = 1.5f - fabsf(Y - cy[k]);           /* 2 units thick, soft edged */
			c = c < 0 ? 0 : c > 1 ? 1 : c;
			if (c * ca[k] > across) across = c * ca[k];
		}
		for (int x = 0; x < FLOOR_TW; x++) {
			float X = (x + 0.5f) * tx - FLOOR_SPAN / 2;
			float cov = 0;
			int k0 = (int)floorf(X / spacing);
			for (int k = k0 - 1; k <= k0 + 2; k++) {
				if (k < -10 || k > 10) continue;
				/* the distance to this line square on to it (they lean outwards) */
				float slope = k * 90.0f / depth;
				float dist = fabsf(X - k * spacing) / sqrtf(1 + slope * slope);
				float c = half + 0.5f - dist;
				c = c < 0 ? 0 : c > 1 ? 1 : c;
				if (c > cov) cov = c;
			}
			float v = cov * fade;
			if (across > v) v = across;
			/* (I8 is stored in 8 x 4 blocks) */
			int bx = x >> 3, by = y >> 2;
			int idx = (by * (FLOOR_TW / 8) + bx) * 32 + (y & 3) * 8 + (x & 7);
			floor_tex_data[idx] = (u8)(v * 255 + 0.5f);
		}
	}
	DCFlushRange(floor_tex_data, sizeof(floor_tex_data));
	GX_InitTexObj(&floor_tex, floor_tex_data, FLOOR_TW, FLOOR_TH, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&floor_tex, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	floor_ready = true;
}

static void floor_draw(float cx) {
	if (!floor_ready) floor_make();
	mode_tex(&floor_tex, false);
	Color line = rgba(52, 52, 120, 255);
	float x0 = cx - FLOOR_SPAN / 2, x1 = cx + FLOOR_SPAN / 2, y0 = HORIZON, y1 = HORIZON + FLOOR_TH;
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	vtx(x0, y0, line, 0, 0); vtx(x1, y0, line, 1, 0); vtx(x1, y1, line, 1, 1); vtx(x0, y1, line, 0, 1);
	GX_End();
	/* (below the texture, if overscan shows more: plain floor) */
}

static void room(float cx) {
	float fx = vl, fy = vt, fw = vr - vl, fh = vb - vt;
	rect_v(fx, fy, fw, HORIZON - fy, rgba(7, 7, 18, 255), rgba(14, 12, 38, 255));
	rect_v(fx, HORIZON, fw, fy + fh - HORIZON, rgba(10, 9, 30, 255), rgba(16, 13, 40, 255));
	/* the floor: the apps' grid, in perspective, as a smooth (filtered) picture */
	floor_draw(cx);
	/* the pedestal: a glowing ring on the floor */
	float py = HORIZON + 30;
	soft_blob(cx, py, 125, 17, rgba(20, 30, 80, 200));
	mode_color();
	for (int pass = 0; pass < 3; pass++) {
		float w = pass == 0 ? 7 : pass == 1 ? 3.5f : 1.5f;
		Color c = pass == 0 ? rgba(90, 150, 255, 50) : pass == 1 ? rgba(110, 170, 255, 140) : rgba(200, 230, 255, 255);
		GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT0, 2 * 49);
		for (int i = 0; i <= 48; i++) {
			float a = 6.2831853f * i / 48, ca = cosf(a), sa = sinf(a);
			vtx(cx + ca * (125 - w), py + sa * (17 - w * 0.14f), c, 0, 0);
			vtx(cx + ca * (125 + w), py + sa * (17 + w * 0.14f), c, 0, 0);
		}
		GX_End();
	}
}

/* the beam: a cone of light from the pedestal up, brightest in the middle */
static void beam(float cx, float top, float k) {
	float py = HORIZON + 30;
	mode_color();
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
	Color mid = rgba(60, 100, 220, (u8)(120 * k)), side = rgba(60, 100, 220, 0), topc = rgba(60, 100, 220, 0);
	float hb = 125, ht = 70;
	GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 12);
	vtx(cx - hb, py, side, 0, 0); vtx(cx, py, mid, 0, 0); vtx(cx, top, topc, 0, 0);
	vtx(cx - hb, py, side, 0, 0); vtx(cx, top, topc, 0, 0); vtx(cx - ht, top, side, 0, 0);
	vtx(cx + hb, py, side, 0, 0); vtx(cx, py, mid, 0, 0); vtx(cx, top, topc, 0, 0);
	vtx(cx + hb, py, side, 0, 0); vtx(cx, top, topc, 0, 0); vtx(cx + ht, top, side, 0, 0);
	GX_End();
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
}

/* sparkles drifting up the beam */
static void sparkles(float cx, float t) {
	mode_color();
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
	unsigned seed = 7;
	for (int i = 0; i < 36; i++) {
		seed = seed * 1103515245u + 12345u; float r1 = ((seed >> 8) & 0xFFFF) / 65535.0f;
		seed = seed * 1103515245u + 12345u; float r2 = ((seed >> 8) & 0xFFFF) / 65535.0f;
		seed = seed * 1103515245u + 12345u; float r3 = ((seed >> 8) & 0xFFFF) / 65535.0f;
		float span = HORIZON - 40;
		float y = HORIZON - fmodf(r1 * span + t * (14 + 22 * r3), span);
		float spread = 40 + (HORIZON - y) * 0.22f;
		float x = cx + (r2 * 2 - 1) * spread + sinf(t * 0.7f + i) * 4;
		float a = 0.3f + 0.6f * r3;
		a *= clamp01((HORIZON - y) / 40) * clamp01((y - 40) / 60);
		soft_blob(x, y, 2.6f, 2.6f, rgba(150, 215, 255, (u8)(255 * a)));
	}
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
}

/* the glow: the frame copied at half size, its bright parts added back, softened */
#define BLOOM_W 320
#define BLOOM_H 288
static u8 bloom_buf[BLOOM_W * BLOOM_H * 2] __attribute__((aligned(32)));
static void bloom(void) {
	u16 w = rmode->fbWidth / 2, h = rmode->efbHeight / 2;
	if (!ui_set.glow || w > BLOOM_W || h > BLOOM_H) return;
	GX_SetTexCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetTexCopyDst(w, h, GX_TF_RGB565, GX_TRUE);
	GX_CopyTex(bloom_buf, GX_FALSE);
	GX_PixModeSync();
	GX_InvalidateTexAll();
	GXTexObj t;
	GX_InitTexObj(&t, bloom_buf, w, h, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&t, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	GX_LoadTexObj(&t, GX_TEXMAP0);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	/* (texture - a threshold) x 2: only what's bright comes through */
	GX_SetTevColor(GX_TEVREG0, (GXColor){ 190, 190, 200, 255 });
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
	static const float offs[4][2] = { { -6, -2 }, { 6, 2 }, { -2, 6 }, { 2, -6 } };
	for (int i = 0; i < 4; i++) {
		Color c = rgba(255, 255, 255, 36);
		float ox = offs[i][0], oy = offs[i][1];
		GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		vtx(vl + ox, vt + oy, c, 0, 0); vtx(vr + ox, vt + oy, c, 1, 0);
		vtx(vr + ox, vb + oy, c, 1, 1); vtx(vl + ox, vb + oy, c, 0, 1);
		GX_End();
	}
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	cur_mode = M_NONE;
}

/* ---------------------------------------------------- the professor's pose */

static PixelPose pose;
static float mouth, px_now = 320, think_now, brows_now, fog_now;
static u32 blink_at, last_ms;

static void pose_update(float t) {
	u32 now = now_ms();
	float dt = last_ms ? (now - last_ms) / 1000.0f : 0.016f;
	if (dt > 0.1f) dt = 0.1f;
	last_ms = now;
	/* his mouth follows his voice: opens fast, closes a little slower */
	float lv = speech_level();
	float want = lv > 0.04f ? clamp01(0.25f + lv * 1.6f) : 0;
	mouth += (want - mouth) * (want > mouth ? 0.6f : 0.35f);
	/* blinks every few seconds */
	if (!blink_at) blink_at = now + 2500;
	float blink = 0;
	if (now >= blink_at) {
		float k = (now - blink_at) / 160.0f;
		blink = k < 0.5f ? k * 2 : 2 - k * 2;
		if (k >= 1) { blink = 0; blink_at = now + 2000 + (now * 7919u) % 3500u; }
	}
	think_now += (sc.think - think_now) * clamp01(dt * 5);
	brows_now += (sc.brows + (mouth > 0.3f ? 0.3f : 0) - brows_now) * clamp01(dt * 6);
	fog_now += (sc.fog - fog_now) * clamp01(dt * 1.5f);
	float target_x = sc.side ? 196 : 320;
	px_now += (target_x - px_now) * clamp01(dt * 4);
	float ch = sc.chuckle_at ? 1 - (now - sc.chuckle_at) / 900.0f : 0;
	pose.t = t;
	pose.talk = mouth;
	pose.blink = blink;
	pose.think = think_now;
	pose.brows = brows_now;
	pose.chuckle = ch > 0 ? ch : 0;
	pose.look_x = think_now > 0.3f ? 0.6f : 0.25f * sinf(t * 0.37f) * sinf(t * 0.23f);
	pose.scribble = think_now;
	pose.props = sc.props && !(sc.side && sc.him[0] && ui_set.captions);   /* (out of the way of his answer) */
}

static void frame_setup(void);
static void head_pass(float x, float alpha_k, float mirror, PixelBox *box) {
	/* 3D, on the GPU (see pixel3d.c), then back to drawing in 2D */
	pixel3d_view(vl, vt, vr, vb, ui_set.depth_flip);
	pixel3d_draw(x, 240, 1, alpha_k, mirror, box);
	frame_setup();
}

static void stage(void) {
	float t = now_ms() / 1000.0f;
	pose_update(t);
	float cx = px_now;
	room(cx);
	pixel3d_build(&pose);
	/* his reflection on the floor (only below the horizon) */
	{
		float y_px = (HORIZON - vt) / (vb - vt) * rmode->efbHeight;
		if (y_px < 0) y_px = 0;
		GX_SetScissor(0, (u32)y_px, rmode->fbWidth, rmode->efbHeight - (u32)y_px);
		head_pass(cx, 0.16f, -0.95f, NULL);
		GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
	}
	beam(cx, 60, 1);
	PixelBox box;
	head_pass(cx, 1, -1000, &box);
	/* hologram scanlines over him */
	if (box.x1 > box.x0) {
		mode_color();
		float off = fmodf(t * 12, 4);
		for (float y = floorf(box.y0) - 4 + off; y < box.y1; y += 4) rect(box.x0, y, box.x1 - box.x0, 2, rgba(6, 10, 40, 40));
	}
	sparkles(cx, t);
	bloom();
	/* fog round his head (the foggy-headed greeting) */
	if (fog_now > 0.01f) {
		for (int i = 0; i < 14; i++) {
			float a = i * 2.39996f + t * (0.15f + 0.02f * i);
			float rr = 40 + (i % 4) * 18;
			soft_blob(cx + cosf(a) * rr, 200 + sinf(a * 1.3f) * rr * 0.6f, 60 + (i % 3) * 14, 36 + (i % 3) * 8,
			          rgba(200, 210, 235, (u8)(70 * fog_now)));
		}
	}
}

/* ------------------------------------------------------ the conversation */

/* word-wraps s to width w at size; calls line() for each line (n lines max) */
static int wrap(const char *s, float size, float w, char out[][160], int max) {
	int n = 0;
	while (*s && n < max) {
		while (*s == ' ') s++;
		if (*s == '\n') { s++; out[n++][0] = 0; continue; }
		int best = 0, i = 0;
		char buf[160];
		while (s[i] && s[i] != '\n' && i < 158) {
			buf[i] = s[i];
			buf[i + 1] = 0;
			if (text_width(buf, size) > w) break;
			if (s[i] == ' ') best = i;
			i++;
		}
		int take = (!s[i] || s[i] == '\n' || i >= 158) ? i : (best > 0 ? best : i);
		if (take <= 0) take = 1;
		memcpy(out[n], s, (size_t)take);
		out[n][take] = 0;
		n++;
		s += take;
		if (*s == '\n') s++;
	}
	return n;
}

static void bubble_frame(float x, float y, float w, float h, bool his) {
	Color edge = his ? rgba(175, 175, 255, 220) : rgba(215, 225, 240, 200);
	Color top = his ? rgba(42, 34, 140, 245) : rgba(60, 70, 92, 235), bot = his ? rgba(52, 40, 160, 245) : rgba(50, 60, 80, 235);
	if (his) tri(x + 4, y + 26, x - 15, y + 40, x + 4, y + 50, edge);   /* the point's edge */
	rrect_v(x - 2, y - 2, w + 4, h + 4, 14, edge, edge);                 /* the edge, 2 pixels */
	rrect_v(x, y, w, h, 12, top, bot);
	if (his) tri(x + 6, y + 29, x - 10, y + 40, x + 6, y + 47, top);     /* the point, over the edge */
}

static void chat(void) {
	if (!sc.side) return;
	static char lines[64][160];
	float x = 350, w = 268, y = 64;
	if (sc.you[0]) {
		int n = wrap(sc.you, 13, w - 58, lines, 3);
		float h = 26 + n * 17;
		bubble_frame(x + 30, y, w - 30, h, false);
		text(x + 44, y + 12, "You", 10, rgba(160, 175, 200, 255), 0, 0);
		for (int i = 0; i < n; i++) text(x + 44, y + 28 + i * 17, lines[i], 13, TEXT, 0, w - 58);
		y += h + 10;
	}
	if (sc.him[0] && ui_set.captions) {
		int n = wrap(sc.him, 13, w - 28, lines, 64);
		int room_lines = (int)((360 - y - 34) / 18);
		if (room_lines < 2) room_lines = 2;
		int first = sc.him_scroll;
		if (first > n - room_lines) first = n - room_lines;
		if (first < 0) first = 0;
		sc.him_scroll = first;
		int shown = n - first < room_lines ? n - first : room_lines;
		sc.him_more = n - first - shown;
		float h = 34 + shown * 18;
		bubble_frame(x, y, w, h, true);
		text(x + 14, y + 13, "Professor Pixel", 10, rgba(140, 220, 255, 255), 0, 0);
		for (int i = 0; i < shown; i++) text(x + 14, y + 30 + i * 18, lines[first + i], 13, TEXT, 0, w - 24);
		if (sc.him_more > 0) tri(x + w - 20, y + h - 12, x + w - 12, y + h - 12, x + w - 16, y + h - 6, GOLD);
		if (first > 0) tri(x + w - 20, y + 14, x + w - 12, y + 14, x + w - 16, y + 8, GOLD);
	} else sc.him_more = 0;
}

/* the caption bar along the bottom of the stage */
static void caption(void) {
	if (sc.cap_kind == UI_CAP_NONE || sc.screen != UI_STAGE || sc.overlay) return;
	float t = now_ms() / 1000.0f;
	float w = sc.cap_kind == UI_CAP_TYPE ? 540 : 380, x = 320 - w / 2, y = 400;
	rrect_v(x, y, w, 32, 14, rgba(20, 18, 70, 225), rgba(20, 18, 70, 225));
	outline(x, y, w, 32, 14, 1, rgba(175, 175, 255, 150));
	float tx = x + 18;
	if (sc.cap_kind == UI_CAP_THINK) {
		for (int i = 0; i < 3; i++) circle(tx + 4 + i * 11, y + 16, 3.5f, rgba(130, 220, 255, (u8)(110 + 120 * (0.5f + 0.5f * sinf(t * 6 - i)))));
		tx += 40;
	} else if (sc.cap_kind == UI_CAP_LISTEN) {
		/* a red "recording" light, and bars that follow how loud it is */
		circle(tx + 4, y + 16, 4.5f, rgba(255, 70, 80, (u8)(170 + 85 * (0.5f + 0.5f * sinf(t * 5)))));
		sc.listen_shown += (sc.listen - sc.listen_shown) * 0.35f;
		float lv = sc.listen_shown;
		for (int i = 0; i < 9; i++) {
			float shape = 0.55f + 0.45f * fabsf(sinf(t * 9 + i * 1.9f));
			float hh = 1.5f + 10.5f * lv * shape * (1 - fabsf(i - 4) / 6.0f);
			rrect_v(tx + 16 + i * 5, y + 16 - hh, 3, hh * 2, 1, rgba(130, 220, 255, 230), rgba(130, 220, 255, 230));
		}
		tx += 70;
	}
	if (sc.cap_kind == UI_CAP_TYPE) {
		char shown[420];
		if (sc.masked) {
			size_t n = strlen(sc.typing);
			snprintf(shown, sizeof(shown), "%.6s", sc.typing);
			for (size_t i = 6; i < n && i < 60; i++) strcat(shown, "\x95");
		} else snprintf(shown, sizeof(shown), "%s", sc.typing);
		const char *show = shown;
		float maxw = w - 40;
		while (*show && text_width(show, 13) > maxw) show++;   /* the end of a long line */
		if (!sc.typing[0]) text(tx, y + 16, sc.cap, 13, DIM, 0, maxw);
		text(tx, y + 16, show, 13, TEXT, 0, 0);
		if (fmodf(t, 1.0f) < 0.55f) {
			float cxp = tx + text_width(show, 13) + 1;
			rect(cxp, y + 7, 2, 18, GOLD);
		}
	} else {
		text(sc.cap_kind == UI_CAP_INFO ? 320 : tx, y + 16, sc.cap, 13, TEXT, sc.cap_kind == UI_CAP_INFO ? 1 : 0, w - (tx - x) - 14);
	}
}

/* ---------------------------------------------------------------- panels */

static void list_panel(void) {
	float w = 360, rows = sc.orow_count, h = 64 + rows * 34;
	float x = 320 - w / 2, y = 222 - h / 2;
	panel(x, y, w, h);
	text(x + 14, y + 18, sc.overlay_title, 13, TITLE, 0, w - 28);
	rule(x + 14, y + 30, w - 28);
	for (int i = 0; i < sc.orow_count; i++) {
		float ry = y + 42 + i * 34;
		frost(x + 14, ry, w - 28, 26);
		if (i == sc.osel) gold_ring(x + 14, ry, w - 28, 26);
		text(x + 26, ry + 13, sc.orows[i].label, 12, TEXT, 0, 170);
		if (sc.orows[i].value[0] && sc.orows[i].value[0] != 1) {
			bool on = i == sc.osel;
			if (on) {
				tri(x + 196, ry + 13, x + 202, ry + 8, x + 202, ry + 18, SOFT);
				tri(x + w - 26, ry + 13, x + w - 32, ry + 8, x + w - 32, ry + 18, SOFT);
			}
			text((x + 202 + x + w - 32) / 2, ry + 13, sc.orows[i].value, 12, on ? TEXT : SOFT, 1, 110);
		}
	}
}

/* choosing an AI: a name, a line under it, a tag at the right; a note below */
static void brain_panel(void) {
	float x = 250, y = 62, w = 372, h = 350;
	panel(x, y, w, h);
	text(x + 16, y + 18, sc.overlay_title, 13, TITLE, 0, w - 32);
	rule(x + 12, y + 32, w - 24);
	const float pitch = 53, ch = 46;
	for (int i = 0; i < sc.orow_count; i++) {
		float ry = y + 44 + i * pitch, rx = x + 16, rw = w - 32;
		frost(rx, ry, rw, ch);
		text(rx + 14, ry + 16, sc.orows[i].label, 14, TEXT, 0, 200);
		if (sc.orows[i].value[0] != 1) text(rx + 14, ry + 33, sc.orows[i].value, 10, SOFT, 0, rw - 28);
		if (sc.orows[i].tag[0]) {
			float tw = text_width(sc.orows[i].tag, 9) + 14;
			rrect_v(rx + rw - 10 - tw, ry + 10, tw, 16, 8, sc.orows[i].gold ? GOLD : rgba(60, 66, 80, 230),
			        sc.orows[i].gold ? rgba(216, 176, 48, 255) : rgba(60, 66, 80, 230));
			text(rx + rw - 10 - tw / 2, ry + 18, sc.orows[i].tag, 9, sc.orows[i].gold ? rgba(40, 30, 8, 255) : rgba(220, 225, 235, 255), 1, 0);
		}
		if (i == sc.osel) gold_ring(rx, ry, rw, ch);
	}
	if (sc.note_title[0]) {
		float ny = y + 44 + sc.orow_count * pitch + 4, nh = y + h - 12 - ny;
		desc_bar(x + 16, ny, w - 32, nh);
		text(x + 30, ny + 16, sc.note_title, 12, TEXT, 0, w - 60);
		static char lines[6][160];
		int n = wrap(sc.note, 11, w - 60, lines, 3);
		for (int i = 0; i < n; i++) text(x + 30, ny + 32 + i * 15, lines[i], 11, rgba(205, 212, 235, 255), 0, 0);
	}
}

/* signing in: the QR code, the address, the steps, how it's going */
static void signin_panel(void) {
	float x = 250, y = 66, w = 372, h = 338;
	panel(x, y, w, h);
	text(x + 16, y + 18, sc.overlay_title, 13, TITLE, 0, w - 32);
	rule(x + 12, y + 32, w - 24);
	float qx = x + 18, qy = y + 46, qs = 160;
	rect(qx, qy, qs, qs, rgba(255, 255, 255, 255));
	if (sc.qr_size) {
		float m = (qs - 16) / sc.qr_size;
		mode_color();
		for (int yy = 0; yy < sc.qr_size; yy++)
			for (int xx = 0; xx < sc.qr_size; xx++)
				if (sc.qr[yy * sc.qr_size + xx]) rect(qx + 8 + xx * m, qy + 8 + yy * m, m + 0.05f, m + 0.05f, rgba(10, 12, 16, 255));
	} else text(qx + qs / 2, qy + qs / 2, "Starting\x85", 12, rgba(90, 96, 110, 255), 1, 0);
	text(qx, qy + qs + 18, sc.addr[0] ? sc.addr : "\x85", 15, GOLD, 0, qs + 30);
	static const char *steps = "1. Scan this with your phone (or open the address on a computer).\n"
	                           "2. Follow the link to make an API key, signed in with your own account.\n"
	                           "3. Paste the key into the page and press Send.";
	static char lines[12][160];
	int n = wrap(steps, 11, w - qs - 54, lines, 12);
	for (int i = 0; i < n; i++) text(qx + qs + 16, qy + 8 + i * 16, lines[i], 11, rgba(225, 230, 242, 255), 0, 0);
	desc_bar(x + 16, y + h - 92, w - 32, 76);
	static char st[4][160];
	int sn = wrap(sc.sign_state, 11, w - 60, st, 3);
	for (int i = 0; i < sn; i++) text(x + 30, y + h - 78 + i * 15, st[i], 11, TEXT, 0, 0);
	text(x + 30, y + h - 26, "Saved on the SD card. You never type a password here.", 10, rgba(205, 212, 235, 255), 0, w - 60);
}

static void overlay_panel(void) {
	if (sc.overlay == UI_OVERLAY_NONE) return;
	if (sc.overlay == UI_OVERLAY_LIST) { rect(vl, vt, vr - vl, vb - vt, rgba(0, 0, 0, 120)); list_panel(); }
	else if (sc.overlay == UI_OVERLAY_BRAIN) brain_panel();
	else if (sc.overlay == UI_OVERLAY_SIGNIN) signin_panel();
}

/* the Settings page: three groups (the picture; sound and the conversation;
 * the AI, Launcher and leaving) */
static void settings_screen(void) {
	rect(vl, vt, vr - vl, vb - vt, rgba(0, 0, 0, 110));
	panel(22, 54, 596, 312);
	text(34, 72, "Settings", 12, TITLE, 0, 380);
	rule(32, 84, 576);
	for (int i = 0; i < sc.orow_count; i++) {
		float x = 34, y = 92 + i * 25 + (i >= 4 ? 6 : 0) + (i >= 7 ? 6 : 0), w = 566, h = 22, cy = y + h / 2;
		bool on = i == sc.osel;
		rect_v(x, y, w, h, rgba(76, 94, 114, on ? 150 : 80), rgba(40, 54, 70, on ? 160 : 90));
		rect(x + 1, y + 1, w - 2, 1, rgba(255, 255, 255, 30));
		text(x + 16, cy, sc.orows[i].label, 13, TEXT, 0, 260);
		const char *v = sc.orows[i].value;
		if (v[0] == 2) text(x + w - 16, cy, v + 1, 12, SOFT, 2, 260);
		else if (v[0] && v[0] != 1) {
			float vx = x + w - 110;
			tri(vx - 66, cy, vx - 59, cy - 6, vx - 59, cy + 6, on ? GOLD : DIM);
			tri(vx + 66, cy, vx + 59, cy - 6, vx + 59, cy + 6, on ? GOLD : DIM);
			text(vx, cy, v, 13, on ? TEXT : SOFT, 1, 110);
		}
		if (on) gold_ring(x, y, w, h);
	}
}

/* ================================================================ thread */

/* ================================================================ thread */

static void frame_setup(void) {
	view_bounds();
	Mtx44 proj;
	guOrtho(proj, vt, vb, vl, vr, 0, 1);
	GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
	Mtx m;
	guMtxIdentity(m);
	GX_LoadPosMtxImm(m, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTevStages(1);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetAlphaUpdate(GX_FALSE);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetCullMode(GX_CULL_NONE);
	GX_InvalidateTexAll();
	cur_mode = M_NONE;
}


static void draw_frame(void) {
	frame_setup();
	L();
	stage();
	chat();
	caption();
	overlay_panel();
	if (sc.screen == UI_SETTINGS) settings_screen();
	if (sc.screen == UI_SETTINGS || sc.overlay == UI_OVERLAY_LIST || sc.overlay == UI_OVERLAY_SIGNIN) {
		if (sc.status1[0] && sc.screen == UI_SETTINGS) status_bar();
	}
	hints_row();
	toast_draw();
	if (ui_set.fps) {   /* frames drawn in the last second */
		static u32 since, frames, shown;
		u32 now = now_ms();
		frames++;
		if (now - since >= 1000) { shown = frames; frames = 0; since = now; }
		char f[24];
		snprintf(f, sizeof(f), "%u fps", (unsigned)shown);
		text(24, 24, f, 12, GOLD, 0, 0);
	}
	U();
}

static void *video_main(void *arg) {
	(void)arg;
	while (running) {
		if (video_change) {
			video_change = false;
			GXRModeObj old = mode;
			pick_mode();
			if (memcmp(&old, &mode, sizeof(mode))) {
				GX_DrawDone();
				VIDEO_SetBlack(TRUE);
				VIDEO_Flush();
				VIDEO_WaitVSync();
				configure_vi();
				setup_gx();
			}
		}
		draw_frame();
		GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);   /* so the copy clears depth too */
		GX_CopyDisp(xfb[fb], GX_TRUE);
		GX_DrawDone();
		VIDEO_SetNextFramebuffer(xfb[fb]);
		VIDEO_Flush();
		VIDEO_WaitVSync();
		fb ^= 1;
	}
	return NULL;
}

void ui_init(void) {
	VIDEO_Init();
	PAD_Init();
	LWP_MutexInit(&lock, false);
	/* framebuffers big enough for any mode (PAL's 576 lines included) */
	size_t fb_bytes = 640 * 576 * 2;
	xfb[0] = MEM_K0_TO_K1(memalign(32, fb_bytes));
	xfb[1] = MEM_K0_TO_K1(memalign(32, fb_bytes));
	pick_mode();
	configure_vi();
	fifo = memalign(32, FIFO_SIZE);
	memset(fifo, 0, FIFO_SIZE);
	GX_Init(fifo, FIFO_SIZE);
	GX_SetCopyClear((GXColor){ 0, 0, 0, 255 }, 0x00FFFFFF);
	setup_gx();
	GX_SetCullMode(GX_CULL_NONE);
	GX_CopyDisp(xfb[0], GX_TRUE);

	tex_init(&tex_font_small, font_small.tex, font_small.w, font_small.h, GX_TF_I8);
	tex_init(&tex_font_large, font_large.tex, font_large.w, font_large.h, GX_TF_I8);

	sc.screen = UI_STAGE;
	sc.props = true;
	running = true;
	LWP_CreateThread(&thread, video_main, NULL, NULL, 64 * 1024, 100);
}

void ui_shutdown(void) {
	if (!running) return;
	running = false;
	LWP_JoinThread(thread, NULL);
	GX_DrawDone();
	VIDEO_SetBlack(TRUE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
}

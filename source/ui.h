/****************************************************************************
* ui.h - Professor Pixel's screens, drawn on a thread of their own: the
* stage (the professor floating in his beam of light, the conversation, the
* caption along the bottom), the panels over it (choosing an AI, signing
* in) and the Settings page. The main thread sets what's shown with these
* calls; drawing never waits for it.
****************************************************************************/
#ifndef UI_H
#define UI_H
#include <gccore.h>
#include <stdbool.h>

/* ---------------------------------------------------------------- settings */

enum { UI_RES_AUTO, UI_RES_480I, UI_RES_480P, UI_RES_COUNT };
#define UI_OVERSCAN_MAX 10
#define UI_VOLUME_MAX 10
#define UI_KEY_MAX 200

typedef struct {
	int resolution;   /* UI_RES_* */
	int widescreen;   /* 0 = 4:3, 1 = 16:9 */
	int full_width;   /* 1 = 704 (default), 0 = 640 */
	int overscan;     /* percent, 0..UI_OVERSCAN_MAX */
	int voice_volume; /* his voice, 0 (off)..UI_VOLUME_MAX */
	int sound_volume; /* menu sounds, 0 (off)..UI_VOLUME_MAX */
	int captions;     /* 1 = his words show in a bubble as he speaks */
	int brain;        /* AiBrain: which AI he uses */
	char launcher[64];/* the .dol at the root of the SD card that Exit starts ("" = find one) */
	char key[3][UI_KEY_MAX];  /* the API key for each AI ("" = none yet) */
	char model[3][80];        /* the model for each AI ("" = the usual one) */
	int depth_flip;   /* (a fix-up, set by hand in pixel.ini if he ever looks inside out) */
	int glow;         /* 1 = the glow round bright things (display.glow=0 in pixel.ini turns it off) */
	int fps;          /* 1 = frames a second shown in the corner (display.fps=1 in pixel.ini) */
} UiSettings;

extern UiSettings ui_set;

void ui_settings_path(const char *path);   /* where the settings are kept (once the SD card is ready) */
void ui_settings_load(void);
bool ui_settings_save(void);               /* false if the SD card wouldn't take them */
void ui_settings_changed(void);            /* call after changing ui_set (the video mode follows) */
const char *ui_resolution_name(int r);

/* --------------------------------------------------------------- lifetime */

void ui_init(void);           /* video, graphics and the drawing thread (call first) */
void ui_shutdown(void);       /* before rebooting or starting another program */

/* ------------------------------------------------------------------ scene */

enum { UI_STAGE, UI_SETTINGS };
void ui_screen(int screen);
int  ui_get_screen(void);

/* the stage: where he stands (0 in the middle, 1 to the left, making room for
 * the conversation), how he looks, and the fog round his head (0..1) */
void ui_layout(int side);
void ui_pose(float think, float brows, bool props);   /* think 0..1; brows: extra lift */
void ui_chuckle(void);                                /* a little laugh (bow tie bouncing) */
void ui_fog(float amount);

/* the caption bar along the bottom of the stage */
enum { UI_CAP_NONE, UI_CAP_INFO, UI_CAP_THINK, UI_CAP_TYPE, UI_CAP_LISTEN };
void ui_caption(int kind, const char *text);
void ui_listen_level(float level);   /* UI_CAP_LISTEN: how loud the microphone is, 0..1 */
/* the line being typed (shown in the caption bar with UI_CAP_TYPE); masked: shown as dots after a few letters */
void ui_typing(const char *text, bool masked);

/* the conversation: the question and his answer (either may be ""); his
 * answer scrolls a line at a time (lines < 0: back to the top) */
void ui_chat(const char *you, const char *him);
void ui_chat_scroll(int lines);
int  ui_chat_more(void);   /* lines of his answer below the bubble (0: all showing) */

/* the description bar (Settings, the panels) and the controls along the bottom */
void ui_status(const char *line1, const char *line2);
/* "key:label", where the key is S (the Control Stick), A, B, X, Y, Z, ST (START), KB (the keyboard), MIC (the microphone's button) or none ("label") */
#define UI_HINTS_MAX 5
void ui_hints(const char *const *hints, int n);
void ui_toast(const char *msg);   /* a message across the top that fades after a few seconds */

/* panels over the stage */
enum { UI_OVERLAY_NONE, UI_OVERLAY_LIST, UI_OVERLAY_BRAIN, UI_OVERLAY_SIGNIN };
#define UI_OVERLAY_ROWS 10
void ui_overlay(int overlay, const char *title);
/* a row: the label, and its value (a choice, with arrows), or for the
 * BRAIN panel a line under the name; value NULL: a plain choice. A value
 * starting with \x02 is information, not a choice (no arrows). */
void ui_overlay_row(int row, const char *label, const char *value);
void ui_overlay_tag(int row, const char *tag, bool gold);   /* the BRAIN panel's little label at the right */
void ui_overlay_note(const char *title, const char *text);  /* the BRAIN panel's box under the rows */
void ui_overlay_select(int row);
void ui_overlay_rows(int count);
/* the sign-in panel: the QR code (size x size modules, 1 = dark; size 0 =
 * none yet), the address, and how it's going */
void ui_signin(const unsigned char *qr, int size, const char *address, const char *state);

#endif

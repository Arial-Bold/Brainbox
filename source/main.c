/* main.c - BrainBox: Professor Scotty, a hologram professor on the GameCube who answers
 * your questions with the AI of your choice (Google Gemini, Claude or
 * ChatGPT, or open models on Groq) and speaks his answers aloud.
 *
 * The main thread runs the conversation, reads the controller and the
 * keyboard, and tells the drawing thread (ui.c) what to show. */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "ui.h"
#include "ai.h"
#include "speech.h"
#include "sound.h"
#include "gckbd.h"
#include "keyweb.h"
#include "listen.h"
#include "dolload.h"
#include "net/storage.h"
#include "net/qrcodegen.h"
#include "net/update.h"
#include "voicefetch.h"

const char appversion[] = "v1.2";

/* ---------------------------------------------------------------- input */

static u32 held, down, held_prev;
static bool listening_now;   /* the microphone is recording (the background goes quiet) */
static bool dir_sounds = true;   /* the move sounds (not while scrolling his answer) */
static int repeat;

/* once a frame: the controller (the stick works as the + Control Pad) and the keyboard */
static void read_input(void) {
	VIDEO_WaitVSync();
	PAD_ScanPads();
	held = PAD_ButtonsHeld(0);
	s8 sx = PAD_StickX(0), sy = PAD_StickY(0), cy = PAD_SubStickY(0);
	if (sx < -48) held |= PAD_BUTTON_LEFT;
	if (sx > 48) held |= PAD_BUTTON_RIGHT;
	if (sy > 48 || cy > 48) held |= PAD_BUTTON_UP;
	if (sy < -48 || cy < -48) held |= PAD_BUTTON_DOWN;
	down = held & ~held_prev;
	held_prev = held;
	const u32 dirs = PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT;
	if (held & dirs) {
		if (down & dirs) repeat = 0;
		else if (++repeat >= 18 && (repeat - 18) % 5 == 0) down |= held & dirs;
	}
	update_poll();
	/* the automatic check at start-up found a new version: say so, once */
	static bool told;
	if (!told && update_was_quiet() && update_state() == UPD_AVAILABLE) {
		told = true;
		char m[96];
		snprintf(m, sizeof(m), "BrainBox %s is ready to install: Settings > Updates", update_latest_version());
		ui_toast(m);
	}
	sound_ambience_tick(listening_now, speech_busy());
	if (down & PAD_BUTTON_B) sound_play(SFX_BACK);
	else if (down & (PAD_BUTTON_A | PAD_BUTTON_X | PAD_BUTTON_Y | PAD_BUTTON_START)) sound_play(SFX_SELECT);
	else if ((down & dirs) && dir_sounds) sound_play(SFX_HOVER);
	kb_update();
}

static void wait_release(void) {
	while (PAD_ButtonsHeld(0)) { VIDEO_WaitVSync(); PAD_ScanPads(); }
	held_prev = 0xFFFFFFFF;
}

static void hints(int n, ...) {
	const char *h[UI_HINTS_MAX];
	va_list ap;
	va_start(ap, n);
	for (int i = 0; i < n && i < UI_HINTS_MAX; i++) h[i] = va_arg(ap, const char *);
	va_end(ap);
	ui_hints(h, n);
}

/* ------------------------------------------------------------ Launcher */

#define PSOSDLOADID 0x7c6000a6

void dol_before_jump(void) {
	speech_stop();
	ui_shutdown();
	sound_shutdown();
}

static int cmp_name(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }
static int launcher_list(char (*names)[64], int max) {
	int n = 0;
	if (!storage_ok()) return 0;
	DIR *d = opendir("sd:/");
	if (!d) return 0;
	struct dirent *ent;
	while ((ent = readdir(d)) && n < max) {
		size_t l = strlen(ent->d_name);
		if (ent->d_name[0] == '.' || l < 5 || l >= 64 || strcasecmp(ent->d_name + l - 4, ".dol")) continue;
		char path[80];
		snprintf(path, sizeof(path), "sd:/%s", ent->d_name);
		if (!storage_is_file(path)) continue;
		snprintf(names[n++], 64, "%s", ent->d_name);
	}
	closedir(d);
	qsort(names, n, 64, cmp_name);
	return n;
}

static const char *launcher_current(void) {
	static char names[48][64];
	static const char *const usual[] = { "autoexec.dol", "boot.dol", "ipl.dol" };
	int n = launcher_list(names, 48);
	if (ui_set.launcher[0])
		for (int i = 0; i < n; i++) if (!strcasecmp(names[i], ui_set.launcher)) return names[i];
	for (unsigned u = 0; u < sizeof(usual) / sizeof(usual[0]); u++)
		for (int i = 0; i < n; i++) if (!strcasecmp(names[i], usual[u])) return names[i];
	for (int i = 0; i < n; i++) if (!strncasecmp(names[i], "swiss", 5)) return names[i];
	return "";
}

static void exit_to_launcher(void) {
	char err[200];
	const char *name = launcher_current();
	if (name[0]) {
		char path[80];
		snprintf(path, sizeof(path), "sd:/%s", name);
		dol_launch(path, err, sizeof(err));
		ui_toast(err);   /* only gets here if it couldn't be started */
		return;
	}
	int *psoid = (int *)0x80001800;
	if (psoid[0] == PSOSDLOADID) {
		dol_before_jump();
		((void (*)(void))0x80001800)();
	}
	dol_before_jump();
	SYS_ResetSystem(SYS_HOTRESET, 0, 0);
}

/* for the updater: starts the newly installed BrainBox */
bool brainbox_launch_dol(const char *path) {
	char err[160];
	dol_launch(path, err, sizeof(err));   /* (only comes back if it couldn't) */
	return false;
}

/* ------------------------------------------------------- what he says */

static void say(const char *you, const char *him) {
	ui_layout(1);
	ui_chat(you, him);
	speech_say(him);
}

/* the hologram in use: its voice (a file beside the app) and how the AI talks */
static const char *voice_name(void) {
	return ui_set.character == UI_CHAR_HER ? "voice_her.flitevox" : "voice_scotty.flitevox";
}

static void use_character(void) {
	char path[300];
	storage_path(path, sizeof(path), voice_name());
	speech_voice(path);
	ai_set_character(ui_set.character);
}

/* Waits for the hologram's voice. If its file isn't beside the app (an update
 * from inside an older version brings the program but not the voices), or it
 * won't load, it's fetched from GitHub and loaded. */
static void ensure_voice(void) {
	for (int k = 0; k < 900 && !speech_ready() && !speech_voice_missing(); k++) read_input();
	if (!speech_voice_missing()) return;
	char path[300], m[200];
	storage_path(path, sizeof(path), voice_name());
	voicefetch_start(voice_name(), path);
	while (voicefetch_state() == VF_WORKING) {
		int p = voicefetch_percent();
		if (p >= 0) snprintf(m, sizeof(m), "Fetching %s's voice from GitHub\x85 %d%%", ui_char_name(ui_set.character), p);
		else snprintf(m, sizeof(m), "Fetching %s's voice from GitHub\x85", ui_char_name(ui_set.character));
		ui_caption(UI_CAP_INFO, m);
		read_input();
	}
	if (voicefetch_state() == VF_DONE) {
		ui_caption(UI_CAP_INFO, "Warming up\x85");
		speech_voice(path);   /* (tried again) */
		for (int k = 0; k < 900 && !speech_ready() && !speech_voice_missing(); k++) read_input();
	}
	ui_caption(UI_CAP_NONE, "");
	if (speech_voice_missing()) {
		if (voicefetch_state() == VF_FAILED)
			snprintf(m, sizeof(m), "%s %s, and it couldn't be fetched: %s", voice_name(), speech_voice_problem(), voicefetch_error());
		else
			snprintf(m, sizeof(m), "%s %s", voice_name(), speech_voice_problem());
		ui_toast(m);
	}
}

static void use_brain(void) {
	int b = ui_set.brain;
	ai_set((AiBrain)b, ui_set.key[b], ui_set.model[b]);
	ai_set_search(ui_set.key[AI_GEMINI], ui_set.web_search);   /* (Google Search, through Gemini, when he needs to look something up) */
}

/* while he's waiting for his answer: says so when he's looking it up */
static void looking_up(void) {
	static bool shown;
	if (ai_searching() && !shown) { ui_caption(UI_CAP_THINK, "Let me look that up on the web\x85"); ui_pose(1, 0.3f, true); }
	shown = ai_searching();
}

static void save_settings(void) {
	if (!storage_ok()) { ui_toast("No SD card: these settings last until BrainBox closes."); return; }
	if (!ui_settings_save()) ui_toast("Couldn't save the settings to the SD card.");
}

/* ----------------------------------------------------- typing a line */

/* Lets the keyboard type a line (the caption bar shows it). Returns true
 * with the line in out on Enter, false on Esc or B. */
static bool type_line(const char *placeholder, char *out, int cap, bool masked) {
	int n = (int)strlen(out);
	ui_typing(out, masked);
	ui_caption(UI_CAP_TYPE, placeholder);
	hints(2, "Enter on the keyboard when done", "B:Cancel");
	for (;;) {
		read_input();
		if (down & PAD_BUTTON_B) { ui_typing("", false); return false; }
		if (!kb_connected()) { ui_toast("The keyboard was unplugged."); ui_typing("", false); return false; }
		KbEvent e;
		bool changed = false;
		while (kb_next(&e)) {
			if (e.key == KB_CHAR && n < cap - 1 && (unsigned char)e.ch >= 32) { out[n++] = e.ch; out[n] = 0; changed = true; }
			else if (e.key == KB_BACKSPACE && n > 0) { out[--n] = 0; changed = true; }
			else if (e.key == KB_ESC) { ui_typing("", false); return false; }
			else if (e.key == KB_ENTER && n > 0) { ui_typing("", false); return true; }
		}
		if (changed) ui_typing(out, masked);
	}
}

/* ------------------------------------------------------ signing in */

static void signin_state(const char *state) {
	static unsigned char q[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
	static unsigned char mods[177 * 177];
	int size = 0;
	if (keyweb_ready() && qrcodegen_encodeText(keyweb_url(), tmp, q, qrcodegen_Ecc_MEDIUM, 1, 40, qrcodegen_Mask_AUTO, true)) {
		size = qrcodegen_getSize(q);
		for (int y = 0; y < size; y++)
			for (int x = 0; x < size; x++) mods[y * size + x] = qrcodegen_getModule(q, x, y);
	}
	ui_signin(size ? mods : NULL, size, keyweb_ready() ? keyweb_url() : "", state);
}

/* Tries a key: he introduces himself with it. True if it worked. */
static bool try_key(int b, const char *key) {
	/* tried first; kept only if it works */
	char old_key[UI_KEY_MAX];
	int old_brain = ui_set.brain;
	snprintf(old_key, sizeof(old_key), "%s", ui_set.key[b]);
	snprintf(ui_set.key[b], UI_KEY_MAX, "%s", key);
	ui_set.brain = b;
	use_brain();
	ai_test();
	signin_state("Trying the key\x85");
	ui_pose(0.6f, 0, true);
	while (ai_state() == AI_BUSY) read_input();
	ui_pose(0, 0, true);
	if (ai_state() == AI_DONE) {
		snprintf(ui_set.model[b], sizeof(ui_set.model[b]), "%s", ai_model());   /* (the model that answered) */
		save_settings();
		return true;
	}
	char msg[400];
	snprintf(msg, sizeof(msg), "%.390s", ai_answer());
	snprintf(ui_set.key[b], UI_KEY_MAX, "%s", old_key);   /* back as it was */
	ui_set.brain = old_brain;
	use_brain();
	signin_state(msg);
	ui_toast("That key didn't work. Try another?");
	return false;
}

/* The sign-in panel for brain b. True once a working key is in. */
static bool sign_in(int b) {
	char title[64];
	snprintf(title, sizeof(title), "Sign in: %s", ai_name((AiBrain)b));
	ui_overlay(UI_OVERLAY_SIGNIN, title);
	keyweb_open((AiBrain)b);
	signin_state("Starting the network\x85");
	hints(2, "Y:Type the key instead", "B:Back");
	bool was_ready = false;
	for (;;) {
		read_input();
		if (keyweb_ready() != was_ready) { was_ready = keyweb_ready(); signin_state(keyweb_message()); }
		else if (!keyweb_ready()) {
			static char last[128];
			if (strcmp(last, keyweb_message())) { snprintf(last, sizeof(last), "%s", keyweb_message()); signin_state(last); }
		}
		char key[UI_KEY_MAX];
		bool got = keyweb_take(key, sizeof(key));
		if (!got && (down & PAD_BUTTON_Y)) {
			if (!kb_connected()) { ui_toast("Plug in the keyboard controller to type the key."); continue; }
			key[0] = 0;
			ui_overlay(UI_OVERLAY_NONE, "");
			got = type_line("Type or paste the API key, then press Enter", key, sizeof(key), true);
			ui_caption(UI_CAP_NONE, "");
			ui_overlay(UI_OVERLAY_SIGNIN, title);
			signin_state(keyweb_message());
			hints(2, "Y:Type the key instead", "B:Back");
		}
		if (got) {
			if (try_key(b, key)) { keyweb_close(); ui_overlay(UI_OVERLAY_NONE, ""); return true; }
			hints(2, "Y:Type the key instead", "B:Back");
		}
		if (down & PAD_BUTTON_B) { keyweb_close(); ui_overlay(UI_OVERLAY_NONE, ""); return false; }
	}
}

/* Choosing an AI, then signing in to it. True once he has a brain. */
static bool choose_brain(void) {
	/* the order they're listed in: the free ones first */
	static const AiBrain order[AI_COUNT] = { AI_GROQ, AI_GEMINI, AI_CLAUDE, AI_OPENAI };
	static const char *subs[AI_COUNT] = { "Google account and an API key", "Anthropic account and an API key",
	                                      "OpenAI account and an API key", "Open models (Llama), a Groq account and a key" };
	static const char *notes[AI_COUNT] = {
		"A free allowance with daily limits. Make a key on your phone or computer and send it to the GameCube.",
		"Pay as you go: a short chat costs a fraction of a cent. Claude can't listen: typing only, or add Groq too.",
		"Pay as you go: a short chat costs a fraction of a cent. Make a key on your phone or computer and send it here.",
		"Free, and very quick. Open models that hear you and answer. Make a key on your phone or computer and send it here." };
	int row = 0;
	for (int i = 0; i < AI_COUNT; i++) if (order[i] == ui_set.brain) row = i;
	for (;;) {
		static char t[80];
		snprintf(t, sizeof(t), "Which AI should %s use?", ui_char_name(ui_set.character));
		ui_overlay(UI_OVERLAY_BRAIN, t);
		for (int i = 0; i < AI_COUNT; i++) {
			AiBrain b = order[i];
			bool free_ = b == AI_GROQ || b == AI_GEMINI;
			ui_overlay_row(i, ai_name(b), subs[b]);
			ui_overlay_tag(i, ui_set.key[b][0] ? "Signed in" : free_ ? "Free tier" : "Pay as you go", free_ || ui_set.key[b][0]);
		}
		ui_overlay_rows(AI_COUNT);
		ui_overlay_select(row);
		ui_overlay_note(ai_name(order[row]), notes[order[row]]);
		/* X signs out of the one chosen (pressed twice, so it isn't by accident): then A signs in again, with a new key */
		bool signed_out = false;
		int armed = -1;
		u64 armed_at = 0;
		for (;;) {
			if (ui_set.key[order[row]][0]) hints(4, "S:Move", "A:Choose", "X:Sign out", "B:Back");
			else hints(3, "S:Move", "A:Choose", "B:Back");
			read_input();
			u64 now = ticks_to_millisecs(gettime());
			if (armed >= 0 && now - armed_at > 3000) armed = -1;
			if (down & (PAD_BUTTON_UP | PAD_BUTTON_DOWN)) {
				row = (row + ((down & PAD_BUTTON_UP) ? AI_COUNT - 1 : 1)) % AI_COUNT;
				ui_overlay_select(row);
				ui_overlay_note(ai_name(order[row]), notes[order[row]]);
				armed = -1;
			}
			if ((down & PAD_BUTTON_X) && ui_set.key[order[row]][0]) {
				AiBrain b = order[row];
				if (armed != row) {
					armed = row;
					armed_at = now;
					char m[96];
					snprintf(m, sizeof(m), "Press X again to sign out of %s", ai_name(b));
					ui_toast(m);
				} else {
					ui_set.key[b][0] = 0;   /* (the key is forgotten, on the SD card too) */
					save_settings();
					if (b == ui_set.brain) use_brain();
					char m[96];
					snprintf(m, sizeof(m), "Signed out of %s. A signs in again with a new key.", ai_name(b));
					ui_toast(m);
					signed_out = true;
					break;
				}
			}
			if (down & PAD_BUTTON_B) { ui_overlay(UI_OVERLAY_NONE, ""); return ui_set.key[ui_set.brain][0] != 0; }
			if (down & PAD_BUTTON_A) break;
		}
		if (signed_out) continue;   /* (the list again, with its tag changed) */
		int b = order[row];
		if (ui_set.key[b][0]) {   /* already signed in: just switch */
			ui_set.brain = b;
			use_brain();
			save_settings();
			ui_overlay(UI_OVERLAY_NONE, "");
			return true;
		}
		wait_release();
		if (sign_in(b)) return true;
	}
}

/* ----------------------------------------------------------- Settings */

enum { S_RES, S_SCREEN, S_WIDTH, S_OVERSCAN, S_HOLOGRAM, S_VOICE, S_SOUNDS, S_CAPTIONS, S_BRAIN, S_UPDATES, S_LAUNCHER, S_EXIT, S_ROWS };

/* the Updates row: how it's going */
static const char *updates_value(void) {
	static char v[64];
	switch (update_state()) {
	case UPD_CHECKING: snprintf(v, sizeof(v), "\x02" "Checking\x85"); break;
	case UPD_LATEST: snprintf(v, sizeof(v), "\x02" "Up to date (%s)", appversion); break;
	case UPD_AVAILABLE: snprintf(v, sizeof(v), "\x02" "Version %s is available", update_latest_version()); break;
	case UPD_CHECK_FAILED: snprintf(v, sizeof(v), "\x02" "Couldn't check"); break;
	case UPD_INSTALLING: {
		static const char *steps[] = { "Connecting", "Downloading", "Unpacking", "Installing" };
		float p = update_progress();
		if (p >= 0) snprintf(v, sizeof(v), "\x02%s %d%%", steps[update_step()], (int)(p * 100));
		else snprintf(v, sizeof(v), "\x02%s\x85", steps[update_step()]);
		break;
	}
	case UPD_INSTALLED: snprintf(v, sizeof(v), "\x02" "Installed: restart to use it"); break;
	case UPD_FAILED: snprintf(v, sizeof(v), "\x02" "Didn't install"); break;
	default: snprintf(v, sizeof(v), "\x02%s", appversion); break;
	}
	return v;
}

static void settings_rows(void) {
	char ov[16], vv[16], sv[16], lau[72], br[64];
	if (ui_set.overscan) snprintf(ov, sizeof(ov), "%d%%", ui_set.overscan); else snprintf(ov, sizeof(ov), "Off");
	if (ui_set.voice_volume) snprintf(vv, sizeof(vv), "%d", ui_set.voice_volume); else snprintf(vv, sizeof(vv), "Off");
	if (ui_set.sound_volume) snprintf(sv, sizeof(sv), "%d", ui_set.sound_volume); else snprintf(sv, sizeof(sv), "Off");
	const char *l = launcher_current();
	if (l[0]) snprintf(lau, sizeof(lau), "%s", l); else snprintf(lau, sizeof(lau), "\x02" "None on the SD card");
	snprintf(br, sizeof(br), "\x02%s%s", ai_name((AiBrain)ui_set.brain), ui_set.key[ui_set.brain][0] ? "" : " (not signed in)");
	ui_overlay_row(S_RES, "Resolution", ui_resolution_name(ui_set.resolution));
	ui_overlay_row(S_SCREEN, "Screen", ui_set.widescreen ? "16:9" : "4:3");
	ui_overlay_row(S_WIDTH, "Picture width", ui_set.full_width ? "Full" : "Standard");
	ui_overlay_row(S_OVERSCAN, "Overscan", ov);
	ui_overlay_row(S_VOICE, "Voice volume", vv);
	ui_overlay_row(S_SOUNDS, "Menu sounds", sv);
	ui_overlay_row(S_HOLOGRAM, "Hologram", ui_char_name(ui_set.character));
	ui_overlay_row(S_CAPTIONS, "Captions", ui_set.captions ? "On" : "Off");
	ui_overlay_row(S_BRAIN, "Brain", br);
	ui_overlay_row(S_UPDATES, "Updates", updates_value());
	ui_overlay_row(S_LAUNCHER, "Launcher", lau);
	ui_overlay_row(S_EXIT, "Exit to Launcher", NULL);
	ui_overlay_rows(S_ROWS);
}

static void settings_help(int row) {
	static const char *help[S_ROWS][2] = {
		{ "Resolution", "Auto picks 480p when a component cable is connected." },
		{ "Screen", "Choose 16:9 if your TV stretches the picture to fill the screen." },
		{ "Picture width", "Full fills the width of the TV picture. Standard leaves a border." },
		{ "Overscan", "Pulls everything in from the edges if your TV cuts them off." },
		{ "Hologram", "Who answers you: Professor Scotty, or " UI_HER_NAME ". Each has a voice of their own." },
		{ "Voice volume", "How loud the hologram speaks. Off for none." },
		{ "Menu sounds", "How loud the sounds are when you move and choose. Off for none." },
		{ "Captions", "Shows his answers in a bubble as he speaks." },
		{ "Brain", "Which AI he uses: Groq, Google Gemini, Claude or ChatGPT. A to change or sign in." },
		{ "Updates", "Opens Software Update: checks GitHub for a newer BrainBox, shows what's new and installs it." },
		{ "Launcher", "The program at the root of the SD card that Exit to Launcher starts." },
		{ "Exit to Launcher", "Leaves BrainBox." },
	};
	ui_status(help[row][0], help[row][1]);
	if (row == S_UPDATES && (update_state() == UPD_FAILED || update_state() == UPD_CHECK_FAILED) && update_error()[0])
		ui_status("Updates", update_error());
	if (row == S_EXIT) hints(3, "S:Select", "A:Exit", "B:Close");
	else if (row >= S_BRAIN && row != S_LAUNCHER) hints(3, "S:Select", "A:Open", "B:Close");
	else hints(3, "S:Select", "A:Change", "B:Close");
}

/* the Software Update window: what's new in the newest version on GitHub,
 * and buttons to install it, restart into it or close (as Flashcube and
 * CubeVerse have). An install carries on if it's closed; the Updates row
 * shows how it's going. */
static void update_buttons(const char **l, const char **r) {
	*l = *r = NULL;
	switch (update_state()) {
	case UPD_NONE:
	case UPD_CHECKING: *r = "Close"; break;
	case UPD_LATEST: *l = "Check Again"; *r = "Close"; break;
	case UPD_CHECK_FAILED: *l = "Try Again"; *r = "Close"; break;
	case UPD_AVAILABLE: *l = "Install"; *r = "Later"; break;
	case UPD_INSTALLING: *r = "Stop"; break;
	case UPD_INSTALLED: *l = "Restart Now"; *r = "Later"; break;
	case UPD_FAILED: *l = "Try Again"; *r = "Close"; break;
	}
}

static const char *update_button_help(const char *b) {
	if (!b) return "";
	if (!strcmp(b, "Install")) return "Downloads the new version from GitHub and puts it in place of this one. Your settings and keys are kept.";
	if (!strcmp(b, "Restart Now")) return "Starts the new version of BrainBox now.";
	if (!strcmp(b, "Stop")) return "Stops the download. Nothing is changed.";
	if (!strcmp(b, "Check Again") || !strcmp(b, "Try Again")) return "Asks GitHub again for the newest version.";
	if (!strcmp(b, "Later")) return "Closes this window. Settings > Updates brings it back.";
	return "Closes this window.";
}

static void update_window(void) {
	UpdateState s = update_state();
	if (s == UPD_NONE || s == UPD_LATEST || s == UPD_CHECK_FAILED) update_request_check(false);
	ui_update_open(true);
	int button = 0;
	dir_sounds = false;
	wait_release();
	for (;;) {
		s = update_state();
		const char *l, *r;
		update_buttons(&l, &r);
		if (!l) button = 1;
		/* what it says */
		char head[96], sub[160];
		const char *v = update_latest_version();
		sub[0] = 0;
		float progress = -2;
		switch (s) {
		case UPD_NONE:
		case UPD_CHECKING: snprintf(head, sizeof(head), "Checking for a new version\x85"); snprintf(sub, sizeof(sub), "Asking GitHub (github.com/%s).", UPDATE_REPO); break;
		case UPD_LATEST: snprintf(head, sizeof(head), "BrainBox is up to date"); snprintf(sub, sizeof(sub), "You have version %s, the newest one.", appversion); break;
		case UPD_CHECK_FAILED: snprintf(head, sizeof(head), "Couldn't check for updates"); snprintf(sub, sizeof(sub), "%s", update_error()); break;
		case UPD_AVAILABLE: snprintf(head, sizeof(head), "BrainBox %s is available", v); snprintf(sub, sizeof(sub), "You have version %s.", appversion); break;
		case UPD_INSTALLING: {
			UpdateStep st = update_step();
			snprintf(head, sizeof(head), "Installing BrainBox %s", v);
			snprintf(sub, sizeof(sub), "%s", st == UPD_STEP_NETWORK ? "Connecting to the network\x85" : st == UPD_STEP_DOWNLOAD ? "Downloading\x85"
			                                 : st == UPD_STEP_UNPACK ? "Unpacking\x85" : "Putting the new version in place\x85");
			progress = update_progress() >= 0 ? update_progress() : -1;
			break;
		}
		case UPD_INSTALLED: snprintf(head, sizeof(head), "BrainBox %s is installed", v); snprintf(sub, sizeof(sub), "Restart BrainBox to start using it."); break;
		case UPD_FAILED: snprintf(head, sizeof(head), "The update didn't finish"); snprintf(sub, sizeof(sub), "%s", update_error()); break;
		}
		bool notes = s == UPD_AVAILABLE || s == UPD_INSTALLING || s == UPD_INSTALLED || s == UPD_FAILED;
		ui_update_view(head, sub, s == UPD_AVAILABLE || s == UPD_INSTALLED, s == UPD_CHECKING || s == UPD_NONE || s == UPD_INSTALLING,
		               progress, notes ? update_notes() : "", l, r, button);
		const char *chosen = button == 0 ? l : r;
		ui_status(chosen ? chosen : "", update_button_help(chosen));
		if (notes && update_notes()[0]) hints(4, "S:Choose", "S:Scroll", "A:Confirm", "B:Close");
		else hints(3, "S:Choose", "A:Confirm", "B:Close");

		read_input();
		if ((down & PAD_BUTTON_LEFT) && l && button == 1) { button = 0; sound_play(SFX_HOVER); }
		if ((down & PAD_BUTTON_RIGHT) && button == 0) { button = 1; sound_play(SFX_HOVER); }
		if (down & PAD_BUTTON_UP) ui_update_scroll(-1);
		if (down & PAD_BUTTON_DOWN) ui_update_scroll(1);
		if (down & (PAD_BUTTON_B | PAD_BUTTON_START)) break;
		if (down & PAD_BUTTON_A) {
			if (!chosen) continue;
			if (!strcmp(chosen, "Install")) { update_request_install(); button = 1; }
			else if (!strcmp(chosen, "Restart Now")) {
				save_settings();
				ui_status("Restarting BrainBox\x85", "");
				char err[160];
				if (!update_restart(err, sizeof(err))) ui_toast(err);
			} else if (!strcmp(chosen, "Stop")) update_cancel();
			else if (!strcmp(chosen, "Check Again") || !strcmp(chosen, "Try Again")) {
				if (s == UPD_FAILED) update_request_install();
				else update_request_check(false);
				button = 0;
			} else break;
			wait_release();
		}
	}
	ui_update_open(false);
	dir_sounds = true;
	wait_release();
}

static void settings(void) {
	UiSettings before = ui_set;
	int row = 0, confirm_old = -1;
	u64 confirm_until = 0;
	ui_screen(UI_SETTINGS);
	ui_caption(UI_CAP_NONE, "");
	ui_overlay(UI_OVERLAY_NONE, "");
	settings_rows();
	ui_overlay_select(row);
	settings_help(row);
	wait_release();
	UpdateState upd_was = update_state();
	int upd_pct = -1;
	for (;;) {
		read_input();
		/* the Updates row follows how it's going */
		{
			int pct = update_progress() >= 0 ? (int)(update_progress() * 100) : -1;
			if (update_state() != upd_was || pct != upd_pct) {
				upd_was = update_state();
				upd_pct = pct;
				settings_rows();
				if (row == S_UPDATES) settings_help(row);
			}
		}
		if (confirm_old >= 0) {
			u64 now = ticks_to_millisecs(gettime());
			if (down & PAD_BUTTON_A) { confirm_old = -1; settings_help(row); wait_release(); continue; }
			if (now >= confirm_until) {
				ui_set.resolution = confirm_old;
				confirm_old = -1;
				ui_settings_changed();
				settings_rows();
				settings_help(row);
				continue;
			}
			char l2[64];
			snprintf(l2, sizeof(l2), "Going back to %s in %d\x85", ui_resolution_name(confirm_old), (int)((confirm_until - now + 999) / 1000));
			ui_status("Can you see this picture?", l2);
			hints(1, "A:Keep it");
			continue;
		}
		int d = 0;
		if (down & PAD_BUTTON_UP) { row = (row + S_ROWS - 1) % S_ROWS; ui_overlay_select(row); settings_help(row); }
		else if (down & PAD_BUTTON_DOWN) { row = (row + 1) % S_ROWS; ui_overlay_select(row); settings_help(row); }
		else if (down & (PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT)) d = (down & PAD_BUTTON_RIGHT) ? 1 : -1;
		if (down & (PAD_BUTTON_B | PAD_BUTTON_START)) break;
		if (down & PAD_BUTTON_A) {
			if (row == S_EXIT) {
				if (memcmp(&before, &ui_set, sizeof(ui_set))) save_settings();
				exit_to_launcher();
				continue;
			}
			if (row == S_UPDATES) {
				update_window();
				settings_rows();
				settings_help(row);
				wait_release();
				continue;
			}
			if (row == S_BRAIN) {
				ui_screen(UI_STAGE);
				wait_release();
				choose_brain();
				ui_screen(UI_SETTINGS);
				ui_overlay(UI_OVERLAY_NONE, "");
				settings_rows();
				ui_overlay_select(row);
				settings_help(row);
				wait_release();
				continue;
			}
			d = 1;
		}
		if (!d) continue;
		switch (row) {
		case S_RES:
			confirm_old = ui_set.resolution;
			ui_set.resolution = (ui_set.resolution + UI_RES_COUNT + d) % UI_RES_COUNT;
			ui_settings_changed();
			confirm_until = ticks_to_millisecs(gettime()) + 10000;
			break;
		case S_SCREEN: ui_set.widescreen ^= 1; ui_settings_changed(); break;
		case S_WIDTH: ui_set.full_width ^= 1; ui_settings_changed(); break;
		case S_OVERSCAN:
			ui_set.overscan += d;
			if (ui_set.overscan < 0) ui_set.overscan = 0;
			if (ui_set.overscan > UI_OVERSCAN_MAX) ui_set.overscan = (down & PAD_BUTTON_A) ? 0 : UI_OVERSCAN_MAX;
			ui_settings_changed();
			break;
		case S_VOICE:
			ui_set.voice_volume += d;
			if (ui_set.voice_volume < 0) ui_set.voice_volume = 0;
			if (ui_set.voice_volume > UI_VOLUME_MAX) ui_set.voice_volume = (down & PAD_BUTTON_A) ? 0 : UI_VOLUME_MAX;
			speech_say("Like this?");
			break;
		case S_SOUNDS:
			ui_set.sound_volume += d;
			if (ui_set.sound_volume < 0) ui_set.sound_volume = 0;
			if (ui_set.sound_volume > UI_VOLUME_MAX) ui_set.sound_volume = (down & PAD_BUTTON_A) ? 0 : UI_VOLUME_MAX;
			sound_play(SFX_CONFIRM);
			break;
		case S_CAPTIONS: ui_set.captions ^= 1; break;
		case S_HOLOGRAM:
			ui_set.character = (ui_set.character + UI_CHARS + d) % UI_CHARS;
			use_character();
			ai_forget();   /* (a new conversation, with someone new) */
			ui_chat("", "");
			break;
		case S_LAUNCHER: {
			static char names[48][64];
			int n = launcher_list(names, 48), cur = -1;
			if (n == 0) { ui_toast("There are no .dol files at the root of the SD card."); break; }
			const char *now = launcher_current();
			for (int i = 0; i < n; i++) if (!strcasecmp(names[i], now)) cur = i;
			cur = cur < 0 ? 0 : (cur + d + n) % n;
			snprintf(ui_set.launcher, sizeof(ui_set.launcher), "%s", names[cur]);
			break;
		}
		}
		settings_rows();
		if (row != S_RES) settings_help(row);
		if (down & PAD_BUTTON_A) wait_release();
	}
	if (memcmp(&before, &ui_set, sizeof(ui_set))) save_settings();
	ui_screen(UI_STAGE);
	if (ui_set.character != before.character) ensure_voice();   /* (someone new: their voice) */
	wait_release();
}

/* ------------------------------------------------------- the conversation */

static bool has_mic;   /* a microphone in a memory card slot */

static bool asked_once;   /* (once he's been asked something, the "how to ask" bar goes for this time) */

static const char *idle_caption(void) {
	if (has_mic) return kb_connected() ? "Hold the mic button and talk, or start typing\x85" : "Hold the mic button and talk to me\x85";
	return kb_connected() ? "Start typing your question\x85" : "Plug in a microphone or the keyboard controller to ask a question";
}

/* the bar telling you how to ask: until you first have (or when there's no way to) */
static void show_idle_caption(void) {
	if (asked_once && (has_mic || kb_connected())) ui_caption(UI_CAP_NONE, "");
	else ui_caption(UI_CAP_INFO, idle_caption());
}

static void idle_hints(void) {
	const char *ask_hint = has_mic ? "MIC:Hold to talk" : "A:Ask";
	bool more = ui_chat_more() > 0;
	if (has_mic && kb_connected()) {
		hints(4, ask_hint, "Y:Type", "ST:Settings", "B:Exit to Launcher");
	} else hints(3, ask_hint, "ST:Settings", "B:Exit to Launcher");
	(void)more;   /* (the arrows beside his answer show there's more: no hint needed) */
}

/* ---------------------------------------------------- a little secret
 * He's a Scotty, after all: ask him to beam you up and he'll oblige
 * (well, himself), and ask for more power and he'll tell you how much
 * she's got. Matched on the words alone (any case, any punctuation). */
static bool says(const char *q, const char *words) {
	char t[400];
	size_t n = 0;
	bool space = true;
	for (const char *p = q; *p && n < sizeof(t) - 2; p++) {
		char c = (char)tolower((unsigned char)*p);
		if (c == '\'') continue;
		if (isalpha((unsigned char)c)) { t[n++] = c; space = false; }
		else if (!space) { t[n++] = ' '; space = true; }
	}
	t[n] = 0;
	return strstr(t, words) != NULL;
}

static bool is_trek(const char *q) {
	return says(q, "beam me up") || says(q, "more power") || says(q, "all shes got") || says(q, "warp speed");
}
static bool is_egg(const char *q) {
	if (is_trek(q)) return true;
	return ui_set.character == UI_CHAR_HER && (says(q, "end program") || says(q, "geordi") || says(q, "la forge"));
}

/* waits about n frames (a 60th of a second each), the stage still moving */
static void wait_frames(int n) { for (int k = 0; k < n; k++) read_input(); }

static bool easter_egg(const char *q) {
	if (ui_set.character == UI_CHAR_HER) {
		/* (the Star Trek ones are Professor Scotty's) */
		if (is_trek(q)) {
			say(q, "Sorry, wrong department! You'll want Engineering. Professor Scotty runs that deck.");
			ui_chuckle();
			return true;
		}
		/* "Computer, end program": she flickers out like a hologram switching off... and comes straight back */
		if (says(q, "end program")) {
			ui_layout(1);
			ui_chat(q, "");
			ui_intro_start();
			sound_play_holo();
			while (!ui_intro_done()) read_input();
			say(q, "Nice try. I'm not that kind of hologram.");
			ui_chuckle();
			return true;
		}
		/* Geordi: the light catches her spectacles... */
		if (says(q, "geordi") || says(q, "la forge")) {
			ui_layout(1);
			ui_chat(q, "");
			ui_pose(1, 0, true);
			wait_frames(30);
			say(q, "Geordi? Hmm. If he had anything to do with making this hologram, he and I need to have a little talk.");
			while (speech_busy()) read_input();
			ui_pose(0, 0.6f, true);   /* (and it clears, with a smile) */
			ui_chuckle();
			return true;
		}
		return false;
	}
	if (says(q, "beam me up")) {
		ui_layout(1);
		say(q, "Aye! Hold still now, this tickles a wee bit. Energizing!");
		ui_chuckle();
		while (speech_busy()) read_input();
		ui_chat(q, "");
		sound_play_shimmer();
		ui_beam_away();
		while (!ui_beam_done()) read_input();
		say(q, "There we are, all in one piece! Well, mostly. Now, where were we?");
		ui_chuckle();
		return true;
	}
	if (is_trek(q)) {
		say(q, "I'm givin' her all she's got! Och, she cannae take much more!");
		ui_chuckle();
		return true;
	}
	return false;
}

/* what he says once the AI has answered q */
static void answered(const char *q) {
	if (ai_state() == AI_DONE) {
		int b = ui_set.brain;
		if (strcmp(ui_set.model[b], ai_model())) {   /* the usual model had gone: keep the one that answered */
			snprintf(ui_set.model[b], sizeof(ui_set.model[b]), "%s", ai_model());
			save_settings();
		}
		say(q, ai_answer());
		if (strstr(ai_answer(), "Ha") || strstr(ai_answer(), "ha!")) ui_chuckle();
	} else {
		char msg[400];
		snprintf(msg, sizeof(msg), "Hmm, I couldn't reach my brain just now. %.300s", ai_answer());
		say(q, msg);
	}
}

/* A question he was told to never mind can still be on its way; a new one
 * waits for it to finish (saying so, with B to give up). false: given up. */
static bool wait_for_brain(void) {
	if (ai_state() != AI_BUSY) return true;
	ui_caption(UI_CAP_THINK, "Just a moment, I'm still finishing a thought\x85");
	hints(1, "B:Never mind");
	wait_release();
	while (ai_state() == AI_BUSY) {
		read_input();
		if (down & PAD_BUTTON_B) { ui_caption(UI_CAP_NONE, ""); wait_release(); return false; }
	}
	return true;
}

/* asks the question and has him answer it */
static void ask(const char *q) {
	asked_once = true;
	if (easter_egg(q)) return;
	ui_layout(1);
	ui_chat(q, "");
	ui_caption(UI_CAP_THINK, "Hmm, let me think about that\x85");
	ui_pose(1, 0, true);
	hints(1, "B:Never mind");
	if (!wait_for_brain()) { ui_pose(0, 0, true); ui_caption(UI_CAP_NONE, ""); return; }
	ui_caption(UI_CAP_THINK, "Hmm, let me think about that\x85");
	use_brain();
	ai_ask(q);
	bool cancelled = false;
	while (ai_state() == AI_BUSY) {
		read_input();
		looking_up();
		if (down & PAD_BUTTON_B) { cancelled = true; break; }
	}
	ui_pose(0, 0.6f, true);
	ui_caption(UI_CAP_NONE, "");
	if (cancelled) {
		/* (the answer arrives later and is dropped) */
		say(q, "Oh, never mind then! Ask me anything else.");
		return;
	}
	answered(q);
}

/* the AI that writes down what's said: Groq if it's signed in (quick and
 * free), or the brain in use if it can hear, or another that can (Claude can't) */
static bool pick_ear(AiBrain *e) {
	int b = ui_set.brain;
	if (ui_set.key[AI_GROQ][0]) { *e = AI_GROQ; return true; }
	if (ai_can_hear((AiBrain)b) && ui_set.key[b][0]) { *e = (AiBrain)b; return true; }
	if (ui_set.key[AI_GEMINI][0]) { *e = AI_GEMINI; return true; }
	if (ui_set.key[AI_OPENAI][0]) { *e = AI_OPENAI; return true; }
	return false;
}

/* keeps the last recording on the SD card while the microphone is being
 * tried out: heard.wav (what's sent to be written down) and mic.txt */
static void save_recording(const short *pcm, int n) {
	if (!storage_ok()) return;
	char path[300];
	storage_path(path, sizeof(path), "mic.txt");
	char line[260];
	snprintf(line, sizeof(line), "%s\n", listen_info());
	storage_write_atomic(path, line, strlen(line));
	unsigned char *w;
	int len;
	if (n > 0 && listen_wav(pcm, n, &w, &len)) {
		storage_path(path, sizeof(path), "heard.wav");
		storage_write_atomic(path, w, (size_t)len);
		free(w);
	}
}

/* hold the microphone's button (or A) and talk: records while it's held, then has him answer */
static void listen_and_ask(void) {
	AiBrain ear;
	if (!pick_ear(&ear)) {
		wait_release();
		say("", "Oh dear, Claude can't hear through the microphone. But Groq, Google Gemini or ChatGPT can listen for me! "
		        "Sign one of them in under Brain in Settings, then switch back to Claude. Or just type your question.");
		return;
	}
	speech_stop();
	if (!wait_for_brain()) return;
	asked_once = true;
	listening_now = true;
	for (int i = 0; i < 10; i++) read_input();   /* (the background fades out before recording) */
	if (!listen_start()) { listening_now = false; has_mic = false; ui_toast("The microphone isn't answering. Is it pushed all the way in?"); return; }
	ui_layout(0);
	ui_chat("", "");
	ui_pose(0, 0.5f, true);
	ui_caption(UI_CAP_LISTEN, "I'm listening\x85");
	hints(1, "MIC:Let go when you're done");
	int warned = 0;
	for (;;) {
		read_input();
		listen_update();
		ui_listen_level(listen_level());
		if (!(held & PAD_BUTTON_A) && !listen_button()) break;
		if (listen_full()) break;
		if (!warned && listen_seconds() > LISTEN_MAX_SECONDS - 3) { warned = 1; ui_caption(UI_CAP_LISTEN, "A few more seconds\x85"); }
	}
	const short *pcm;
	int n;
	bool got = listen_stop(&pcm, &n);
	listening_now = false;
	ui_listen_level(0);
	if (!got) {
		ui_pose(0, 0, true);
		ui_caption(UI_CAP_NONE, "");
		save_recording(pcm, n);
		ui_toast(listen_seconds() < 0.3f ? "Hold the button down while you talk." : "I didn't hear anything. Try talking a little closer to the microphone.");
		wait_release();
		return;
	}
	ui_layout(1);
	ui_chat("\x85", "");
	ui_caption(UI_CAP_THINK, "Let me hear that again\x85");
	ui_pose(0.5f, 0.3f, true);
	hints(1, "B:Never mind");
	use_brain();
	ai_hear(pcm, n, LISTEN_RATE, ear, ui_set.key[ear], ui_set.model[ear]);
	save_recording(pcm, n);   /* (while it's on its way) */
	bool cancelled = false, shown = false, egg = false;
	while (ai_state() == AI_BUSY) {
		read_input();
		if (!shown && ai_heard()) {
			shown = true;
			if (is_egg(ai_heard())) {
				/* (it isn't sent on to the AI: see ai_hear_filter, so it's finished with in a moment) */
				egg = true;
				while (ai_state() == AI_BUSY) read_input();
				break;
			}
			ui_chat(ai_heard(), "");
			ui_caption(UI_CAP_THINK, "Hmm, let me think about that\x85");
			ui_pose(1, 0, true);
		}
		if (shown) looking_up();
		if (down & PAD_BUTTON_B) { cancelled = true; break; }
	}
	ui_pose(0, 0.6f, true);
	ui_caption(UI_CAP_NONE, "");
	const char *q = ai_heard() ? ai_heard() : "";
	static char heard[1200];
	snprintf(heard, sizeof(heard), "%s", q);
	if (egg) { ui_pose(0, 0.6f, true); ui_caption(UI_CAP_NONE, ""); easter_egg(heard); return; }   /* (his answer to it, still coming, is dropped) */
	if (cancelled) { say(heard, "Oh, never mind then! Ask me anything else."); return; }
	if (ai_heard_nothing()) { say("", "Hmm? I didn't quite catch that. Hold A, speak up a little, and let go when you're done!"); return; }
	answered(heard);
}

static void conversation(void) {
	bool exit_armed = false;
	u64 exit_at = 0;
	show_idle_caption();
	idle_hints();
	bool was_kb = kb_connected();
	for (;;) {
		dir_sounds = false;   /* (up and down scroll his answer: quietly) */
		read_input();
		dir_sounds = true;
		u64 now = ticks_to_millisecs(gettime());
		if (exit_armed && now - exit_at > 3000) exit_armed = false;
		if (kb_connected() != was_kb) { was_kb = kb_connected(); show_idle_caption(); idle_hints(); }
		/* a key typed on the keyboard starts a question */
		KbEvent e;
		char q[400] = "";
		bool start = false;
		while (kb_next(&e)) {
			if (e.key == KB_CHAR && (unsigned char)e.ch > 32) { size_t n = strlen(q); if (n < sizeof(q) - 1) { q[n] = e.ch; q[n + 1] = 0; } start = true; }
			if (e.key == KB_ENTER) start = true;
		}
		/* the microphone can be plugged in or out at any time */
		bool mic = listen_present();
		if (mic != has_mic) {
			has_mic = mic;
			show_idle_caption();
			idle_hints();
			if (mic) ui_toast("Microphone found: hold its button and talk!");
		}
		/* the button on the microphone */
		static bool mic_was;
		bool mic_btn = has_mic && listen_button();
		bool mic_pressed = mic_btn && !mic_was;
		mic_was = mic_btn;
		if (mic_pressed) {
			listen_and_ask();
			mic_was = listen_button();
			show_idle_caption();
			idle_hints();
			continue;
		}
		if (down & PAD_BUTTON_A) {
			if (has_mic) {
				listen_and_ask();
				show_idle_caption();
				idle_hints();
				continue;
			}
			if (!kb_connected()) { ui_toast("Plug in a microphone or the keyboard controller to ask a question."); continue; }
			start = true;
		}
		if ((down & PAD_BUTTON_Y) && has_mic) {
			if (!kb_connected()) { ui_toast("Plug in the keyboard controller to type."); continue; }
			start = true;
		}
		if (start) {
			speech_stop();
			if (type_line("Type your question, then press Enter", q, sizeof(q), false)) ask(q);
			show_idle_caption();
			idle_hints();
			continue;
		}
		if (down & PAD_BUTTON_UP) { ui_chat_scroll(-1); idle_hints(); }
		if (down & PAD_BUTTON_DOWN) { ui_chat_scroll(3); idle_hints(); }
		if (down & PAD_BUTTON_X) speech_stop();
		if (down & PAD_BUTTON_START) {
			speech_stop();
			settings();
			show_idle_caption();
			idle_hints();
			/* (the B that closed Settings isn't the first B of "exit to Launcher") */
			wait_release();
			exit_armed = false;
			continue;
		}
		if (down & PAD_BUTTON_B) {
			if (speech_busy()) { speech_stop(); continue; }
			if (exit_armed) exit_to_launcher();
			else { exit_armed = true; exit_at = now; ui_toast("Press B again to exit to Launcher"); }
		}
	}
}

/* -------------------------------------------------------------- start */

int main(int argc, char **argv) {
	ui_init();
	listen_init();
	sound_init();
	speech_init();
	storage_set_program(argc > 0 && argv ? argv[0] : NULL);
	if (storage_init()) {
		char path[300];
		storage_path(path, sizeof(path), "brainbox.ini");
		/* (from before it was BrainBox: the settings of the old name, here or in apps/pixel, carried over) */
		if (!storage_is_file(path)) {
			char old[300];
			storage_path(old, sizeof(old), "pixel.ini");
			if (!storage_is_file(old)) snprintf(old, sizeof(old), "%sapps/pixel/pixel.ini", storage_root());
			size_t len = 0;
			char *data = storage_is_file(old) ? storage_read(old, &len) : NULL;
			if (data) { storage_write_atomic(path, data, len); free(data); }
		}
		ui_settings_path(path);
		ui_settings_load();
	}
	if (storage_ok()) update_remove_old();   /* (the version an update replaced, now this one has started) */
	use_character();   /* (the hologram's voice, loaded in the background) */
	use_brain();
	ai_hear_filter(is_egg);   /* (his little secrets aren't sent on to the AI) */
	ai_warm_up();   /* the network and the clock, while he switches on */
	ui_layout(0);
	ui_pose(0, 0, true);
	hints(0);
	/* the opening: he switches on as a hologram, to its sound (which is made
	 * first thing, in the background: a moment) */
	/* (holding Z as it starts skips it) */
	PAD_ScanPads();
	bool skip = (PAD_ButtonsHeld(0) & PAD_TRIGGER_Z) != 0;
	if (ui_set.intro && !skip) {
		for (int k = 0; k < 240 && !sound_holo_ready(); k++) read_input();
		ui_intro_start();
		sound_play_holo();
		while (!ui_intro_done()) read_input();
	} else ui_intro_skip();
	update_request_check(true);   /* (quietly: it says so only if there's a new version) */
	/* his voice starts in the background too (a second or so: usually ready by now) */
	if (!speech_ready()) ui_caption(UI_CAP_INFO, "Warming up\x85");
	ensure_voice();
	ui_caption(UI_CAP_NONE, "");
	if (!storage_ok()) ui_toast("There's no SD card: settings won't be kept.");

	if (!ui_set.key[ui_set.brain][0]) {
		/* no brain yet: his head's a little foggy */
		ui_fog(1);
		ui_pose(0.6f, 0, true);
		say("", "Oh my... my head's a little foggy today. I seem to have misplaced my brain! Would you lend me one? "
		        "Just choose your favorite AI and sign me in.");
		hints(2, "A:Choose an AI", "B:Exit to Launcher");
		for (;;) {
			read_input();
			if (down & PAD_BUTTON_A) {
				wait_release();
				ui_chat("", "");
				ui_layout(0);
				if (choose_brain()) break;
				hints(2, "A:Choose an AI", "B:Exit to Launcher");
			}
			if (down & PAD_BUTTON_B) exit_to_launcher();
		}
		ui_fog(0);
		ui_pose(0, 0.8f, true);
		ui_chuckle();
		say("", ai_state() == AI_DONE ? ai_answer() : "Ah! That's much better. Ask me anything!");
	} else {
		char hello[160];
		snprintf(hello, sizeof(hello), ui_set.character == UI_CHAR_HER ? "Hi there! I'm %s. What would you like to know today?"
		                                                                 : "Hello there! I'm %s. What would you like to know today?", ui_char_name(ui_set.character));
		say("", hello);
	}
	has_mic = listen_present();
	conversation();
	return 0;
}

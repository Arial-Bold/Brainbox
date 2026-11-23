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
#include "weather.h"
#include "memory.h"
#include "voicefetch.h"

const char appversion[] = "v1.5";

/* ---------------------------------------------------------------- input */

static u32 held, down, held_prev;
static bool listening_now;   /* the microphone is recording (the background goes quiet) */
static bool dir_sounds = true;   /* the move sounds (not while scrolling his answer) */
static int repeat;

/* once a frame: the controller (the stick works as the + Control Pad) and the keyboard */
static bool boot_greeted;   /* (1.5) the first words at start-up have been started */
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
	/* (1.5) Once the first words at start-up have been said, the work that
	 * was held back so they'd come out clean: the hologram's background sound,
	 * the quiet check for updates, and finding the home town. Starting those
	 * while she was saying hello broke her voice up with static. */
	static bool settled;
	if (!settled && boot_greeted && !speech_busy()) {
		settled = true;
		sound_ambience_allow();
		update_request_check(true);   /* (quietly: it says so only if there's a new version) */
		ai_warm_continue();
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
	weather_set_home(ui_set.home_town);   /* (for the weather and the local time) */
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

enum { S_RES, S_SCREEN, S_WIDTH, S_OVERSCAN, S_HOLOGRAM, S_VOICE, S_SOUNDS, S_HOLOSOUND, S_CAPTIONS, S_BRAIN, S_HOMETOWN, S_UPDATES, S_LAUNCHER, S_EXIT, S_ROWS };

/* the Home town row: the town as typed, or Auto (and the town the network suggests, once it's known) */
static const char *hometown_value(void) {
	static char v[96];
	if (ui_set.home_town[0]) { snprintf(v, sizeof(v), "\x02%s", ui_set.home_town); return v; }
	char known[120];
	weather_home_known(known, sizeof(known));
	char *comma = strchr(known, ',');
	if (comma) *comma = 0;   /* (just the town: the row is short) */
	if (known[0]) snprintf(v, sizeof(v), "\x02" "Auto (%s)", known);
	else snprintf(v, sizeof(v), "\x02" "Auto");
	return v;
}

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
	ui_overlay_row(S_HOLOSOUND, "Hologram sound", ui_set.holo_sound ? "On" : "Off");
	ui_overlay_row(S_HOLOGRAM, "Hologram", ui_char_name(ui_set.character));
	ui_overlay_row(S_CAPTIONS, "Captions", ui_set.captions ? "On" : "Off");
	ui_overlay_row(S_BRAIN, "Brain", br);
	ui_overlay_row(S_HOMETOWN, "Home town", hometown_value());
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
		{ "Hologram sound", "The hologram's quiet hum and far-off bells in the background. Its loudness follows Menu sounds." },
		{ "Captions", "Shows his answers in a bubble as he speaks." },
		{ "Brain", "Which AI he uses: Groq, Google Gemini, Claude or ChatGPT. A to change or sign in." },
		{ "Home town", "For the weather and the time. Auto guesses from the network; type a town to be exact." },
		{ "Updates", "Opens Software Update: checks GitHub for a newer BrainBox, shows what's new and installs it." },
		{ "Launcher", "The program at the root of the SD card that Exit to Launcher starts." },
		{ "Exit to Launcher", "Leaves BrainBox." },
	};
	ui_status(help[row][0], help[row][1]);
	if (row == S_UPDATES && (update_state() == UPD_FAILED || update_state() == UPD_CHECK_FAILED) && update_error()[0])
		ui_status("Updates", update_error());
	if (row == S_EXIT) hints(3, "S:Select", "A:Exit", "B:Close");
	else if (row == S_HOMETOWN) {
		if (ui_set.home_town[0]) hints(4, "S:Select", "A:Type a town", "X:Auto", "B:Close");
		else hints(3, "S:Select", "A:Type a town", "B:Close");
	}
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
		if ((down & PAD_BUTTON_X) && row == S_HOMETOWN && ui_set.home_town[0]) {   /* back to Auto */
			ui_set.home_town[0] = 0;
			weather_set_home("");
			settings_rows();
			settings_help(row);
			continue;
		}
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
			if (row == S_HOMETOWN) {
				if (!kb_connected()) { ui_toast("Plug in the keyboard controller to type your town."); wait_release(); continue; }
				char town[64];
				snprintf(town, sizeof(town), "%s", ui_set.home_town);
				ui_screen(UI_STAGE);
				bool got = type_line("Your town, like Detroit or Paris, France, then press Enter", town, sizeof(town), false);
				ui_caption(UI_CAP_NONE, "");
				ui_screen(UI_SETTINGS);
				if (got) {
					snprintf(ui_set.home_town, sizeof(ui_set.home_town), "%s", town);
					weather_set_home(ui_set.home_town);
				}
				settings_rows();
				ui_overlay_select(row);
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
		case S_HOLOSOUND: ui_set.holo_sound ^= 1; break;
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
static void quiz(const char *request);
static bool wants_quiz(const char *q);
static void ask(const char *q) {
	asked_once = true;
	if (easter_egg(q)) return;
	if (wants_quiz(q)) { ui_chat(q, ""); quiz(q); return; }
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
			if (is_egg(ai_heard()) || wants_quiz(ai_heard())) {
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
	if (egg) {   /* (his answer to it, still coming, is dropped) */
		ui_pose(0, 0.6f, true);
		ui_caption(UI_CAP_NONE, "");
		if (!is_egg(heard)) quiz(heard);   /* (the quiz: not sent on to the AI either) */
		else easter_egg(heard);
		return;
	}
	if (cancelled) { say(heard, "Oh, never mind then! Ask me anything else."); return; }
	if (ai_heard_nothing()) { say("", "Hmm? I didn't quite catch that. Hold A, speak up a little, and let go when you're done!"); return; }
	answered(heard);
}

/* ------------------------------------------------------------ the quiz (1.5) */

/* "Quiz me", "let's play a quiz", "trivia about space"... */
static bool quiz_listening;   /* what's heard is only written down, not sent on to the AI (see hear_filter) */
static bool wants_quiz(const char *q) { return says(q, "quiz") || says(q, "trivia"); }
static bool hear_filter(const char *heard) {
	if (quiz_listening) return true;
	return is_egg(heard) || wants_quiz(heard);
}

#define QUIZ_PLAYERS 4
#define QUIZ_QMAX 160
#define QUIZ_AMAX 80
typedef struct { char q[QUIZ_QMAX], a[QUIZ_AMAX]; } QuizItem;
static QuizItem quiz_items[16];
static int quiz_nitems, quiz_next;

/* the host's lines: Professor Scotty's, or hers */
static const char *qline(const char *scotty, const char *her) { return ui_set.character == UI_CHAR_HER ? her : scotty; }

/* says it (with the bubble), waits while it's said if wait */
static void host(const char *line, bool wait) {
	ui_layout(1);
	ui_chat("", line);
	speech_say(line);
	if (wait) while (speech_busy()) read_input();
}

/* Waits for the AI, with a caption; B gives up (false, and quiz_stop is set) */
static bool quiz_stop;
static bool quiz_wait(const char *caption) {
	ui_caption(UI_CAP_THINK, caption);
	ui_pose(1, 0, true);   /* (fully thinking, as for a question: her spectacles catch the light) */
	hints(1, "B:Stop the quiz");
	bool ok = true;
	while (ai_state() == AI_BUSY) {
		read_input();
		if (down & PAD_BUTTON_B) { ok = false; quiz_stop = true; break; }
	}
	ui_pose(0, 0.3f, true);
	ui_caption(UI_CAP_NONE, "");
	return ok;
}

/* One reply from the player: said into the microphone (held A, or its own
 * button) or typed on the keyboard. 1: got it (in out); 2: X, hear it again;
 * -1: B, stop the quiz. */
static int quiz_reply(const char *prompt, char *out, int cap) {
	AiBrain ear;
	bool can_hear = has_mic && pick_ear(&ear);
	for (;;) {
		out[0] = 0;
		bool kb = kb_connected();
		if (!can_hear && !kb) {
			ui_caption(UI_CAP_INFO, "Plug in a microphone or the keyboard controller to answer");
			hints(1, "B:Stop the quiz");
		} else {
			ui_caption(UI_CAP_NONE, "");   /* (1.5: no caption: the bubble already asks it, and the hints say how to answer) */
			if (can_hear && kb) hints(4, "A:Hold and say it", "Y:Type it", "X:Hear it again", "B:Stop the quiz");
			else if (can_hear) hints(3, "A:Hold and say it", "X:Hear it again", "B:Stop the quiz");
			else hints(3, "KB:Type it, then Enter", "X:Hear it again", "B:Stop the quiz");
		}
		static bool mic_was;
		for (;;) {
			read_input();
			if (down & PAD_BUTTON_B) { speech_stop(); ui_caption(UI_CAP_NONE, ""); wait_release(); return -1; }
			if (down & PAD_BUTTON_X) { ui_caption(UI_CAP_NONE, ""); return 2; }
			/* typing: a key, or Y (or A without a microphone) */
			KbEvent e;
			bool type = false;
			while (kb_connected() && kb_next(&e)) {
				if (e.key == KB_CHAR && (unsigned char)e.ch > 32) { size_t n = strlen(out); if ((int)n < cap - 1) { out[n] = e.ch; out[n + 1] = 0; } type = true; }
				if (e.key == KB_ENTER) type = true;
			}
			if (kb_connected() && ((down & PAD_BUTTON_Y) || ((down & PAD_BUTTON_A) && !can_hear))) type = true;
			if (type) {
				speech_stop();
				bool got = type_line(prompt, out, cap, false);
				ui_caption(UI_CAP_NONE, "");
				if (got && out[0]) return 1;
				break;   /* (cancelled: ask again) */
			}
			/* talking: A held, or the microphone's button */
			bool mic_btn = can_hear && listen_button();
			bool mic_pressed = mic_btn && !mic_was;
			mic_was = mic_btn;
			if (can_hear && ((down & PAD_BUTTON_A) || mic_pressed)) {
				speech_stop();
				listening_now = true;
				for (int i = 0; i < 10; i++) read_input();
				if (!listen_start()) { listening_now = false; ui_toast("The microphone isn't answering. Is it pushed all the way in?"); break; }
				ui_caption(UI_CAP_LISTEN, "I'm listening\x85");
				hints(1, "MIC:Let go when you're done");
				for (;;) {
					read_input();
					listen_update();
					ui_listen_level(listen_level());
					if (!(held & PAD_BUTTON_A) && !listen_button()) break;
					if (listen_full()) break;
				}
				const short *pcm;
				int n;
				bool got = listen_stop(&pcm, &n);
				listening_now = false;
				ui_listen_level(0);
				mic_was = listen_button();
				if (!got) { ui_toast("I didn't hear anything. Hold the button while you talk."); break; }
				ui_caption(UI_CAP_THINK, "Let me hear that again\x85");
				quiz_listening = true;
				use_brain();
				ai_hear(pcm, n, LISTEN_RATE, ear, ui_set.key[ear], ui_set.model[ear]);
				bool stop = false;
				while (ai_state() == AI_BUSY) { read_input(); if (down & PAD_BUTTON_B) { stop = true; break; } }
				/* (stopped while it was being written down: let that finish first, so it isn't taken for a question) */
				for (int k = 0; stop && k < 300 && ai_state() == AI_BUSY; k++) read_input();
				quiz_listening = false;
				ui_caption(UI_CAP_NONE, "");
				if (stop) { wait_release(); return -1; }
				if (ai_heard() && ai_heard()[0]) { snprintf(out, cap, "%s", ai_heard()); return 1; }
				ui_toast("I didn't quite catch that. Try again?");
				break;
			}
		}
	}
}

/* a number of players or questions, from what was said ("three", "3", "just me") */
static int quiz_number(const char *t) {
	static const char *const words[] = { "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
	                                     "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty" };
	for (const char *p = t; *p; p++) if (isdigit((unsigned char)*p)) return atoi(p);
	for (int i = 20; i >= 1; i--) {   /* (longest first: "fourteen" before "four") */
		char w[24];
		snprintf(w, sizeof(w), " %s ", words[i]);
		char padded[300];
		snprintf(padded, sizeof(padded), " %s ", t);
		for (char *c = padded; *c; c++) *c = isalpha((unsigned char)*c) ? (char)tolower((unsigned char)*c) : ' ';
		if (strstr(padded, w)) return i;
	}
	if (says(t, "just me") || says(t, "only me") || says(t, "myself") || says(t, "solo") || says(t, "alone") || says(t, "single")) return 1;
	if (says(t, "couple") || says(t, "pair") || says(t, "both of us") || says(t, "the two of us")) return 2;
	return 0;
}

/* a name from what was said ("My name's Alex", "It's Sam Jones.", "alex") */
static void quiz_name(const char *t, char *out, int cap, int player) {
	char s[120];
	snprintf(s, sizeof(s), "%s", t);
	for (char *c = s; *c; c++) if (*c == ',' || *c == '.' || *c == '!') *c = ' ';
	for (char *c = s; *c; ) { if (c[0] == ' ' && (c == s || c[1] == ' ')) memmove(c, c + 1, strlen(c)); else c++; }   /* (single spaces) */
	static const char *const lead[] = { "my name is ", "my names ", "my name's ", "i am ", "im ", "i'm ", "it is ", "its ", "it's ", "call me ", "this is ", "hi ", "hello ", "hey ", "um ", "uh ", "er ", "oh ", "well " };
	for (bool again = true; again;) {
		again = false;
		for (unsigned k = 0; k < sizeof(lead) / sizeof(lead[0]); k++)
			if (!strncasecmp(s, lead[k], strlen(lead[k]))) { memmove(s, s + strlen(lead[k]), strlen(s + strlen(lead[k])) + 1); again = true; }
	}
	/* the first word, letters only (and a hyphen or apostrophe in it) */
	int n = 0;
	for (const char *p = s; *p && n < cap - 1 && n < 14; p++) {
		unsigned char c = (unsigned char)*p;
		if (isalpha(c) || c >= 0xC0 || ((c == '-' || c == '\'') && n)) out[n++] = (char)c;
		else if (n) break;
	}
	out[n] = 0;
	if (!n) snprintf(out, cap, "Player %d", player + 1);
	else {
		/* (all one case: made Capitalised; mixed, like McKenzie: kept as it is) */
		bool lo = false, up = false;
		for (int i = 0; i < n; i++) { if (islower((unsigned char)out[i])) lo = true; if (isupper((unsigned char)out[i])) up = true; }
		if (!(lo && up)) for (int i = 1; i < n; i++) out[i] = (char)tolower((unsigned char)out[i]);
		out[0] = (char)toupper((unsigned char)out[0]);
	}
}

/* the topic from what was said ("let's do space", "anything") */
static void quiz_topic(const char *t, char *out, int cap) {
	char s[160];
	snprintf(s, sizeof(s), "%s", t);
	static const char *const lead[] = { "lets do ", "let's do ", "let's have ", "lets have ", "how about ", "what about ", "maybe ", "um ", "uh ", "er ",
	                                    "i'd like ", "id like ", "i want ", "the topic is ", "topic is ", "the ", "a quiz on ", "a quiz about ", "quiz on ", "quiz about " };
	for (bool again = true; again;) {
		again = false;
		for (unsigned k = 0; k < sizeof(lead) / sizeof(lead[0]); k++)
			if (!strncasecmp(s, lead[k], strlen(lead[k]))) { memmove(s, s + strlen(lead[k]), strlen(s + strlen(lead[k])) + 1); again = true; }
	}
	size_t n = strlen(s);
	while (n && strchr(" .!?,", s[n - 1])) s[--n] = 0;
	if (!n || says(s, "anything") || says(s, "surprise") || says(s, "you choose") || says(s, "you pick") || says(s, "random") || says(s, "dont mind") || says(s, "whatever"))
		snprintf(out, cap, "general knowledge");
	else snprintf(out, cap, "%s", s);
}

/* where needle is in hay, ignoring case (NULL if it isn't) */
static const char *find_nocase(const char *hay, const char *needle) {
	size_t n = strlen(needle);
	for (; *hay; hay++) if (!strncasecmp(hay, needle, n)) return hay;
	return NULL;
}

/* the topic named with the request itself ("quiz me on dinosaurs", "trivia about space"), or "" */
static void quiz_topic_in(const char *q, char *out, int cap) {
	out[0] = 0;
	const char *seps[] = { " on ", " about ", " of " };
	for (unsigned k = 0; k < 3; k++) {
		const char *p = find_nocase(q, seps[k]);
		if (p) { quiz_topic(p + strlen(seps[k]), out, cap); return; }
	}
}

/* More questions from the AI: true if some came. avoid: answers already used. */
static bool quiz_fetch(const char *topic, int want, const char *avoid) {
	static char sys[1400], req[1400];
	snprintf(sys, sizeof(sys),
	         "You write questions for a quiz game played out loud on a Nintendo GameCube: a speech synthesizer reads each "
	         "question, and the players say their answers. So: short questions (under 25 words) in plain spoken English, "
	         "each with one short, clear answer (a name, a number, a word or two) that is easy to say and to judge; no "
	         "multiple choice, no trick questions, nothing with several right answers. Mix easy and medium questions. Every "
	         "fact must be correct and well established. Reply with exactly %d lines and nothing else, each in the form: "
	         "Q: the question || A: the answer", want);
	snprintf(req, sizeof(req), "Topic: %s. Write %d questions.%s%s", topic, want, avoid[0] ? " Don't use any of these answers again: " : "", avoid);
	ai_raw(sys, req);
	if (!quiz_wait(qline("Hmm, let me think up some good questions\x85", "Ooh, let me find some good questions\x85"))) return false;
	if (ai_state() != AI_DONE) return false;
	quiz_nitems = quiz_next = 0;
	const char *p = ai_answer();
	while (*p && quiz_nitems < 16) {
		const char *e = strchr(p, '\n');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		char line[400];
		snprintf(line, sizeof(line), "%.*s", (int)(len < sizeof(line) - 1 ? len : sizeof(line) - 1), p);
		char *q = strstr(line, "Q:"), *a = strstr(line, "A:");
		char *bar = strstr(line, "||");
		if (q && a && a > q) {
			if (bar && bar < a) *bar = 0; else a[-1] = 0;
			char *qs = q + 2, *as = a + 2;
			while (*qs == ' ' || *qs == '*') qs++;
			while (*as == ' ' || *as == '*') as++;
			QuizItem *it = &quiz_items[quiz_nitems];
			snprintf(it->q, sizeof(it->q), "%s", qs);
			snprintf(it->a, sizeof(it->a), "%s", as);
			for (char *t = it->q + strlen(it->q); t > it->q && (t[-1] == ' ' || t[-1] == '|' || t[-1] == '*'); ) *--t = 0;
			for (char *t = it->a + strlen(it->a); t > it->a && (t[-1] == ' ' || t[-1] == '.' || t[-1] == '*'); ) *--t = 0;
			if (it->q[0] && it->a[0]) quiz_nitems++;
		}
		if (!e) break;
		p = e + 1;
	}
	return quiz_nitems > 0;
}

/* plain lower-case letters and digits, single spaces (for comparing answers) */
static void quiz_plain(const char *in, char *out, size_t cap) {
	size_t n = 0;
	bool sp = true;
	for (const char *p = in; *p && n < cap - 1; p++) {
		unsigned char c = (unsigned char)*p;
		if (isalnum(c)) { out[n++] = (char)tolower(c); sp = false; }
		else if (!sp && n) { out[n++] = ' '; sp = true; }
	}
	while (n && out[n - 1] == ' ') n--;
	out[n] = 0;
	if (!strncmp(out, "the ", 4)) memmove(out, out + 4, strlen(out + 4) + 1);
}

/* is it a pass? ("pass", "I don't know", "skip") */
static bool quiz_pass(const char *t) {
	return says(t, "pass") || says(t, "dont know") || says(t, "no idea") || says(t, "skip") || says(t, "not sure") || says(t, "give up");
}

/* Judges the answer (the AI, kindly; if it can't, a plain comparison): true
 * if right, with what the host says in reaction */
static bool quiz_judge(const QuizItem *it, const char *who, const char *said, char *react, int cap) {
	if (quiz_pass(said)) {
		snprintf(react, cap, qline("No bother, %s! The answer was %s.", "No worries, %s! The answer was %s."), who, it->a);
		return false;
	}
	static char sys[2400], req[700];
	snprintf(sys, sizeof(sys), "%s You are hosting a quiz game. Judge the player's answer fairly and kindly: accept small "
	         "spelling or speech-recognition slips, other names for the same thing, and answers that are clearly right but "
	         "worded differently; a surname alone is fine for a person, and a number written as words is fine. Start your "
	         "reply with the single word RIGHT or WRONG, then one short sentence reacting in character, by the player's name. "
	         "If they're wrong, say the correct answer. Don't ask a question back.", ai_persona());
	snprintf(req, sizeof(req), "Question: %s\nCorrect answer: %s\n%s answered: \"%s\"", it->q, it->a, who, said);
	ai_raw(sys, req);
	if (quiz_wait(qline("Hmm, let me see\x85", "Ooh, let's see\x85")) && ai_state() == AI_DONE) {
		const char *r = ai_answer();
		while (*r == ' ' || *r == '*' || *r == '\n') r++;
		bool right = !strncasecmp(r, "RIGHT", 5), wrong = !strncasecmp(r, "WRONG", 5);
		if (right || wrong) {
			r += 5;
			while (*r && strchr(" .,:;!-\n", *r)) r++;
			if (*r) snprintf(react, cap, "%s", r);
			else snprintf(react, cap, right ? "That's right, %s!" : "Not quite, %s. It was %s.", who, it->a);
			return right;
		}
	}
	/* (the AI couldn't say: a plain comparison) */
	char a[QUIZ_AMAX + 8], s[200];
	quiz_plain(it->a, a, sizeof(a));
	quiz_plain(said, s, sizeof(s));
	bool right = a[0] && s[0] && (strstr(s, a) || (strlen(s) >= 3 && strstr(a, s)));
	if (right) snprintf(react, cap, qline("Aye, that's right, %s!", "Yes! That's right, %s!"), who);
	else snprintf(react, cap, qline("Och, not quite, %s. It was %s.", "Oh, not quite, %s. It was %s."), who, it->a);
	return right;
}

static void quiz_scores(char *out, int cap, int np, char names[][16], const int *score) {
	int n = 0;
	for (int i = 0; i < np && n < cap; i++) n += snprintf(out + n, cap - n, "%s%s %d", i ? ",  " : "", names[i], score[i]);
}

/* The quiz: the host asks the topic, how many are playing, their names and
 * how many questions each, then asks them in turn and keeps score. */
static void quiz(const char *request) {
	char topic[120], reply[300], line[600];
	char names[QUIZ_PLAYERS][16];
	int score[QUIZ_PLAYERS] = { 0 }, np = 0, rounds = 0;
	quiz_stop = false;
	speech_stop();
	ui_chat("", "");
	/* the topic (unless it came with the request) */
	quiz_topic_in(request, topic, sizeof(topic));
	if (topic[0]) {
		snprintf(line, sizeof(line), qline("A quiz on %s? Och, splendid! How many players will we have? Up to four can play.",
		                                   "Ooh, a quiz on %s! I love it. How many players are there? Up to four can play."), topic);
		host(line, false);
	} else {
		host(qline("A quiz, is it? Och, I do love a good quiz! What topic shall it be?",
		           "Ooh, a quiz! Fun! What topic shall we do?"), false);
		for (;;) {
			int r = quiz_reply("Say or type a topic, or \"anything\"", reply, sizeof(reply));
			if (r < 0) { host(qline("Maybe another time, then!", "Okay, another time!"), false); return; }
			if (r == 2) { host(qline("What topic shall it be? Anything you like!", "What topic shall we do? Anything you like!"), false); continue; }
			quiz_topic(reply, topic, sizeof(topic));
			break;
		}
		snprintf(line, sizeof(line), qline("%s it is! And how many players will we have? Up to four.", "%s, great choice! How many players? Up to four can play."), topic);
		line[0] = (char)toupper((unsigned char)line[0]);
		host(line, false);
	}
	/* how many are playing */
	for (int tries = 0; !np; tries++) {
		int r = quiz_reply("How many players? (1 to 4)", reply, sizeof(reply));
		if (r < 0) { host(qline("Maybe another time, then!", "Okay, another time!"), false); return; }
		int k = r == 1 ? quiz_number(reply) : 0;
		if (k >= 1 && k <= QUIZ_PLAYERS) np = k;
		else if (k > QUIZ_PLAYERS) host(qline("Och, that's a crowd! Up to four can play. How many will it be?", "Oh, that's a lot of us! Up to four can play. How many?"), false);
		else host(qline("Sorry, how many players? One, two, three or four?", "Sorry, how many players? One, two, three or four?"), false);
	}
	/* their names */
	for (int i = 0; i < np; i++) {
		static const char *const nth[] = { "one", "two", "three", "four" };
		if (np == 1) snprintf(line, sizeof(line), "%s", qline("Grand! And what's your name?", "Great! And what's your name?"));
		else snprintf(line, sizeof(line), qline("Player %s, what's your name?", "Player %s, what's your name?"), nth[i]);
		host(line, false);
		for (;;) {
			char prompt[60];
			snprintf(prompt, sizeof(prompt), np == 1 ? "Your name" : "Player %d's name", i + 1);
			int r = quiz_reply(prompt, reply, sizeof(reply));
			if (r < 0) { host(qline("Maybe another time, then!", "Okay, another time!"), false); return; }
			if (r == 2) { host(line, false); continue; }
			quiz_name(reply, names[i], sizeof(names[i]), i);
			break;
		}
		if (np > 1 && i < np - 1) {
			snprintf(line, sizeof(line), qline("Welcome, %s!", "Hi, %s!"), names[i]);
			host(line, true);
		}
	}
	/* how many questions each */
	snprintf(line, sizeof(line), np == 1 ? qline("Lovely to meet you, %s! How many questions: five, ten or fifteen?", "Nice to meet you, %s! How many questions: five, ten or fifteen?")
	                                     : qline("Welcome, %s! Now, how many questions each: five, ten or fifteen?", "Hi, %s! Now, how many questions each: five, ten or fifteen?"),
	         names[np - 1]);
	host(line, false);
	while (!rounds) {
		int r = quiz_reply(np == 1 ? "How many questions? (5, 10 or 15)" : "How many questions each? (5, 10 or 15)", reply, sizeof(reply));
		if (r < 0) { host(qline("Maybe another time, then!", "Okay, another time!"), false); return; }
		int k = r == 1 ? quiz_number(reply) : 0;
		if (k > 0) rounds = k <= 7 ? 5 : k <= 12 ? 10 : 15;
		else host(qline("Five, ten or fifteen questions?", "Five, ten or fifteen questions?"), false);
	}
	/* the scoreboard appears by his shoulder */
	ui_quiz_board(np, (const char (*)[16])names, score, rounds, 0);
	/* the questions, a few at a time */
	int total = np * rounds;
	static char avoid[1200];
	avoid[0] = 0;
	quiz_nitems = quiz_next = 0;
	for (int turn = 0; turn < total; turn++) {
		int who = turn % np;
		if (quiz_next >= quiz_nitems) {
			int want = total - turn < 10 ? total - turn + 2 : 10;   /* (a couple spare, in case of a bad one) */
			if (!quiz_fetch(topic, want, avoid)) {
				if (quiz_stop) { ui_quiz_board_hide(); host(qline("Maybe another time, then!", "Okay, another time!"), false); return; }
				ui_quiz_board_hide();
				host(qline("Och, my brain's gone a wee bit foggy and I can't think of any questions just now. Let's try again later!",
				           "Oh no, I can't seem to find any questions just now. Let's try again later!"), false);
				return;
			}
		}
		QuizItem *it = &quiz_items[quiz_next++];
		size_t al = strlen(avoid);
		if (al + strlen(it->a) + 4 < sizeof(avoid)) snprintf(avoid + al, sizeof(avoid) - al, "%s%s", al ? "; " : "", it->a);
		if (turn == 0) {
			if (np == 1) snprintf(line, sizeof(line), qline("Right then! %d questions on %s. Here we go!", "Okay! %d questions on %s. Here we go!"), rounds, topic);
			else snprintf(line, sizeof(line), qline("Right then! %d questions each on %s. %s, you're up first!",
			                                        "Okay! %d questions each on %s. %s, you're first!"), rounds, topic, names[0]);
			host(line, true);
		}
		ui_quiz_board(np, (const char (*)[16])names, score, rounds, who);   /* (whose turn it is) */
		int qnum = turn / np + 1;
		if (np == 1) snprintf(line, sizeof(line), "Question %d. %s", qnum, it->q);
		else snprintf(line, sizeof(line), "%s, question %d. %s", names[who], qnum, it->q);
		host(line, false);
		char prompt[60];
		snprintf(prompt, sizeof(prompt), np == 1 ? "Your answer" : "%s's answer", names[who]);
		int r;
		while ((r = quiz_reply(prompt, reply, sizeof(reply))) == 2) host(line, false);
		if (r < 0) {
			quiz_scores(line, sizeof(line), np, names, score);
			char bye[700];
			if (np == 1) snprintf(bye, sizeof(bye), qline("Stopping there? Righto. You got %d out of %d. Well played!", "Stopping there? Okay! You got %d out of %d. Nicely done!"), score[0], turn);
			else snprintf(bye, sizeof(bye), qline("Stopping there? Righto. The scores were: %s. Well played, all!", "Stopping there? Okay! The scores were: %s. Nicely played, everyone!"), line);
			host(bye, true);
			ui_quiz_board_hide();
			return;
		}
		char react[400];
		bool right = quiz_judge(it, names[who], reply, react, sizeof(react));
		if (quiz_stop) r = -1;   /* (B while it was being judged: stopping, as above) */
		if (r < 0) {
			quiz_scores(line, sizeof(line), np, names, score);
			char bye[700];
			if (np == 1) snprintf(bye, sizeof(bye), qline("Stopping there? Righto. You got %d out of %d. Well played!", "Stopping there? Okay! You got %d out of %d. Nicely done!"), score[0], turn);
			else snprintf(bye, sizeof(bye), qline("Stopping there? Righto. The scores were: %s. Well played, all!", "Stopping there? Okay! The scores were: %s. Nicely played, everyone!"), line);
			host(bye, true);
			ui_quiz_board_hide();
			return;
		}
		if (right) { score[who]++; ui_quiz_point(who); ui_quiz_board(np, (const char (*)[16])names, score, rounds, who); }
		ui_layout(1);
		ui_chat(reply, react);
		speech_say(react);
		if (right) { ui_chuckle(); sound_play(SFX_CONFIRM); }
		while (speech_busy()) read_input();
		wait_frames(20);
	}
	/* the end */
	if (np == 1) {
		int s = score[0];
		const char *verdict = s * 10 >= total * 8 ? qline("Magnificent! A true scholar!", "Wow! Top of the class!")
		                    : s * 10 >= total * 5 ? qline("Well done indeed!", "Really nicely done!")
		                    : s > 0 ? qline("Not bad at all, and you'll do even better next time!", "Good effort, and next time you'll do even better!")
		                            : qline("Och, a tricky lot, those! Have another go sometime.", "Oh, those were tricky ones! Have another go sometime.");
		snprintf(line, sizeof(line), qline("And that's the end of the quiz! %s, you got %d out of %d. %s", "And that's the quiz! %s, you got %d out of %d. %s"),
		         names[0], s, total, verdict);
	} else {
		int best = 0;
		for (int i = 1; i < np; i++) if (score[i] > score[best]) best = i;
		int ties = 0;
		for (int i = 0; i < np; i++) if (score[i] == score[best]) ties++;
		char sc[300];
		quiz_scores(sc, sizeof(sc), np, names, score);
		if (ties > 1) snprintf(line, sizeof(line), qline("And that's the end of the quiz! The scores: %s. A tie at the top! Well played, all.",
		                                                 "And that's the quiz! The scores: %s. It's a tie! Nicely played, everyone."), sc);
		else snprintf(line, sizeof(line), qline("And that's the end of the quiz! The scores: %s. Our winner is %s! Well played, all.",
		                                        "And that's the quiz! The scores: %s. And the winner is %s! Nicely played, everyone."), sc, names[best]);
	}
	ui_quiz_board(np, (const char (*)[16])names, score, rounds, -1);
	ui_chuckle();
	sound_play(SFX_CONFIRM);
	host(line, true);
	wait_frames(120);   /* (the final scores stay up a moment) */
	ui_quiz_board_hide();
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

/* ------------------------------------------------------- saying hello */

/* (1.4) What they say when BrainBox starts: a few to choose from, some for
 * the time of day, and some for when they remember your name. */
static const char *const HELLO_SCOTTY[] = {
	"Hello there! I'm Professor Scotty. What would you like to know today?",
	"Ah! There you are. Professor Scotty, at your service. What shall we puzzle over today?",
	"Hmm? Oh, hello! I was just giving my globe a wee polish. What's on your mind?",
	"Och, a visitor! Come in, come in. Ask me anything you like.",
	"Ah, good to see you! My circuits are warm and my bow tie's straight. What shall we learn today?",
	"Hmm, now where did I put my spectacles? Ah, never mind! Hello there. What would you like to know?",
	"Ah! All systems at full power, as a certain engineer I know would say. What can I help you with?",
	"Hello, hello! A fine day for a good question, aye? Let's hear it.",
};
static const char *const HELLO_SCOTTY_TIME[4] = {
	"Good morning! Professor Scotty here, bright-eyed and ready. What shall we explore today?",
	"Good afternoon! A grand time for a wee bit of learning, aye? What's your question?",
	"Good evening! Settle in by the fire, and let's see what we can discover tonight.",
	"Burning the midnight oil, are we? Och, me too. What's on your mind?",
};
static const char *const HELLO_SCOTTY_NAME[] = {
	"Ah, %s! Welcome back. What shall we puzzle over today?",
	"Hello again, %s! My globe's been spinning away, waiting for you. What's on your mind?",
	"%s! There you are, my friend. Ask me anything you like.",
};
static const char *const HELLO_HER[] = {
	"Hi there! I'm " UI_HER_NAME ". What would you like to know today?",
	"Oh! Hello! I was just reshelving the poetry section. What can I help you find?",
	"Ooh, a visitor! Pull up a chair. What are we curious about today?",
	"Hi! " UI_HER_NAME ", head librarian and keeper of odd facts. Ask me anything.",
	"Oh, hello! I've got my glasses on and a fresh cup of tea. What shall we look into?",
	"Hi there! Every good question is the start of a story. What's yours today?",
	"Ooh, perfect timing, I just finished a chapter. What would you like to talk about?",
};
static const char *const HELLO_HER_TIME[4] = {
	"Good morning! The library's open and the coffee's on. What can I help you with?",
	"Good afternoon! Perfect time for a little reading, or a big question. What's on your mind?",
	"Good evening! Get comfy, and tell me what you're wondering about.",
	"Oh! Up late with a good question? That's my favorite kind. What is it?",
};
static const char *const HELLO_HER_NAME[] = {
	"Oh, %s! Welcome back. What are we curious about today?",
	"Hi again, %s! I saved you a seat by the window. What's on your mind?",
	"Ooh, %s! I was hoping you'd stop by. What shall we look into?",
};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void greeting(char *out, int cap) {
	bool her = ui_set.character == UI_CHAR_HER;
	static bool seeded;
	if (!seeded) { srand((unsigned)gettime()); seeded = true; }
	const char *name = memory_name();
	int pick = rand() % 6;
	if (name[0] && pick < 3) {   /* (half the time, when they know your name) */
		const char *const *list = her ? HELLO_HER_NAME : HELLO_SCOTTY_NAME;
		snprintf(out, cap, list[rand() % (her ? COUNT(HELLO_HER_NAME) : COUNT(HELLO_SCOTTY_NAME))], name);
	} else if (pick == 3 || (!name[0] && pick == 0)) {   /* (now and then: the time of day) */
		int h = weather_local_hour();
		int part = h >= 5 && h < 12 ? 0 : h >= 12 && h < 18 ? 1 : h >= 18 && h < 23 ? 2 : 3;
		snprintf(out, cap, "%s", her ? HELLO_HER_TIME[part] : HELLO_SCOTTY_TIME[part]);
	} else {
		snprintf(out, cap, "%s", her ? HELLO_HER[rand() % COUNT(HELLO_HER)] : HELLO_SCOTTY[rand() % COUNT(HELLO_SCOTTY)]);
	}
}

/* (1.4) How long starting up takes, step by step: written to boot.txt beside
 * the app when display.fps=1 is in brainbox.ini (with the frame timings). */
static u64 boot_t0;
static char boot_log[8000];
static void boot_mark(const char *what) {
	size_t n = strlen(boot_log);
	snprintf(boot_log + n, sizeof(boot_log) - n, "%6u ms  %s\n", (unsigned)ticks_to_millisecs(gettime() - boot_t0), what);
}
static void boot_mark_at(const char *what, u64 at) {
	size_t n = strlen(boot_log);
	if (at) snprintf(boot_log + n, sizeof(boot_log) - n, "%6u ms  %s\n", (unsigned)ticks_to_millisecs(at - boot_t0), what);
}
static void boot_save(void) {
	if (!ui_set.fps || !storage_ok()) return;
	boot_mark_at("first picture on the TV", ui_first_frame_at);
	boot_mark_at("first picture with the hologram made", ui_head_frame_at);
	size_t n = strlen(boot_log);
	if (n < sizeof(boot_log) - 100) { boot_log[n++] = '\n'; boot_log[n] = 0; ui_intro_frames(boot_log + n, (int)(sizeof(boot_log) - n)); }
	char path[300];
	storage_path(path, sizeof(path), "boot.txt");
	storage_write_atomic(path, boot_log, strlen(boot_log));
}

int main(int argc, char **argv) {
	boot_t0 = gettime();
	ui_init();
	boot_mark("screen started");
	listen_init();
	sound_init();
	speech_init();
	boot_mark("sound and speech started");
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
	ui_character_known();   /* (now he can be made: the right hologram, first time) */
	memory_enable(ui_set.memory);
	memory_load();   /* (what he remembers about you) */
	boot_mark("SD card and settings read");
	if (storage_ok()) update_remove_old();   /* (the version an update replaced, now this one has started) */
	use_character();   /* (the hologram's voice, loaded in the background) */
	use_brain();
	ai_hear_filter(hear_filter);   /* (his little secrets, and the quiz, aren't sent on to the AI) */
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
		boot_mark("opening's sound ready: opening starts");
		ui_intro_start();
		sound_play_holo();
		while (!ui_intro_done()) read_input();
	} else ui_intro_skip();
	/* his voice starts in the background too (a second or so: usually ready by now) */
	if (!speech_ready()) ui_caption(UI_CAP_INFO, "Warming up\x85");
	boot_mark("opening finished");
	ensure_voice();
	boot_mark("voice ready");
	ui_caption(UI_CAP_NONE, "");
	if (!storage_ok()) ui_toast("There's no SD card: settings won't be kept.");

	if (!ui_set.key[ui_set.brain][0]) {
		/* no brain yet: his head's a little foggy */
		ui_fog(1);
		ui_pose(0.6f, 0, true);
		say("", "Oh my... my head's a little foggy today. I seem to have misplaced my brain! Would you lend me one? "
		        "Just choose your favorite AI and sign me in.");
		boot_greeted = true;
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
		char hello[200];
		greeting(hello, sizeof(hello));
		say("", hello);
		boot_greeted = true;
	}
	boot_mark("first words said");
	boot_save();
	has_mic = listen_present();
	conversation();
	return 0;
}

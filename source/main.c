/* main.c - Professor Pixel: a hologram professor on the GameCube who answers
 * your questions with the AI of your choice (Google Gemini, Claude or
 * ChatGPT) and speaks his answers aloud.
 *
 * The main thread runs the conversation, reads the controller and the
 * keyboard, and tells the drawing thread (ui.c) what to show. */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
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
#include "dolload.h"
#include "net/storage.h"
#include "net/qrcodegen.h"

const char appversion[] = "v1.0";

/* ---------------------------------------------------------------- input */

static u32 held, down, held_prev;
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
	if (down & PAD_BUTTON_B) sound_play(SFX_BACK);
	else if (down & (PAD_BUTTON_A | PAD_BUTTON_X | PAD_BUTTON_Y | PAD_BUTTON_START)) sound_play(SFX_SELECT);
	else if (down & dirs) sound_play(SFX_HOVER);
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

/* ------------------------------------------------------- what he says */

static void say(const char *you, const char *him) {
	ui_layout(1);
	ui_chat(you, him);
	speech_say(him);
}

static void use_brain(void) {
	int b = ui_set.brain;
	ai_set((AiBrain)b, ui_set.key[b], ui_set.model[b]);
}

static void save_settings(void) {
	if (!storage_ok()) { ui_toast("No SD card: these settings last until Professor Pixel closes."); return; }
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
	static const char *subs[3] = { "Google account and an API key", "Anthropic account and an API key", "OpenAI account and an API key" };
	static const char *notes[3] = {
		"Has a free allowance with daily limits. You'll make a key on your phone or computer and send it to the GameCube.",
		"Pay as you go: a short chat costs a fraction of a cent. Make a key on your phone or computer and send it here.",
		"Pay as you go: a short chat costs a fraction of a cent. Make a key on your phone or computer and send it here." };
	int row = ui_set.brain;
	for (;;) {
		ui_overlay(UI_OVERLAY_BRAIN, "Which AI should Professor Pixel use?");
		for (int i = 0; i < 3; i++) {
			ui_overlay_row(i, ai_name((AiBrain)i), subs[i]);
			ui_overlay_tag(i, ui_set.key[i][0] ? "Signed in" : i == 0 ? "Free tier" : "Pay as you go", i == 0 || ui_set.key[i][0]);
		}
		ui_overlay_rows(3);
		ui_overlay_select(row);
		ui_overlay_note(ai_name((AiBrain)row), notes[row]);
		hints(3, "S:Move", "A:Choose", "B:Back");
		for (;;) {
			read_input();
			if (down & (PAD_BUTTON_UP | PAD_BUTTON_DOWN)) {
				row = (row + ((down & PAD_BUTTON_UP) ? 2 : 1)) % 3;
				ui_overlay_select(row);
				ui_overlay_note(ai_name((AiBrain)row), notes[row]);
			}
			if (down & PAD_BUTTON_B) { ui_overlay(UI_OVERLAY_NONE, ""); return ui_set.key[ui_set.brain][0] != 0; }
			if (down & PAD_BUTTON_A) break;
		}
		if (ui_set.key[row][0]) {   /* already signed in: just switch */
			ui_set.brain = row;
			use_brain();
			save_settings();
			ui_overlay(UI_OVERLAY_NONE, "");
			return true;
		}
		wait_release();
		if (sign_in(row)) return true;
	}
}

/* ----------------------------------------------------------- Settings */

enum { S_RES, S_SCREEN, S_WIDTH, S_OVERSCAN, S_VOICE, S_SOUNDS, S_CAPTIONS, S_BRAIN, S_LAUNCHER, S_EXIT, S_ROWS };

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
	ui_overlay_row(S_CAPTIONS, "Captions", ui_set.captions ? "On" : "Off");
	ui_overlay_row(S_BRAIN, "Brain", br);
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
		{ "Voice volume", "How loud Professor Pixel speaks. Off for none." },
		{ "Menu sounds", "How loud the sounds are when you move and choose. Off for none." },
		{ "Captions", "Shows his answers in a bubble as he speaks." },
		{ "Brain", "Which AI he uses: Google Gemini, Claude or ChatGPT. A to change or sign in." },
		{ "Launcher", "The program at the root of the SD card that Exit to Launcher starts." },
		{ "Exit to Launcher", "Leaves Professor Pixel." },
	};
	ui_status(help[row][0], help[row][1]);
	if (row >= S_BRAIN && row != S_LAUNCHER) hints(3, "S:Move", "A:Select", "B:Back");
	else hints(3, "S:Move", "A:Change", "B:Back");
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
	for (;;) {
		read_input();
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
	wait_release();
}

/* ------------------------------------------------------- the conversation */

static const char *idle_caption(void) {
	return kb_connected() ? "Start typing your question\x85" : "Plug in the keyboard controller to ask a question";
}

static void idle_hints(void) {
	if (ui_chat_more() > 0) hints(4, "A:Ask", "S:Scroll", "ST:Settings", "B:Exit to Launcher");
	else hints(3, "A:Ask", "ST:Settings", "B:Exit to Launcher");
}

/* asks the question and has him answer it */
static void ask(const char *q) {
	ui_layout(1);
	ui_chat(q, "");
	ui_caption(UI_CAP_THINK, "Hmm, let me think about that\x85");
	ui_pose(1, 0, true);
	hints(1, "B:Never mind");
	while (ai_state() == AI_BUSY) read_input();   /* (a question he was told to never mind, still on its way) */
	use_brain();
	ai_ask(q);
	bool cancelled = false;
	while (ai_state() == AI_BUSY) {
		read_input();
		if (down & PAD_BUTTON_B) { cancelled = true; break; }
	}
	ui_pose(0, 0.6f, true);
	ui_caption(UI_CAP_NONE, "");
	if (cancelled) {
		/* (the answer arrives later and is dropped) */
		say(q, "Oh, never mind then! Ask me anything else.");
		return;
	}
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

static void conversation(void) {
	bool exit_armed = false;
	u64 exit_at = 0;
	ui_caption(UI_CAP_INFO, idle_caption());
	idle_hints();
	bool was_kb = kb_connected();
	for (;;) {
		read_input();
		u64 now = ticks_to_millisecs(gettime());
		if (exit_armed && now - exit_at > 3000) exit_armed = false;
		if (kb_connected() != was_kb) { was_kb = kb_connected(); ui_caption(UI_CAP_INFO, idle_caption()); }
		/* a key typed on the keyboard starts a question */
		KbEvent e;
		char q[400] = "";
		bool start = false;
		while (kb_next(&e)) {
			if (e.key == KB_CHAR && (unsigned char)e.ch > 32) { size_t n = strlen(q); if (n < sizeof(q) - 1) { q[n] = e.ch; q[n + 1] = 0; } start = true; }
			if (e.key == KB_ENTER) start = true;
		}
		if (down & PAD_BUTTON_A) {
			if (!kb_connected()) { ui_toast("Plug in the keyboard controller to type a question."); continue; }
			start = true;
		}
		if (start) {
			speech_stop();
			if (type_line("Type your question, then press Enter", q, sizeof(q), false)) ask(q);
			ui_caption(UI_CAP_INFO, idle_caption());
			idle_hints();
			continue;
		}
		if (down & PAD_BUTTON_UP) { ui_chat_scroll(-1); idle_hints(); }
		if (down & PAD_BUTTON_DOWN) { ui_chat_scroll(3); idle_hints(); }
		if (down & PAD_BUTTON_X) speech_stop();
		if (down & PAD_BUTTON_START) { speech_stop(); settings(); ui_caption(UI_CAP_INFO, idle_caption()); idle_hints(); }
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
	sound_init();
	speech_init();
	storage_set_program(argc > 0 && argv ? argv[0] : NULL);
	if (storage_init()) {
		char path[300];
		storage_path(path, sizeof(path), "pixel.ini");
		ui_settings_path(path);
		ui_settings_load();
	}
	use_brain();
	ui_layout(0);
	ui_pose(0, 0, true);
	ui_caption(UI_CAP_INFO, "Warming up\x85");
	hints(0);
	/* his voice starts in the background (a second or so) */
	for (int k = 0; k < 600 && !speech_ready(); k++) read_input();
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
		say("", "Hello there! I'm Professor Pixel. What would you like to know today?");
	}
	conversation();
	return 0;
}

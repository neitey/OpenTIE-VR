#include "tie_app/test/test_harness.h"

#include "aeron/log.h"
#include "tie/shell.h"
#include "tie/shipext.h"
#include "tie_runtime/runtime/shell_task.h"
#include "tie_runtime/snapshot/snapshot.h"
#include <SDL3/SDL.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * Script (TIE_TEST_SCRIPT): one command per line, '#' starts a comment.
 *
 *   <frame> <command> args...
 *
 * frame: N (absolute host frame) or +N (relative to the previous line, or to
 * the frame a 'wait' finished on). Lines run in order; a line never runs
 * before the one above it.
 *
 *   key <name> [hold]        press for `hold` frames (3); SDL scancode names
 *                            (Return Space Escape Up A F1 Backspace [ ] ;),
 *                            optional ctrl+/shift+/alt+/gui+ prefixes
 *   keydown <name> / keyup <name>
 *   text <string>            typed text, one character every other frame
 *   mouse <x> <y> [click|rclick|down|up|rdown|rup]
 *                            logical coordinates (1440x1080 for 4:3 =
 *                            640x480 frontend pixels * 2.25)
 *   axis <yaw|pitch|roll|SDL gamepad axis> <-1..1>    virtual gamepad
 *   button <SDL gamepad button> [hold] / buttondown / buttonup
 *   head <yaw> <pitch> [roll [x y z]]   fake XR head pose (deg, m; +yaw
 *                            looks left, +pitch up)
 *   capture [label]          PNGs this frame (window, XR targets)
 *   wait scene <frontend|briefing|hangar|flight|debrief|cutscene|loading>
 *   wait shell <scene id>    e.g. 100 registration, 110 concourse
 *   wait flying [n]          normal flight screen, n flight frames stepped
 *                            (all waits: optional timeout=<frames>, 3600)
 *   mission <m>              (frame 0) same as TIE_TEST_MISSION=<m>
 *   fake XR controllers (TIE_TEST_XR=1):
 *   xrpad                    the virtual gamepad takes the engine's XR Touch
 *                            pad identity (before the first axis/button)
 *   xrbutton a|b|x|y|lt|ls|rs|rgrip   one press for Aeron_XrPollButton
 *   xrmenu a|b|x|y           latch for Aeron_XrTakeAPressed/TakeMenu?Pressed
 *   xrthrottle <0..1>        Aeron_XrThrottleLever
 *   xrhand left|right <trigger> [grip]   Aeron_XrGetHand trigger/grip
 *   xrpose left|right <x> <y> <z> [yaw pitch]   hand pose (m LOCAL, deg),
 *                            replaces TIE_TEST_XR_LHAND/RHAND from then on
 *   (the game's flight stick, Aeron_XrSetVirtualStick, moves the xrpad's
 *   right-stick axes like the engine does: logged as "xr vstick")
 *   log <text>, quit
 * ------------------------------------------------------------------------ */

enum {
	TT_CMD_KEY,
	TT_CMD_KEYDOWN,
	TT_CMD_KEYUP,
	TT_CMD_TEXT,
	TT_CMD_MOUSE,
	TT_CMD_AXIS,
	TT_CMD_BUTTON,
	TT_CMD_BUTTONDOWN,
	TT_CMD_BUTTONUP,
	TT_CMD_HEAD,
	TT_CMD_CAPTURE,
	TT_CMD_WAIT,
	TT_CMD_LOG,
	TT_CMD_QUIT,
	TT_CMD_MISSION,
	TT_CMD_XRBUTTON,
	TT_CMD_XRMENU,
	TT_CMD_XRTHROTTLE,
	TT_CMD_XRHAND,
	TT_CMD_XRPOSE,
	TT_CMD_XRPAD,
};

enum {
	TT_WAIT_SCENE,
	TT_WAIT_SHELL,
	TT_WAIT_FLYING,
};

enum {
	TT_MOUSE_MOVE,
	TT_MOUSE_CLICK,
	TT_MOUSE_RCLICK,
	TT_MOUSE_DOWN,
	TT_MOUSE_UP,
	TT_MOUSE_RDOWN,
	TT_MOUSE_RUP,
};

typedef struct TieTestLine {
	int line_no;
	int relative;
	int64_t frame;
	int cmd;
	int key;      /* AeronKey */
	uint8_t mods; /* AERON_KEY_MOD_* */
	int hold;
	int ivalue[3];
	float fvalue[6];
	int fcount;
	int64_t timeout;
	char text[160];
} TieTestLine;

#define TT_MAX_PENDING 64
typedef struct TieTestPendingRelease {
	int64_t frame;
	int kind; /* 0 key, 1 mouse button, 2 gamepad button */
	int code;
	uint8_t mods;
} TieTestPendingRelease;

#define TT_MAX_CAPTURE_FRAMES 256

static struct {
	bool active;
	bool verbose;
	uint64_t frame;
	int exit_status;
	bool exit_requested;
	int64_t exit_frame;
	int window_w, window_h;
	/* script */
	TieTestLine* lines;
	int line_count;
	int next_line;
	int64_t base_frame;
	int64_t wait_started;
	uint32_t wait_flight_frame;
	bool wait_logged;
	/* captures */
	char capture_dir[512];
	int64_t capture_frames[TT_MAX_CAPTURE_FRAMES];
	int capture_frame_count;
	int64_t capture_every;
	bool capture_now;
	char capture_label[64];
	bool capture_armed;
	char capture_prefix[640];
	/* injected state */
	uint8_t key_down[AERON_KEY_COUNT];
	TieTestPendingRelease pending[TT_MAX_PENDING];
	int pending_count;
	char text_queue[256];
	int text_len;
	int text_pos;
	bool mouse_used;
	int mouse_x, mouse_y;
	uint32_t mouse_buttons;
	uint32_t mouse_pressed, mouse_released;
	bool pad_used;
	bool pad_is_xr;
	int16_t pad_axes[AERON_GAMEPAD_AXIS_COUNT];
	uint32_t pad_buttons;
	uint32_t pad_prev_buttons;
	/* scene tracking for the log */
	int last_scene_kind;
	int last_shell_scene;
	int last_flight_screen;
	uint64_t start_ticks_ms;
} g_test = {
	.exit_frame = -1,
	.last_scene_kind = -1,
	.last_shell_scene = -1,
	.last_flight_screen = -1,
};

static void TieTest_Log(const char* fmt, ...) SDL_PRINTF_VARARG_FUNC(1);
static void TieTest_Log(const char* fmt, ...) {
	char buffer[1024];
	va_list args;
	va_start(args, fmt);
	SDL_vsnprintf(buffer, sizeof buffer, fmt, args);
	va_end(args);
	/* Straight to stdout as well: the scripts grep one stream. */
	fprintf(stdout, "[tie-test] frame %llu: %s\n", (unsigned long long)g_test.frame, buffer);
	fflush(stdout);
}

static const char* const k_scene_names[] = { "frontend", "briefing", "hangar", "flight",
											 "debrief",  "cutscene", "loading" };

static int TieTest_SceneFromName(const char* name) {
	for (int i = 0; i < (int)SDL_arraysize(k_scene_names); ++i)
		if (SDL_strcasecmp(name, k_scene_names[i]) == 0)
			return i;
	return -1;
}

static const char* TieTest_SceneName(int kind) {
	return kind >= 0 && kind < (int)SDL_arraysize(k_scene_names) ? k_scene_names[kind] : "?";
}

static int TieTest_ShellScene(void) {
	/* sHead_gbl points at the shell task's first member. */
	return sHead_gbl ? ((const ShellTask*)(const void*)sHead_gbl)->cur_scene : -1;
}

/* ---- key specs: [ctrl+][shift+][alt+][gui+]<SDL scancode name> ---- */

static bool TieTest_ParseKey(const char* spec, int* out_key, uint8_t* out_mods) {
	uint8_t mods = 0;
	for (;;) {
		static const struct {
			const char* prefix;
			uint8_t mod;
		} prefixes[] = {
			{ "ctrl+", AERON_KEY_MOD_CTRL },
			{ "shift+", AERON_KEY_MOD_SHIFT },
			{ "alt+", AERON_KEY_MOD_ALT },
			{ "gui+", AERON_KEY_MOD_GUI },
		};
		bool matched = false;
		for (size_t i = 0; i < SDL_arraysize(prefixes); ++i) {
			const size_t n = strlen(prefixes[i].prefix);
			if (SDL_strncasecmp(spec, prefixes[i].prefix, n) == 0 && spec[n]) {
				mods |= prefixes[i].mod;
				spec += n;
				matched = true;
			}
		}
		if (!matched)
			break;
	}
	AeronKey key;
	if (!AeronKey_FromName(spec, &key))
		return false;
	*out_key = (int)key;
	*out_mods = mods;
	return true;
}

static int TieTest_ModifierKey(uint8_t mod) {
	switch (mod) {
		case AERON_KEY_MOD_CTRL:
			return AERON_KEY_LCTRL;
		case AERON_KEY_MOD_SHIFT:
			return AERON_KEY_LSHIFT;
		case AERON_KEY_MOD_ALT:
			return AERON_KEY_LALT;
		default:
			return AERON_KEY_LGUI;
	}
}

/* ---- script parsing ---- */

static char* TieTest_NextToken(char** cursor) {
	char* s = *cursor;
	while (*s && isspace((unsigned char)*s))
		++s;
	if (!*s)
		return NULL;
	char* start = s;
	while (*s && !isspace((unsigned char)*s))
		++s;
	if (*s)
		*s++ = '\0';
	*cursor = s;
	return start;
}

static bool TieTest_ParseLine(char* text, int line_no, TieTestLine* out) {
	memset(out, 0, sizeof *out);
	out->line_no = line_no;
	char* hash = strchr(text, '#');
	if (hash)
		*hash = '\0';
	char* cursor = text;
	char* tok = TieTest_NextToken(&cursor);
	if (!tok)
		return false;
	if (tok[0] == '+') {
		out->relative = 1;
		out->frame = strtoll(tok + 1, NULL, 10);
	} else if (isdigit((unsigned char)tok[0])) {
		out->frame = strtoll(tok, NULL, 10);
	} else {
		TieTest_Log("script line %d: expected a frame number, got '%s'", line_no, tok);
		return false;
	}
	char* cmd = TieTest_NextToken(&cursor);
	if (!cmd) {
		TieTest_Log("script line %d: missing command", line_no);
		return false;
	}
	static const struct {
		const char* name;
		int cmd;
	} commands[] = {
		{ "key", TT_CMD_KEY },
		{ "keydown", TT_CMD_KEYDOWN },
		{ "keyup", TT_CMD_KEYUP },
		{ "text", TT_CMD_TEXT },
		{ "mouse", TT_CMD_MOUSE },
		{ "axis", TT_CMD_AXIS },
		{ "button", TT_CMD_BUTTON },
		{ "buttondown", TT_CMD_BUTTONDOWN },
		{ "buttonup", TT_CMD_BUTTONUP },
		{ "head", TT_CMD_HEAD },
		{ "capture", TT_CMD_CAPTURE },
		{ "wait", TT_CMD_WAIT },
		{ "log", TT_CMD_LOG },
		{ "quit", TT_CMD_QUIT },
		{ "mission", TT_CMD_MISSION },
		{ "xrbutton", TT_CMD_XRBUTTON },
		{ "xrmenu", TT_CMD_XRMENU },
		{ "xrthrottle", TT_CMD_XRTHROTTLE },
		{ "xrhand", TT_CMD_XRHAND },
		{ "xrpose", TT_CMD_XRPOSE },
		{ "xrpad", TT_CMD_XRPAD },
	};
	out->cmd = -1;
	for (size_t i = 0; i < SDL_arraysize(commands); ++i)
		if (SDL_strcasecmp(cmd, commands[i].name) == 0)
			out->cmd = commands[i].cmd;
	if (out->cmd < 0) {
		TieTest_Log("script line %d: unknown command '%s'", line_no, cmd);
		return false;
	}
	char* arg;
	switch (out->cmd) {
		case TT_CMD_KEY:
		case TT_CMD_KEYDOWN:
		case TT_CMD_KEYUP:
			arg = TieTest_NextToken(&cursor);
			if (!arg || !TieTest_ParseKey(arg, &out->key, &out->mods)) {
				TieTest_Log("script line %d: bad key '%s' (SDL scancode names, e.g. Return, Space, A, "
							"shift+Z)",
							line_no, arg ? arg : "");
				return false;
			}
			arg = TieTest_NextToken(&cursor);
			out->hold = arg ? atoi(arg) : 3;
			if (out->hold < 1)
				out->hold = 1;
			return true;
		case TT_CMD_TEXT:
		case TT_CMD_LOG:
		case TT_CMD_MISSION:
			while (*cursor && isspace((unsigned char)*cursor))
				++cursor;
			SDL_strlcpy(out->text, cursor, sizeof out->text);
			for (size_t n = strlen(out->text); n > 0 && isspace((unsigned char)out->text[n - 1]); --n)
				out->text[n - 1] = '\0';
			return true;
		case TT_CMD_MOUSE: {
			char* x = TieTest_NextToken(&cursor);
			char* y = TieTest_NextToken(&cursor);
			if (!x || !y) {
				TieTest_Log("script line %d: mouse <x> <y> [click|rclick|down|up|rdown|rup]", line_no);
				return false;
			}
			out->ivalue[0] = atoi(x);
			out->ivalue[1] = atoi(y);
			arg = TieTest_NextToken(&cursor);
			static const char* const actions[] = { "move", "click", "rclick", "down", "up", "rdown", "rup" };
			out->ivalue[2] = TT_MOUSE_MOVE;
			for (int i = 0; arg && i < (int)SDL_arraysize(actions); ++i)
				if (SDL_strcasecmp(arg, actions[i]) == 0)
					out->ivalue[2] = i;
			return true;
		}
		case TT_CMD_AXIS: {
			char* name = TieTest_NextToken(&cursor);
			char* value = TieTest_NextToken(&cursor);
			if (!name || !value) {
				TieTest_Log("script line %d: axis <yaw|pitch|roll|leftx|...> <-1..1>", line_no);
				return false;
			}
			const char* alias = SDL_strcasecmp(name, "yaw") == 0     ? "leftx"
								: SDL_strcasecmp(name, "pitch") == 0 ? "lefty"
								: SDL_strcasecmp(name, "roll") == 0  ? "rightx"
																	 : name;
			const AeronGamepadAxis axis = Aeron_GamepadAxisFromName(alias);
			if ((int)axis < 0 || axis >= AERON_GAMEPAD_AXIS_COUNT) {
				TieTest_Log("script line %d: unknown axis '%s'", line_no, name);
				return false;
			}
			out->ivalue[0] = (int)axis;
			out->fvalue[0] = (float)SDL_atof(value);
			return true;
		}
		case TT_CMD_BUTTON:
		case TT_CMD_BUTTONDOWN:
		case TT_CMD_BUTTONUP: {
			char* name = TieTest_NextToken(&cursor);
			const AeronGamepadButton button = name ? Aeron_GamepadButtonFromName(name) : (AeronGamepadButton)-1;
			if ((int)button < 0 || button >= AERON_GAMEPAD_BUTTON_COUNT) {
				TieTest_Log("script line %d: unknown gamepad button '%s'", line_no, name ? name : "");
				return false;
			}
			out->ivalue[0] = (int)button;
			arg = TieTest_NextToken(&cursor);
			out->hold = arg ? atoi(arg) : 3;
			if (out->hold < 1)
				out->hold = 1;
			return true;
		}
		case TT_CMD_HEAD:
			while (out->fcount < 6 && (arg = TieTest_NextToken(&cursor)) != NULL)
				out->fvalue[out->fcount++] = (float)SDL_atof(arg);
			if (out->fcount < 2) {
				TieTest_Log("script line %d: head <yaw> <pitch> [roll [x y z]] (degrees, metres)", line_no);
				return false;
			}
			return true;
		case TT_CMD_CAPTURE:
			arg = TieTest_NextToken(&cursor);
			if (arg)
				SDL_strlcpy(out->text, arg, sizeof out->text);
			return true;
		case TT_CMD_XRBUTTON:
		case TT_CMD_XRMENU: {
			static const char* const buttons[] = { "a", "b", "x", "y", "lt", "ls", "rs", "rgrip" };
			arg = TieTest_NextToken(&cursor);
			out->ivalue[0] = -1;
			const int count = out->cmd == TT_CMD_XRMENU ? 4 : (int)SDL_arraysize(buttons);
			for (int i = 0; arg && i < count; ++i)
				if (SDL_strcasecmp(arg, buttons[i]) == 0)
					out->ivalue[0] = i;
			if (out->ivalue[0] < 0) {
				TieTest_Log("script line %d: %s", line_no,
							out->cmd == TT_CMD_XRMENU ? "xrmenu a|b|x|y" : "xrbutton a|b|x|y|lt|ls|rs|rgrip");
				return false;
			}
			return true;
		}
		case TT_CMD_XRTHROTTLE:
			arg = TieTest_NextToken(&cursor);
			out->fvalue[0] = arg ? (float)SDL_atof(arg) : 0.0f;
			return arg != NULL;
		case TT_CMD_XRHAND: {
			arg = TieTest_NextToken(&cursor);
			char* trigger = TieTest_NextToken(&cursor);
			char* grip = TieTest_NextToken(&cursor);
			if (!arg || !trigger || (SDL_strcasecmp(arg, "left") && SDL_strcasecmp(arg, "right"))) {
				TieTest_Log("script line %d: xrhand left|right <trigger 0..1> [grip 0..1]", line_no);
				return false;
			}
			out->ivalue[0] = SDL_strcasecmp(arg, "right") == 0;
			out->fvalue[0] = (float)SDL_atof(trigger);
			out->fvalue[1] = grip ? (float)SDL_atof(grip) : 0.0f;
			return true;
		}
		case TT_CMD_XRPOSE: {
			arg = TieTest_NextToken(&cursor);
			if (!arg || (SDL_strcasecmp(arg, "left") && SDL_strcasecmp(arg, "right"))) {
				TieTest_Log("script line %d: xrpose left|right <x> <y> <z> [yaw pitch]", line_no);
				return false;
			}
			out->ivalue[0] = SDL_strcasecmp(arg, "right") == 0;
			float pose[5] = { 0.0f, 0.0f, 0.0f, out->ivalue[0] ? 10.0f : -10.0f, 30.0f };
			int n = 0;
			for (char* t; n < 5 && (t = TieTest_NextToken(&cursor)) != NULL; ++n)
				pose[n] = (float)SDL_atof(t);
			if (n < 3) {
				TieTest_Log("script line %d: xrpose left|right <x> <y> <z> [yaw pitch]", line_no);
				return false;
			}
			SDL_memcpy(out->fvalue, pose, sizeof pose);
			out->fcount = 5;
			return true;
		}
		case TT_CMD_WAIT: {
			char* what = TieTest_NextToken(&cursor);
			out->timeout = 3600;
			if (!what) {
				TieTest_Log("script line %d: wait scene <kind> | shell <n> | flying [frames]", line_no);
				return false;
			}
			if (SDL_strcasecmp(what, "scene") == 0) {
				arg = TieTest_NextToken(&cursor);
				out->ivalue[0] = TT_WAIT_SCENE;
				out->ivalue[1] = arg ? TieTest_SceneFromName(arg) : -1;
				if (out->ivalue[1] < 0) {
					TieTest_Log("script line %d: scene kinds: frontend briefing hangar flight debrief "
								"cutscene loading",
								line_no);
					return false;
				}
			} else if (SDL_strcasecmp(what, "shell") == 0) {
				arg = TieTest_NextToken(&cursor);
				out->ivalue[0] = TT_WAIT_SHELL;
				out->ivalue[1] = arg ? atoi(arg) : -1;
			} else if (SDL_strcasecmp(what, "flying") == 0) {
				out->ivalue[0] = TT_WAIT_FLYING;
				out->ivalue[1] = 1;
			} else {
				TieTest_Log("script line %d: unknown wait '%s'", line_no, what);
				return false;
			}
			while ((arg = TieTest_NextToken(&cursor)) != NULL) {
				if (SDL_strncasecmp(arg, "timeout=", 8) == 0)
					out->timeout = strtoll(arg + 8, NULL, 10);
				else if (out->ivalue[0] == TT_WAIT_FLYING)
					out->ivalue[1] = atoi(arg);
			}
			return true;
		}
		default:
			return true;
	}
}

static bool TieTest_LoadScript(const char* path) {
	FILE* file = fopen(path, "r");
	if (!file) {
		TieTest_Log("cannot open TIE_TEST_SCRIPT %s: %s", path, strerror(errno));
		return false;
	}
	char line[512];
	int line_no = 0;
	int capacity = 0;
	bool ok = true;
	while (fgets(line, sizeof line, file)) {
		++line_no;
		char* p = line;
		while (*p && isspace((unsigned char)*p))
			++p;
		if (!*p || *p == '#')
			continue;
		TieTestLine parsed;
		if (!TieTest_ParseLine(p, line_no, &parsed)) {
			ok = false;
			continue;
		}
		if (g_test.line_count == capacity) {
			capacity = capacity ? capacity * 2 : 64;
			TieTestLine* grown = realloc(g_test.lines, (size_t)capacity * sizeof *grown);
			if (!grown) {
				ok = false;
				break;
			}
			g_test.lines = grown;
		}
		g_test.lines[g_test.line_count++] = parsed;
	}
	fclose(file);
	TieTest_Log("script %s: %d commands%s", path, g_test.line_count, ok ? "" : " (with errors, see above)");
	return ok;
}

static void TieTest_ParseCaptureFrames(const char* list) {
	if (!list || !list[0])
		return;
	if (SDL_strncasecmp(list, "every:", 6) == 0) {
		g_test.capture_every = strtoll(list + 6, NULL, 10);
		return;
	}
	while (*list && g_test.capture_frame_count < TT_MAX_CAPTURE_FRAMES) {
		char* end = NULL;
		const long long value = strtoll(list, &end, 10);
		if (end == list)
			break;
		g_test.capture_frames[g_test.capture_frame_count++] = value;
		list = *end == ',' ? end + 1 : end;
	}
}

static void TieTest_MakeDirs(const char* path) {
	char buffer[512];
	SDL_strlcpy(buffer, path, sizeof buffer);
	for (char* p = buffer + 1; *p; ++p) {
		if (*p == '/') {
			*p = '\0';
			SDL_CreateDirectory(buffer);
			*p = '/';
		}
	}
	SDL_CreateDirectory(buffer);
}

bool TieTest_Init(void) {
	const char* script = SDL_getenv("TIE_TEST_SCRIPT");
	const char* capture = SDL_getenv("TIE_TEST_CAPTURE");
	const char* frames = SDL_getenv("TIE_TEST_CAPTURE_FRAMES");
	const char* exit_frame = SDL_getenv("TIE_TEST_EXIT_FRAME");
	const char* window = SDL_getenv("TIE_TEST_WINDOW");
	const char* xr = SDL_getenv("TIE_TEST_XR");
	const char* verbose = SDL_getenv("TIE_TEST_LOG");
	const char* mission = SDL_getenv("TIE_TEST_MISSION");
	g_test.active = (script && script[0]) || (capture && capture[0]) || (exit_frame && exit_frame[0]) ||
					(window && window[0]) || (xr && xr[0] == '1') || (mission && mission[0]);
	if (!g_test.active)
		return false;
	g_test.start_ticks_ms = SDL_GetTicks();
	g_test.verbose = verbose && verbose[0] == '1';
	if (window && SDL_sscanf(window, "%dx%d", &g_test.window_w, &g_test.window_h) != 2) {
		g_test.window_w = 0;
		g_test.window_h = 0;
	}
	if (exit_frame && exit_frame[0])
		g_test.exit_frame = strtoll(exit_frame, NULL, 10);
	if (capture && capture[0]) {
		SDL_strlcpy(g_test.capture_dir, capture, sizeof g_test.capture_dir);
		TieTest_MakeDirs(g_test.capture_dir);
		TieTest_ParseCaptureFrames(frames);
	}
	if (script && script[0] && !TieTest_LoadScript(script))
		g_test.exit_status = 2;
	if (xr && xr[0] == '1') {
		int eye_w = 512;
		int eye_h = 512;
		const char* eye = SDL_getenv("TIE_TEST_XR_EYE");
		if (eye)
			(void)SDL_sscanf(eye, "%dx%d", &eye_w, &eye_h);
		if (!TieTestWrap_Available()) {
			TieTest_Log("TIE_TEST_XR=1 needs the link-time wraps (Linux desktop build); ignored");
		} else {
			TieTestXr_Enable(eye_w, eye_h);
			float ypr[3] = { 0, 0, 0 };
			float pos[3] = { 0, 0, 0 };
			const char* head = SDL_getenv("TIE_TEST_XR_HEAD");
			if (head)
				(void)SDL_sscanf(head, "%f,%f,%f,%f,%f,%f", &ypr[0], &ypr[1], &ypr[2], &pos[0], &pos[1], &pos[2]);
			TieTestXr_SetHead(ypr, pos);
			TieTest_Log("fake XR headset on: eyes %dx%d, head yaw %.1f pitch %.1f roll %.1f", eye_w, eye_h,
						(double)ypr[0], (double)ypr[1], (double)ypr[2]);
		}
	}
	if (g_test.capture_dir[0] && !TieTestWrap_Available())
		TieTest_Log("window captures need the link-time wraps (Linux desktop build); only XR targets "
					"would be written");
	TieTest_Log("test harness on (script=%s capture=%s frames=%s exit=%lld window=%dx%d)",
				script ? script : "-", capture ? capture : "-", frames ? frames : "-",
				(long long)g_test.exit_frame, g_test.window_w, g_test.window_h);
	return true;
}

bool TieTest_Active(void) { return g_test.active; }

uint64_t TieTest_Frame(void) { return g_test.frame; }

int TieTest_ExitStatus(void) { return g_test.exit_status; }

void TieTest_ConfigureAeron(AeronConfig* config) {
	if (!g_test.active || !config)
		return;
	if (g_test.window_w >= 160 && g_test.window_h >= 120) {
		config->window_width = g_test.window_w;
		config->window_height = g_test.window_h;
	}
}

/* ---- TIE_TEST_MISSION: straight into a flight ---- */

void TieTest_AfterRuntimeInit(void) {
	const char* mission = SDL_getenv("TIE_TEST_MISSION");
	for (int i = 0; (!mission || !mission[0]) && i < g_test.line_count; ++i)
		if (g_test.lines[i].cmd == TT_CMD_MISSION)
			mission = g_test.lines[i].text;
	if (!g_test.active || !mission || !mission[0])
		return;
	ShellTask* shell = sHead_gbl ? (ShellTask*)(void*)sHead_gbl : NULL;
	if (!shell || !shell->started || shell->phase != SHELL_PHASE_DISPATCH) {
		TieTest_Log("FAIL TIE_TEST_MISSION: the shell is not waiting for its first scene");
		g_test.exit_status = 3;
		g_test.exit_requested = true;
		return;
	}
	/* What the registration desk does for a new pilot. */
	shipext_Set_Pilot_Name("TEST");
	shipext_Create_Pilot("TEST");
	shipext_Load_Pilot("TEST");
	int16_t scene;
	if (SDL_strncasecmp(mission, "train", 5) == 0 && (mission[5] == '\0' || mission[5] == ':')) {
		int ship = 0;
		int level = 0;
		if (mission[5] == ':')
			(void)SDL_sscanf(mission + 6, "%d:%d", &ship, &level);
		ship = ship < 0 ? 0 : (ship >= NUM_SHIPS ? NUM_SHIPS - 1 : ship);
		level = level < 0 ? 0 : (level > 15 ? 15 : level);
		pilot_record.cur_train_ship = (uint8_t)ship;
		pilot_record.train_level[ship] = (uint8_t)level;
		if (pilot_record.train_max_level[ship] < level)
			pilot_record.train_max_level[ship] = (uint8_t)level;
		scene = SCENE_FLIGHT_TRAIN;
		TieTest_Log("TIE_TEST_MISSION: training course, ship %d level %d", ship, level + 1);
	} else {
		char name[64];
		SDL_strlcpy(name, mission, sizeof name);
		char* dot = strrchr(name, '.');
		if (dot && SDL_strcasecmp(dot, ".tie") == 0)
			*dot = '\0';
		shipext_Set_Mission_Name(name);
		scene = SCENE_FLIGHT_COMBAT;
		TieTest_Log("TIE_TEST_MISSION: combat-chamber flight of mission/%s.tie", name);
	}
	shipext_Update_Pilot();
	shell->cur_scene = scene;
}

/* ---- input injection ---- */

static void TieTest_KeyEvent(AeronInputSnapshot* input, int key, uint8_t mods, bool down) {
	if (key <= 0 || key >= AERON_KEY_COUNT)
		return;
	if (input->key_event_count < AERON_KEY_EVENT_CAPACITY) {
		input->key_events[input->key_event_count++] = (AeronKeyEvent) {
			.chord = { (uint16_t)key, mods },
			.down = down ? 1 : 0,
			.repeat = 0,
		};
	} else {
		input->key_events_overflow = 1;
	}
	if (down) {
		if (!g_test.key_down[key])
			input->key_pressed[key] = 1;
		if (input->key_typed[key] < 255)
			input->key_typed[key]++;
		if ((mods & AERON_KEY_MOD_ALT) && input->key_alt_typed[key] < 255)
			input->key_alt_typed[key]++;
		g_test.key_down[key] = 1;
	} else {
		if (g_test.key_down[key])
			input->key_released[key] = 1;
		g_test.key_down[key] = 0;
		input->key_down[key] = 0;
	}
}

static uint8_t TieTest_HeldModifiers(void) {
	uint8_t mods = 0;
	if (g_test.key_down[AERON_KEY_LSHIFT] || g_test.key_down[AERON_KEY_RSHIFT])
		mods |= AERON_KEY_MOD_SHIFT;
	if (g_test.key_down[AERON_KEY_LCTRL] || g_test.key_down[AERON_KEY_RCTRL])
		mods |= AERON_KEY_MOD_CTRL;
	if (g_test.key_down[AERON_KEY_LALT] || g_test.key_down[AERON_KEY_RALT])
		mods |= AERON_KEY_MOD_ALT;
	if (g_test.key_down[AERON_KEY_LGUI] || g_test.key_down[AERON_KEY_RGUI])
		mods |= AERON_KEY_MOD_GUI;
	return mods;
}

static void TieTest_AppendText(AeronInputSnapshot* input, char ch) {
	if (input->text_length + 1 < AERON_TEXT_INPUT_CAPACITY) {
		input->text[input->text_length++] = ch;
		input->text[input->text_length] = '\0';
	}
}

static void TieTest_KeyPress(AeronInputSnapshot* input, int key, uint8_t mods) {
	for (uint8_t bit = 1; bit <= AERON_KEY_MOD_GUI; bit <<= 1)
		if (mods & bit)
			TieTest_KeyEvent(input, TieTest_ModifierKey(bit), TieTest_HeldModifiers() | bit, true);
	const uint8_t held = TieTest_HeldModifiers() | mods;
	TieTest_KeyEvent(input, key, held, true);
	/* What SDL's text input would deliver for this key (US layout). */
	if (!(held & (AERON_KEY_MOD_CTRL | AERON_KEY_MOD_ALT | AERON_KEY_MOD_GUI))) {
		const SDL_Keycode code = SDL_GetKeyFromScancode((SDL_Scancode)key, SDL_KMOD_NONE, false);
		if (code >= 0x20 && code < 0x7f) {
			char ch = (char)code;
			if ((held & AERON_KEY_MOD_SHIFT) && ch >= 'a' && ch <= 'z')
				ch = (char)(ch - 'a' + 'A');
			TieTest_AppendText(input, ch);
		}
	}
}

static void TieTest_KeyRelease(AeronInputSnapshot* input, int key, uint8_t mods) {
	TieTest_KeyEvent(input, key, TieTest_HeldModifiers(), false);
	for (uint8_t bit = AERON_KEY_MOD_GUI; bit >= 1; bit >>= 1)
		if (mods & bit)
			TieTest_KeyEvent(input, TieTest_ModifierKey(bit), TieTest_HeldModifiers(), false);
}

static void TieTest_AddPending(int64_t frame, int kind, int code, uint8_t mods) {
	if (g_test.pending_count >= TT_MAX_PENDING) {
		TieTest_Log("too many held inputs; release dropped");
		return;
	}
	g_test.pending[g_test.pending_count++] = (TieTestPendingRelease) { frame, kind, code, mods };
}

static void TieTest_ServicePending(AeronInputSnapshot* input) {
	for (int i = 0; i < g_test.pending_count;) {
		const TieTestPendingRelease p = g_test.pending[i];
		if ((int64_t)g_test.frame < p.frame) {
			++i;
			continue;
		}
		if (p.kind == 0)
			TieTest_KeyRelease(input, p.code, p.mods);
		else if (p.kind == 1) {
			g_test.mouse_buttons &= ~(uint32_t)p.code;
			g_test.mouse_released |= (uint32_t)p.code;
		} else
			g_test.pad_buttons &= ~(1u << p.code);
		g_test.pending[i] = g_test.pending[--g_test.pending_count];
	}
}

static bool TieTest_WaitDone(const TieTestLine* line) {
	const TieSnapshot* snapshot = TieSnapshot_Current();
	switch (line->ivalue[0]) {
		case TT_WAIT_SCENE:
			return snapshot && (int)snapshot->scene_kind == line->ivalue[1];
		case TT_WAIT_SHELL:
			return TieTest_ShellScene() == line->ivalue[1];
		case TT_WAIT_FLYING:
			if (!snapshot || snapshot->scene_kind != TIE_SCENE_FLIGHT || snapshot->flight_screen != 0 ||
				snapshot->replay_mode == 2) {
				g_test.wait_flight_frame = 0;
				return false;
			}
			if (g_test.wait_flight_frame == 0) {
				g_test.wait_flight_frame = snapshot->flight_frame ? snapshot->flight_frame : 1;
				return false;
			}
			return snapshot->flight_frame - g_test.wait_flight_frame >= (uint32_t)line->ivalue[1];
		default:
			return true;
	}
}

static void TieTest_Execute(AeronInputSnapshot* input, const TieTestLine* line) {
	switch (line->cmd) {
		case TT_CMD_KEY:
			TieTest_KeyPress(input, line->key, line->mods);
			TieTest_AddPending((int64_t)g_test.frame + line->hold, 0, line->key, line->mods);
			break;
		case TT_CMD_KEYDOWN:
			TieTest_KeyPress(input, line->key, line->mods);
			break;
		case TT_CMD_KEYUP:
			TieTest_KeyRelease(input, line->key, line->mods);
			break;
		case TT_CMD_TEXT: {
			const size_t n = strlen(line->text);
			if (g_test.text_pos >= g_test.text_len)
				g_test.text_len = g_test.text_pos = 0;
			for (size_t i = 0; i < n && g_test.text_len < (int)sizeof g_test.text_queue; ++i)
				g_test.text_queue[g_test.text_len++] = line->text[i];
			break;
		}
		case TT_CMD_MOUSE: {
			g_test.mouse_used = true;
			g_test.mouse_x = line->ivalue[0];
			g_test.mouse_y = line->ivalue[1];
			const uint32_t left = AERON_MOUSE_BUTTON_LEFT;
			const uint32_t right = AERON_MOUSE_BUTTON_RIGHT;
			switch (line->ivalue[2]) {
				case TT_MOUSE_CLICK:
				case TT_MOUSE_RCLICK: {
					const uint32_t b = line->ivalue[2] == TT_MOUSE_CLICK ? left : right;
					g_test.mouse_buttons |= b;
					g_test.mouse_pressed |= b;
					TieTest_AddPending((int64_t)g_test.frame + 3, 1, (int)b, 0);
					break;
				}
				case TT_MOUSE_DOWN:
				case TT_MOUSE_RDOWN: {
					const uint32_t b = line->ivalue[2] == TT_MOUSE_DOWN ? left : right;
					g_test.mouse_buttons |= b;
					g_test.mouse_pressed |= b;
					break;
				}
				case TT_MOUSE_UP:
				case TT_MOUSE_RUP: {
					const uint32_t b = line->ivalue[2] == TT_MOUSE_UP ? left : right;
					g_test.mouse_buttons &= ~b;
					g_test.mouse_released |= b;
					break;
				}
				default:
					break;
			}
			break;
		}
		case TT_CMD_AXIS: {
			g_test.pad_used = true;
			float v = line->fvalue[0];
			v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
			const bool trigger =
				line->ivalue[0] == AERON_GAMEPAD_AXIS_LEFT_TRIGGER || line->ivalue[0] == AERON_GAMEPAD_AXIS_RIGHT_TRIGGER;
			if (trigger && v < 0.0f)
				v = 0.0f;
			g_test.pad_axes[line->ivalue[0]] = (int16_t)(v * 32767.0f);
			break;
		}
		case TT_CMD_BUTTON:
			g_test.pad_used = true;
			g_test.pad_buttons |= 1u << line->ivalue[0];
			TieTest_AddPending((int64_t)g_test.frame + line->hold, 2, line->ivalue[0], 0);
			break;
		case TT_CMD_BUTTONDOWN:
			g_test.pad_used = true;
			g_test.pad_buttons |= 1u << line->ivalue[0];
			break;
		case TT_CMD_BUTTONUP:
			g_test.pad_buttons &= ~(1u << line->ivalue[0]);
			break;
		case TT_CMD_HEAD: {
			const float ypr[3] = { line->fvalue[0], line->fvalue[1], line->fcount > 2 ? line->fvalue[2] : 0.0f };
			const float pos[3] = { line->fcount > 3 ? line->fvalue[3] : 0.0f, line->fcount > 4 ? line->fvalue[4] : 0.0f,
								   line->fcount > 5 ? line->fvalue[5] : 0.0f };
			if (TieTestXr_Enabled())
				TieTestXr_SetHead(ypr, pos);
			else
				TieTest_Log("script line %d: head needs TIE_TEST_XR=1", line->line_no);
			break;
		}
		case TT_CMD_CAPTURE:
			g_test.capture_now = true;
			SDL_strlcpy(g_test.capture_label, line->text, sizeof g_test.capture_label);
			break;
		case TT_CMD_LOG:
			TieTest_Log("log: %s", line->text);
			break;
		case TT_CMD_XRBUTTON:
		case TT_CMD_XRMENU:
		case TT_CMD_XRTHROTTLE:
		case TT_CMD_XRHAND:
		case TT_CMD_XRPOSE:
			if (!TieTestXr_Enabled()) {
				TieTest_Log("script line %d: XR controller input needs TIE_TEST_XR=1", line->line_no);
				break;
			}
			if (line->cmd == TT_CMD_XRBUTTON)
				TieTestXr_QueueButton(line->ivalue[0]);
			else if (line->cmd == TT_CMD_XRMENU)
				TieTestXr_MenuPress(line->ivalue[0]);
			else if (line->cmd == TT_CMD_XRTHROTTLE)
				TieTestXr_SetThrottle(line->fvalue[0]);
			else if (line->cmd == TT_CMD_XRPOSE)
				TieTestXr_SetHandPose(line->ivalue[0], line->fvalue);
			else
				TieTestXr_SetHandInput(line->ivalue[0], line->fvalue[0], line->fvalue[1]);
			break;
		case TT_CMD_XRPAD:
			g_test.pad_used = true;
			g_test.pad_is_xr = true;
			break;
		case TT_CMD_QUIT:
			TieTest_Log("quit (script line %d)", line->line_no);
			g_test.exit_requested = true;
			break;
		default:
			break;
	}
}

static void TieTest_RunScript(AeronInputSnapshot* input) {
	while (g_test.next_line < g_test.line_count && !g_test.exit_requested) {
		const TieTestLine* line = &g_test.lines[g_test.next_line];
		const int64_t target = line->relative ? g_test.base_frame + line->frame : line->frame;
		if ((int64_t)g_test.frame < target)
			return;
		if (line->cmd == TT_CMD_WAIT) {
			if (!g_test.wait_logged) {
				g_test.wait_logged = true;
				g_test.wait_started = (int64_t)g_test.frame;
				g_test.wait_flight_frame = 0;
				if (g_test.verbose)
					TieTest_Log("waiting (script line %d)", line->line_no);
			}
			if (!TieTest_WaitDone(line)) {
				if ((int64_t)g_test.frame - g_test.wait_started > line->timeout) {
					TieTest_Log("FAIL wait timed out after %lld frames (script line %d)",
								(long long)line->timeout, line->line_no);
					g_test.exit_status = 3;
					g_test.exit_requested = true;
				}
				return;
			}
			TieTest_Log("wait done (script line %d, %lld frames)", line->line_no,
						(long long)((int64_t)g_test.frame - g_test.wait_started));
			g_test.wait_logged = false;
			g_test.base_frame = (int64_t)g_test.frame;
			g_test.next_line++;
			continue;
		}
		if (g_test.verbose)
			TieTest_Log("script line %d", line->line_no);
		TieTest_Execute(input, line);
		g_test.base_frame = target;
		g_test.next_line++;
	}
}

static void TieTest_ApplyGamepad(AeronInputSnapshot* input) {
	AeronControllerSnapshot* pad = &input->controllers[AERON_CONTROLLER_MAX - 1];
	if (!pad->connected || pad->instance_id != 0x7e57u) {
		memset(pad, 0, sizeof *pad);
		pad->connected = 1;
		pad->kind = AERON_CONTROLLER_KIND_GAMEPAD;
		pad->instance_id = 0x7e57u;
		/* xrpad: the identity of the engine's virtual Touch gamepad
		 * (aeron/src/xr_input.c), so the game treats it as the XR pad. */
		SDL_strlcpy(pad->name, g_test.pad_is_xr ? "OpenXR Touch" : "TIE test gamepad", sizeof pad->name);
		SDL_strlcpy(pad->path, g_test.pad_is_xr ? "openxr:touch" : "tie-test", sizeof pad->path);
		SDL_strlcpy(pad->guid,
					g_test.pad_is_xr ? "4f70656e5852546f7563680000000001" : "03000000544945546573747061640000",
					sizeof pad->guid);
		pad->axis_count = AERON_GAMEPAD_AXIS_COUNT;
		pad->button_count = AERON_GAMEPAD_BUTTON_COUNT;
		pad->gamepad_available_axes = (1u << AERON_GAMEPAD_AXIS_COUNT) - 1u;
		pad->gamepad_available_buttons = (uint32_t)((1ull << AERON_GAMEPAD_BUTTON_COUNT) - 1ull);
	}
	int16_t axes[AERON_GAMEPAD_AXIS_COUNT];
	SDL_memcpy(axes, g_test.pad_axes, sizeof axes);
	/* The game's flight stick drives the XR pad's right-stick axes (LEFTX/
	 * LEFTY, forward = -Y) and roll (RIGHTX), whichever is pushed further,
	 * as Aeron's virtual Touch pad does. */
	float vstick[3];
	if (g_test.pad_is_xr && TieTestXr_VirtualStick(vstick)) {
		const int16_t v[3] = { (int16_t)(vstick[0] * 32767.0f), (int16_t)(-vstick[1] * 32767.0f),
							   (int16_t)(vstick[2] * 32767.0f) };
		const int slot[3] = { AERON_GAMEPAD_AXIS_LEFTX, AERON_GAMEPAD_AXIS_LEFTY, AERON_GAMEPAD_AXIS_RIGHTX };
		for (int k = 0; k < 3; ++k)
			if (abs(v[k]) > abs(axes[slot[k]]))
				axes[slot[k]] = v[k];
	}
	for (int i = 0; i < AERON_GAMEPAD_AXIS_COUNT; ++i) {
		pad->gamepad_axes[i] = axes[i];
		if (i < AERON_CONTROLLER_AXIS_MAX)
			pad->raw_axes[i] = axes[i];
	}
	pad->gamepad_buttons = g_test.pad_buttons;
	pad->gamepad_pressed_buttons = g_test.pad_buttons & ~g_test.pad_prev_buttons;
	pad->gamepad_released_buttons = g_test.pad_prev_buttons & ~g_test.pad_buttons;
	pad->raw_buttons = g_test.pad_buttons;
	pad->raw_pressed_buttons = pad->gamepad_pressed_buttons;
	pad->raw_released_buttons = pad->gamepad_released_buttons;
	g_test.pad_prev_buttons = g_test.pad_buttons;
}

static void TieTest_TrackScene(void) {
	const TieSnapshot* snapshot = TieSnapshot_Current();
	const int kind = snapshot ? (int)snapshot->scene_kind : -1;
	const int shell = TieTest_ShellScene();
	const int screen = snapshot && kind == TIE_SCENE_FLIGHT ? (int)snapshot->flight_screen : -1;
	if (kind != g_test.last_scene_kind || shell != g_test.last_shell_scene || screen != g_test.last_flight_screen) {
		TieTest_Log("scene=%s shell=%d flight_screen=%d", TieTest_SceneName(kind), shell, screen);
		g_test.last_scene_kind = kind;
		g_test.last_shell_scene = shell;
		g_test.last_flight_screen = screen;
	}
	if (g_test.frame % 600 == 0)
		TieTest_Log("alive: %.1f s, scene=%s shell=%d flight_frame=%u xr_stereo_frames=%llu",
					(double)(SDL_GetTicks() - g_test.start_ticks_ms) / 1000.0, TieTest_SceneName(kind), shell,
					snapshot ? (unsigned)snapshot->flight_frame : 0u,
					(unsigned long long)TieTestXr_CommittedFrames());
}

void TieTest_ApplyInput(AeronInputSnapshot* input) {
	if (!g_test.active || !input)
		return;
	TieTest_TrackScene();
	/* The offscreen test window never gets OS focus. */
	input->has_focus = 1;
	g_test.mouse_pressed = 0;
	g_test.mouse_released = 0;
	TieTest_ServicePending(input);
	TieTest_RunScript(input);
	/* One typed character every other frame (DOS-style key queue). */
	if (g_test.text_pos < g_test.text_len && (g_test.frame & 1u) == 0)
		TieTest_AppendText(input, g_test.text_queue[g_test.text_pos++]);
	/* Keys held across frames stay down in the snapshot. */
	for (int key = 0; key < AERON_KEY_COUNT; ++key)
		if (g_test.key_down[key])
			input->key_down[key] = 1;
	if (g_test.mouse_used) {
		input->mouse.x = g_test.mouse_x;
		input->mouse.y = g_test.mouse_y;
		input->mouse.inside_content = 1;
		input->mouse.buttons = g_test.mouse_buttons;
		input->mouse.pressed_buttons |= g_test.mouse_pressed;
		input->mouse.released_buttons |= g_test.mouse_released;
	}
	if (g_test.pad_used)
		TieTest_ApplyGamepad(input);
}

/* ---- capture / frame end ---- */

static bool TieTest_CaptureDue(void) {
	const int64_t frame = (int64_t)g_test.frame;
	if (g_test.capture_now)
		return true;
	if (g_test.capture_every > 0 && frame % g_test.capture_every == 0)
		return true;
	for (int i = 0; i < g_test.capture_frame_count; ++i)
		if (g_test.capture_frames[i] == frame)
			return true;
	return false;
}

void TieTest_BeforePresent(void) {
	if (!g_test.active)
		return;
	g_test.capture_armed = false;
	if (!g_test.capture_dir[0] || !TieTest_CaptureDue())
		return;
	g_test.capture_armed = true;
	if (g_test.capture_label[0] && g_test.capture_now)
		SDL_snprintf(g_test.capture_prefix, sizeof g_test.capture_prefix, "%s/f%06llu_%s", g_test.capture_dir,
					 (unsigned long long)g_test.frame, g_test.capture_label);
	else
		SDL_snprintf(g_test.capture_prefix, sizeof g_test.capture_prefix, "%s/f%06llu", g_test.capture_dir,
					 (unsigned long long)g_test.frame);
	if (TieTestWrap_Available()) {
		char path[700];
		SDL_snprintf(path, sizeof path, "%s_window.png", g_test.capture_prefix);
		TieTestWrap_RequestWindowCapture(path);
	}
}

bool TieTest_AfterPresent(void) {
	if (!g_test.active)
		return false;
	if (TieTestXr_Enabled()) {
		const int written = TieTestXr_EndFrame(g_test.capture_armed ? g_test.capture_prefix : NULL);
		if (g_test.capture_armed && written)
			TieTest_Log("captured %d XR images: %s_*", written, g_test.capture_prefix);
	}
	if (g_test.capture_armed) {
		const TieSnapshot* snapshot = TieSnapshot_Current();
		if (snapshot && snapshot->scene_kind == TIE_SCENE_FLIGHT)
			TieTest_Log("capture %s_window.png (flight_frame %u, clock %.6s)", g_test.capture_prefix,
						(unsigned)snapshot->flight_frame, snapshot->hud.mission_clock_text);
		else
			TieTest_Log("capture %s_window.png", g_test.capture_prefix);
	}
	g_test.capture_armed = false;
	g_test.capture_now = false;
	g_test.capture_label[0] = '\0';
	++g_test.frame;
	if (Aeron_QuitRequested() && !g_test.exit_requested) {
		/* SIGTERM/SIGINT (SDL turns them into a quit event) or a window close. */
		TieTest_Log("FAIL quit requested from outside (signal or window close)");
		if (g_test.exit_status == 0)
			g_test.exit_status = 4;
		g_test.exit_requested = true;
	}
	if (g_test.exit_frame >= 0 && (int64_t)g_test.frame > g_test.exit_frame && !g_test.exit_requested) {
		TieTest_Log("TIE_TEST_EXIT_FRAME reached");
		g_test.exit_requested = true;
	}
	if (g_test.exit_requested) {
		TieTest_Log("exit (status %d)", g_test.exit_status);
		return true;
	}
	return false;
}

void TieTest_Shutdown(void) {
	if (!g_test.active)
		return;
	if (g_test.next_line < g_test.line_count && g_test.exit_status == 0)
		TieTest_Log("note: %d script commands never ran", g_test.line_count - g_test.next_line);
	TieTest_Log("done status=%d", g_test.exit_status);
	free(g_test.lines);
	g_test.lines = NULL;
	g_test.line_count = 0;
}

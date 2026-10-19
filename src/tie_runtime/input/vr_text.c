#include "tie_runtime/input/vr_text.h"

#include "aeron/aeron.h"
#include "tie_runtime/input/vr_buttons.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Key codes of the parser: ASCII (letters lower case) for plain keys. */
enum {
	VRT_KEY_NONE = 0,
	VRT_KEY_ENTER = 13,
	VRT_KEY_ESCAPE = 27,
	VRT_KEY_SPACE = ' ',
	VRT_KEY_BACKSPACE = 8,
	VRT_KEY_TAB = 9,
	VRT_KEY_F = 0x100,       /* + 1..12 */
	VRT_KEY_SHIFT = 0x200,   /* + lower-case letter */
	VRT_KEY_ALT = 0x300,     /* + lower-case letter */
	VRT_KEY_SHIFT_F = 0x400, /* + 1..12 */
};

bool TieVrText_Active(void) {
	const char* env = getenv("TIE_VR_TEXT");
	if (env && env[0] == '0')
		return false;
	if (env && env[0] == '1')
		return true;
	return Aeron_XrIsPresenting() != 0;
}

/* TIE's default keys (actions.c) whose action has a VR control. */
static TieInputAction TieVrText_KeyAction(int key) {
	switch (key) {
		case VRT_KEY_SPACE:
			return TIE_INPUT_ACTION_CONFIRM;
		case VRT_KEY_ENTER:
			return TIE_INPUT_ACTION_MATCH_SPEED;
		case 't':
			return TIE_INPUT_ACTION_TARGET_NEXT;
		case 'y':
			return TIE_INPUT_ACTION_TARGET_PREV;
		case 'r':
			return TIE_INPUT_ACTION_TARGET_NEAREST_FIGHTER_OR_MINE;
		case 'e':
			return TIE_INPUT_ACTION_TARGET_MY_ATTACKER;
		case 'u':
			return TIE_INPUT_ACTION_TARGET_NEWEST_CRAFT;
		case 'w':
			return TIE_INPUT_ACTION_CYCLE_WEAPON_GROUP;
		case 'x':
			return TIE_INPUT_ACTION_CYCLE_WEAPON_FIRING_MODE;
		case 'b':
			return TIE_INPUT_ACTION_TOGGLE_BEAM;
		case 's':
			return TIE_INPUT_ACTION_CYCLE_SHIELD_MODE;
		case 'q':
			return TIE_INPUT_ACTION_END_MISSION;
		case 'h':
			return TIE_INPUT_ACTION_HYPERSPACE;
		case '\'':
			return TIE_INPUT_ACTION_XFER_CANNON_TO_SHIELDS;
		case ';':
			return TIE_INPUT_ACTION_XFER_SHIELDS_TO_CANNON;
		case VRT_KEY_F + 9:
			return TIE_INPUT_ACTION_CYCLE_CANNON_RECHARGE_RATE;
		case VRT_KEY_F + 10:
			return TIE_INPUT_ACTION_CYCLE_SHIELD_RECHARGE_RATE;
		default:
			return TIE_INPUT_ACTION_NONE;
	}
}

static const char* TieVrText_KeyLabel(int key, TieVrTextContext context, bool compact, bool left_handed) {
	if (context == TIE_VR_TEXT_MENU) {
		for (int b = 0; b < AERON_XR_BUTTON_COUNT; ++b)
			if (TieVrButtons_MenuKey((AeronXrButton)b) == key && key)
				return TieVrButtons_ButtonName((AeronXrButton)b, compact, left_handed);
		return NULL;
	}
	const TieInputAction action = TieVrText_KeyAction(key);
	return action == TIE_INPUT_ACTION_NONE ? NULL : TieVrButtons_ActionControl(action, compact, left_handed);
}

/* ---- parsing (after OpenXWA-VR's vr_buttons.c) ---- */

static bool TieVrText_StartsWith(const char* s, const char* prefix) {
	for (; *prefix; ++s, ++prefix)
		if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
			return false;
	return true;
}

static bool TieVrText_IsWord(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* A key token at s: a named key, F1-F12, Shift/Alt + a key, or one
 * character. Sets *key, returns its length (0 = none). `single_ok` allows
 * a lone lower-case letter or punctuation (only inside quotes). */
static int TieVrText_Token(const char* s, int* key, bool single_ok) {
	static const struct {
		const char* word;
		int key;
	} named[] = {
		{ "spacebar", VRT_KEY_SPACE }, { "space bar", VRT_KEY_SPACE }, { "space", VRT_KEY_SPACE },
		{ "enter", VRT_KEY_ENTER },    { "return", VRT_KEY_ENTER },    { "escape", VRT_KEY_ESCAPE },
		{ "esc", VRT_KEY_ESCAPE },     { "tab", VRT_KEY_TAB },         { "backspace", VRT_KEY_BACKSPACE },
	};
	for (size_t i = 0; i < sizeof named / sizeof named[0]; ++i) {
		const size_t n = strlen(named[i].word);
		if (TieVrText_StartsWith(s, named[i].word) && !TieVrText_IsWord(s[n])) {
			*key = named[i].key;
			return (int)n;
		}
	}
	for (int alt = 0; alt < 2; ++alt) {
		const char* mod = alt ? "alt" : "shift";
		const size_t n = strlen(mod);
		if (TieVrText_StartsWith(s, mod) && (s[n] == '-' || s[n] == '+')) {
			const char* t = s + n + 1;
			if ((t[0] == 'F' || t[0] == 'f') && isdigit((unsigned char)t[1])) {
				const bool two = isdigit((unsigned char)t[2]) != 0;
				const int f = two ? (t[1] - '0') * 10 + (t[2] - '0') : t[1] - '0';
				const int len = two ? 3 : 2;
				if (f >= 1 && f <= 12 && !TieVrText_IsWord(t[len])) {
					*key = (alt ? VRT_KEY_NONE : VRT_KEY_SHIFT_F + f);
					return *key ? (int)(n + 1) + len : 0;
				}
			}
			if (isalnum((unsigned char)t[0]) && !TieVrText_IsWord(t[1])) {
				*key = (alt ? VRT_KEY_ALT : VRT_KEY_SHIFT) + tolower((unsigned char)t[0]);
				return (int)(n + 2);
			}
			return 0;
		}
	}
	if ((s[0] == 'F' || s[0] == 'f') && isdigit((unsigned char)s[1])) {
		const bool two = isdigit((unsigned char)s[2]) != 0;
		const int f = two ? (s[1] - '0') * 10 + (s[2] - '0') : s[1] - '0';
		if (f >= 1 && f <= 12 && !TieVrText_IsWord(s[two ? 3 : 2])) {
			*key = VRT_KEY_F + f;
			return two ? 3 : 2;
		}
	}
	/* One letter or digit: a capital outside quotes ("a key" is prose). */
	if (isalnum((unsigned char)s[0]) && !TieVrText_IsWord(s[1]) &&
		(single_ok || isupper((unsigned char)s[0]) || isdigit((unsigned char)s[0]))) {
		*key = tolower((unsigned char)s[0]);
		return 1;
	}
	if (single_ok && s[0] > 0x20 && s[0] < 0x7f && s[0] != '\'' && s[0] != '"') {
		*key = (unsigned char)s[0];
		return 1;
	}
	return 0;
}

/* A key reference at s: "'T'", "['T']", "[Space Bar]", "'T' key", "Shift-D",
 * "space bar", "Press T", each optionally after "the ". Returns its length
 * and the key; *bracketed when it sits in TIE's highlight brackets (kept
 * around the label). `after_press`: s follows "press " / "hit ". */
static int TieVrText_Reference(const char* s, int* key, bool after_press, bool* bracketed) {
	int skip = 0;
	*bracketed = false;
	if (TieVrText_StartsWith(s, "the "))
		skip = 4;
	const char* t = s + skip;
	int len = 0;
	int open = 0;
	if (t[0] == '[') {
		open = 1;
		++t;
	}
	if (t[0] == '\'' || t[0] == '"') {
		const int n = TieVrText_Token(t + 1, key, true);
		if (n == 0 || t[1 + n] != t[0])
			return 0;
		len = n + 2;
	} else {
		const int n = TieVrText_Token(t, key, false);
		if (n == 0)
			return 0;
		const bool modifier = TieVrText_StartsWith(t, "shift") || TieVrText_StartsWith(t, "alt");
		/* "space bar" / "spacebar", or a named key in capitals (RETURN, ESC);
		 * not prose like "return to base" or "into space". */
		bool named = false;
		if (*key == VRT_KEY_SPACE && n > 5)
			named = true;
		else if (n > 1 && *key < VRT_KEY_F) {
			named = true;
			for (int c = 0; c < n; ++c)
				named = named && !islower((unsigned char)t[c]);
		}
		const bool keyword = TieVrText_StartsWith(t + n, " key") && !TieVrText_IsWord(t[n + 4]);
		/* Bare tokens need context: a modifier, a named key, "<x> key", or
		 * a capital / F-key right after "press". */
		const bool pressed = after_press && skip == 0 && (isupper((unsigned char)t[0]) || isdigit((unsigned char)t[0]));
		if (!modifier && !named && !keyword && !pressed)
			return 0;
		len = n;
	}
	if (open) {
		if (t[len] != ']')
			return 0;
		*bracketed = true;
		len += 1;
	}
	if (TieVrText_StartsWith(t + len, " key") && !TieVrText_IsWord(t[len + 4]))
		len += 4;
	return skip + open + len;
}

/* "press " / "hit " right before out[end] (skipping highlight brackets):
 * its start, or -1. */
static int TieVrText_VerbBefore(const char* out, size_t end, size_t* verb_len) {
	static const char* const verbs[] = { "press ", "hit " };
	while (end > 0 && out[end - 1] == '[')
		--end;
	for (size_t v = 0; v < sizeof verbs / sizeof verbs[0]; ++v) {
		const size_t vl = strlen(verbs[v]);
		if (end >= vl && TieVrText_StartsWith(out + end - vl, verbs[v]) &&
			(end == vl || !TieVrText_IsWord(out[end - vl - 1]))) {
			*verb_len = vl;
			return (int)(end - vl);
		}
	}
	return -1;
}

static bool TieVrText_Append(char* out, size_t size, size_t* o, const char* text) {
	const size_t n = strlen(text);
	if (*o + n >= size)
		return false;
	memcpy(out + *o, text, n);
	*o += n;
	out[*o] = 0;
	return true;
}

bool TieVrText_RewriteKeys(const char* in, char* out, size_t size, TieVrTextContext context, bool compact,
						   bool left_handed) {
	size_t o = 0;
	bool changed = false;
	if (!out || size == 0)
		return false;
	out[0] = 0;
	if (!in)
		return false;
	for (size_t i = 0; in[i] != '\0';) {
		/* TIE color codes (0xFE + a letter) separate words too. */
		const bool boundary = i == 0 || !TieVrText_IsWord(in[i - 1]) ||
							  (i >= 2 && (unsigned char)in[i - 2] == 0xFE);
		int key = VRT_KEY_NONE;
		bool bracketed = false;
		int n = 0;
		if (boundary) {
			size_t verb_len = 0;
			const bool after_press = TieVrText_VerbBefore(in, i, &verb_len) >= 0;
			n = TieVrText_Reference(in + i, &key, after_press, &bracketed);
		}
		const char* label = n > 0 ? TieVrText_KeyLabel(key, context, compact, left_handed) : NULL;
		if (label) {
			/* Triggers are pulled: "Hit [Space Bar]" -> "Pull [both triggers]". */
			size_t verb_len = 0;
			const int verb = context == TIE_VR_TEXT_FLIGHT && strstr(label, "trigger") &&
									 strncmp(label, "grip", 4) != 0
								 ? TieVrText_VerbBefore(out, o, &verb_len)
								 : -1;
			if (verb >= 0) {
				char tail[8];
				const size_t tail_len = o - (size_t)verb - verb_len; /* the '[' after the verb */
				snprintf(tail, sizeof tail, "%.*s", (int)tail_len, out + verb + verb_len);
				const bool capital = isupper((unsigned char)out[verb]) != 0;
				o = (size_t)verb;
				out[o] = 0;
				if (!TieVrText_Append(out, size, &o, capital ? "Pull " : "pull ") ||
					!TieVrText_Append(out, size, &o, tail))
					return false;
			}
			if ((bracketed && !TieVrText_Append(out, size, &o, "[")) || !TieVrText_Append(out, size, &o, label) ||
				(bracketed && !TieVrText_Append(out, size, &o, "]")))
				return false;
			i += (size_t)n;
			changed = true;
			continue;
		}
		if (o + 1 >= size)
			return false;
		out[o++] = in[i++];
		out[o] = 0;
	}
	return changed;
}

static bool TieVrText_LeftHanded(void) { return Aeron_XrLeftHanded() != 0; }

void TieVrText_RewriteMessage(char* body, size_t size) {
	char out[256];
	if (!body || size == 0 || size > sizeof out || !TieVrText_Active())
		return;
	const bool left = TieVrText_LeftHanded();
	if (TieVrText_RewriteKeys(body, out, size, TIE_VR_TEXT_FLIGHT, false, left) ||
		TieVrText_RewriteKeys(body, out, size, TIE_VR_TEXT_FLIGHT, true, left))
		memcpy(body, out, strlen(out) + 1);
}

const char* TieVrText_MenuHint(const char* text) {
	static char out[256];
	if (!text || !TieVrText_Active())
		return text;
	return TieVrText_RewriteKeys(text, out, sizeof out, TIE_VR_TEXT_MENU, false, TieVrText_LeftHanded()) ? out
																										  : text;
}

const char* TieVrText_PauseMessage(void) {
	static char text[96];
	if (!TieVrText_Active())
		return NULL;
	/* TIE's own pause: A or B resumes (vr_controls.c). */
	const bool left = TieVrText_LeftHanded();
	snprintf(text, sizeof text, "\006Mission paused. Press %s or %s to continue.",
			 TieVrButtons_ButtonName(AERON_XR_BUTTON_A, false, left),
			 TieVrButtons_ButtonName(AERON_XR_BUTTON_B, false, left));
	return text;
}

const char* TieVrText_MissileWarningMessage(void) {
	static char text[96];
	if (!TieVrText_Active())
		return NULL;
	/* Space confirms the prompt; in VR the trigger chord sends it. */
	snprintf(text, sizeof text, "\004Missile Warning! [Pull %s to target?",
			 TieVrButtons_ActionControl(TIE_INPUT_ACTION_CONFIRM, false, TieVrText_LeftHanded()));
	return text;
}

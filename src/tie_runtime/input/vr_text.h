/*
 * Game text that names keyboard keys, rewritten for the VR (Quest Touch)
 * controls while the headset presents: "Press ['T'] to select ..." ->
 * "Press [A] to select ...", "Hit [Space Bar] to confirm ..." -> "Pull
 * [both triggers] to confirm ...". TIE equivalent of OpenXWA-VR's
 * XwaVrButtons_RewriteText; the controls come from vr_buttons.c.
 *
 * Only keys whose action has a VR control are replaced; every other key
 * name ("Shift-A", "F2", 'G') stays as written. Game data is not touched:
 * callers rewrite at the point where the text is shown or queued.
 */
#ifndef TIE_RUNTIME_INPUT_VR_TEXT_H
#define TIE_RUNTIME_INPUT_VR_TEXT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rewriting is on: the headset presents, or TIE_VR_TEXT=1 (desktop tests;
 * TIE_VR_TEXT=0 turns it off). */
bool TieVrText_Active(void);

typedef enum TieVrTextContext {
	TIE_VR_TEXT_FLIGHT = 0, /* cockpit messages: keys -> flight controls */
	TIE_VR_TEXT_MENU,       /* in-flight rooms/menus: Space -> X, Enter -> A, Esc -> B */
} TieVrTextContext;

/* Rewrites key references in `in` into `out` (always NUL-terminated).
 * Returns true when something was replaced and the result fit. `compact`
 * uses the short control names ("L-trigger"). Pure (no Active check). */
bool TieVrText_RewriteKeys(const char* in, char* out, size_t size, TieVrTextContext context, bool compact,
						   bool left_handed);

/* An in-flight message body (msg_messageprintf, after expansion): rewritten
 * in place when Active, with the full control names, else the compact
 * ones; left alone when neither fits in `size`. */
void TieVrText_RewriteMessage(char* body, size_t size);

/* Menu/room text when Active: a rewritten copy (static storage, valid
 * until the next call), else `text` itself. */
const char* TieVrText_MenuHint(const char* text);

/* Whole-message replacements when Active (NULL otherwise). The game's own
 * pause ('p'): "Mission paused. Press A or B to continue." */
const char* TieVrText_PauseMessage(void);
/* "Missile Warning! Both triggers to target?" with the TIE color codes. */
const char* TieVrText_MissileWarningMessage(void);

#ifdef __cplusplus
}
#endif

#endif

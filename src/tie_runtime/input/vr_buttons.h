/*
 * The VR (Quest Touch) controls of TIE Fighter: the one place to change a
 * binding. Mirrors OpenXWA-VR's xwa_runtime/runtime/vr_buttons.c where TIE
 * has the same action, so both games feel alike.
 *
 * Steering, roll, the throttle lever and fire stay on Aeron's virtual
 * gamepad (aeron/src/xr_input_map.h; the TIE side is the built-in XR
 * profile in controller_mapping.c). Every other control in flight is an
 * Aeron "flight button" (Aeron_XrSetFlightButtons / Aeron_XrPollButton)
 * looked up in the table below; holding the left grip selects the shifted
 * layer (Aeron itself reports no shift layer, so TIE reads the grip).
 *
 * The table is data on purpose: controller labels, the controls card and
 * voice commands read it through TieVrButtons_Table / TieVrButtons_Label.
 */
#ifndef TIE_RUNTIME_INPUT_VR_BUTTONS_H
#define TIE_RUNTIME_INPUT_VR_BUTTONS_H

#include <stdbool.h>
#include <stddef.h>

#include "aeron/aeron.h"
#include "tie_runtime/input/actions.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One flight button binding: `button` (+ the left grip when `shifted`)
 * sends `action`. */
typedef struct TieVrButtonBinding {
	AeronXrButton button;
	bool shifted;
	TieInputAction action;
	const char* label; /* short text for controller labels ("next target") */
} TieVrButtonBinding;

/* All flight bindings, plain layer first. */
const TieVrButtonBinding* TieVrButtons_Table(size_t* count);
/* The action of `button` on its plain or shifted layer; NONE when unbound. */
TieInputAction TieVrButtons_Action(AeronXrButton button, bool shifted);

/* Controls that are not flight buttons, for labels and hints. */
typedef enum TieVrControl {
	TIE_VR_CONTROL_RIGHT_STICK = 0, /* yaw / pitch */
	TIE_VR_CONTROL_LEFT_STICK_X,    /* roll */
	TIE_VR_CONTROL_LEFT_STICK_Y,    /* throttle lever */
	TIE_VR_CONTROL_RIGHT_TRIGGER,   /* fire */
	TIE_VR_CONTROL_LEFT_GRIP,       /* shift layer */
	TIE_VR_CONTROL_MENU,            /* settings (Esc) */
	TIE_VR_CONTROL_CHORD,           /* both triggers: confirm (Space) */
	TIE_VR_CONTROL_SHIFT_CHORD,     /* grip + both triggers: hyperspace */
	TIE_VR_CONTROL_COUNT
} TieVrControl;
const char* TieVrButtons_ControlName(TieVrControl control, bool compact);
const char* TieVrButtons_ControlLabel(TieVrControl control);

/* Both triggers pulled within this window and past this travel: the chord. */
#define TIE_VR_CHORD_PULL 0.75f
#define TIE_VR_CHORD_RELEASE 0.3f
#define TIE_VR_CHORD_WINDOW_MS 350u
/* Holding the left grip past this selects the shifted layer. */
#define TIE_VR_SHIFT_GRIP 0.5f

/* Menus (frontend, briefings, films, in-flight rooms): the pointer clicks;
 * these flight buttons send keys there. B is always Escape (one screen
 * back). 0 = nothing. */
int TieVrButtons_MenuKey(AeronXrButton button);

/* A button's name: "B", "left trigger", "right stick click". `compact`
 * gives the short form ("L-trigger"). With left-handed controls it names
 * the physical control that now sends it (B is then "Y"). */
const char* TieVrButtons_ButtonName(AeronXrButton button, bool compact, bool left_handed);

/* The control that sends `action` in flight, e.g. "B" or "grip + A"
 * ("grip+A" when compact); NULL when no VR control does. Static storage,
 * valid until the next call. */
const char* TieVrButtons_ActionControl(TieInputAction action, bool compact, bool left_handed);

#ifdef __cplusplus
}
#endif

#endif

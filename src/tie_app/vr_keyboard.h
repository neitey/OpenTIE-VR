#ifndef TIE_APP_VR_KEYBOARD_H
#define TIE_APP_VR_KEYBOARD_H

/*
 * On-screen keyboard for game text fields (pilot name on the registration
 * screen, film name when saving a replay) when there is no keyboard: in the
 * headset (Aeron_XrIsPresenting) or with TIE_VR_KEYBOARD=1 (desktop tests;
 * TIE_VR_KEYBOARD=0 turns it off). Drawn as a UI overlay on the virtual
 * screen; the laser pointer (right controller = mouse, trigger = click)
 * presses keys, which go into TIE's key queue like typed keys. Port of
 * OpenXWA-VR's xwa_app/vr_keyboard.c.
 *
 * A field is live while the game calls TieInput_MarkTextEntry (its draw or
 * edit step, every frame); the keyboard lingers a few frames after that.
 */
#include <stdbool.h>
#include <stdint.h>

#include "tie_app/ui.h"

/* The keyboard draws with the application UI font (not owned). */
void TieVrKeyboard_Init(TieUi* ui);
/* Before TieInput_BeginFrame: draws the keys, types what was pressed and
 * hides clicks on the keyboard from the game. `hidden`: settings menu or
 * pause owns the screen. */
void TieVrKeyboard_BeginFrame(const AeronInputSnapshot* input, int32_t delta_us, bool hidden);
/* After the game's layers, before the settings menu and Aeron_Present. */
void TieVrKeyboard_Submit(void);
void TieVrKeyboard_Shutdown(void);
/* The keyboard is on screen this frame. */
bool TieVrKeyboard_Visible(void);

#endif

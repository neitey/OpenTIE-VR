#ifndef TIE_RUNTIME_INPUT_VR_POINTER_H
#define TIE_RUNTIME_INPUT_VR_POINTER_H

/*
 * VR laser pointer steadying for the host's own UI (the settings overlay).
 *
 * The engine turns the right controller's aim ray into the mouse (trigger =
 * left button) without any filtering, so hand tremor makes the hover focus
 * flicker between rows and the jerk of pulling the trigger moves the pointer
 * off the row between press and release (AeronUi activates on a release over
 * the pressed widget). TieVrPointer_Steady filters a copy of the frame's
 * input while the headset presents:
 *   - speed-adaptive smoothing (strong when the hand is nearly still, almost
 *     none on a quick sweep) and a small dead band, so a resting pointer
 *     stays on its row;
 *   - click lock: from the press the pointer stays where it was until the
 *     ray moves well away (then it drags, e.g. a slider or scrollbar), so
 *     press and release land on the same row.
 * TieVrPointer_Offset gives where the steadied pointer sits relative to the
 * raw one, for the laser beam to end on it.
 */

#include <stdbool.h>

#include "aeron/input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Filters in->mouse (logical coordinates) in place; `dt_seconds` the frame
 * time. Does nothing (and resets) unless the headset presents. */
void TieVrPointer_Steady(AeronInputSnapshot* in, float dt_seconds);

/* The steadied pointer minus the raw one, in logical pixels, from this
 * frame's TieVrPointer_Steady; false when the pointer is not being steadied
 * (no menu using it this frame). */
bool TieVrPointer_Offset(float* dx, float* dy);

/* No steadied menu this frame (call when the menu closes). */
void TieVrPointer_Reset(void);

#ifdef __cplusplus
}
#endif

#endif

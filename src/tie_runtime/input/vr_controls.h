/*
 * VR (OpenXR Touch controller) controls, per host frame.
 *
 * Picks the controller mode for the frame (OpenXWA-VR: runtime/port.c
 * XwaPort_UpdateVrModes):
 *   - flight (mission running, normal view, not paused, no settings):
 *     menu pointer off, flight buttons on and dispatched through the table
 *     in vr_buttons.c, both-trigger chord, throttle lever kept in step with
 *     the craft's throttle, haptics on firing and on hits;
 *   - everywhere else: the right controller is the mouse pointer on the
 *     virtual screen (trigger = click), A = Enter, X = Space, B = Escape
 *     (one screen back), and B closes the settings menu.
 * Steering, roll, the absolute throttle lever and fire come from Aeron's
 * virtual gamepad through controller_mapping.c's built-in XR profile.
 *
 * No-op while no XR session is presenting.
 */
#ifndef TIE_RUNTIME_INPUT_VR_CONTROLS_H
#define TIE_RUNTIME_INPUT_VR_CONTROLS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	TIE_VR_REQUEST_CLOSE_SETTINGS = 1u, /* B pressed while the settings menu is open */
};

/* Call once per host frame after Aeron_BeginFrame and the settings hotkeys,
 * before TieInput_BeginFrame. Returns TIE_VR_REQUEST_* bits. */
uint32_t TieVrControls_Frame(bool settings_open, bool host_paused);

/* Nonzero while the frame's mode is flight (see above). */
bool TieVrControls_InFlight(void);

/* The throttle lever position (0..1) that makes the virtual gamepad report
 * `throttle` (TieControllerMapping_ThrottlePosition's 0..65535) exactly. */
float TieVrControls_LeverForThrottle(uint16_t throttle);

#ifdef __cplusplus
}
#endif

#endif

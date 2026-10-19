/*
 * VR lap console, quick wheel and side panel (TIE port of OpenXWA-VR's
 * xwa_remaster/vr_lap_console.c).
 *
 * Lap console (XWA_XR_CONSOLE, on unless 0): a holographic touch panel over
 * the player's lap with the commands that have no controller button:
 * energy (recharge rates, shield direction, transfers, beam), targeting
 * (incl. components and the F5..F7 target memories), wingmen orders and
 * ship (weapons, throttle steps, hyperspace, end mission, map / goals /
 * damage rooms). It sleeps (dim) while the player looks ahead and wakes when
 * they look down. Buttons are poked with the controller tip, or pointed at
 * and the trigger pulled (the trigger does not fire while it points at a
 * panel). Squeezing a grip at either end picks the panel up; the lap
 * console stays where it is put (XWA_XR_CONSOLE_POSE, saved by the host).
 *
 * Side panel (XWA_XR_SIDE_PANEL, on unless 0): on the right, three tabs
 * (XWA_XR_PANEL_TAB = map | voice | buttons): a tactical map of the area
 * from TieSnapshot.flights[], the voice commands and the controller layout.
 *
 * Quick wheel (XWA_XR_WHEEL, on unless 0): the left grip is TIE's shift
 * layer for the flight buttons, so the wheel opens when that grip is held
 * for a moment (TIE_VR_WHEEL_DELAY_S) without a grip + button chord. It
 * floats in front of that controller; pointing picks a slice, its trigger
 * takes it (the wheel stays open), letting go of the grip closes it. A
 * chord pressed while it is open closes it (the chord still acts), and the
 * trigger pulled while pointing at no slice is grip + left trigger.
 *
 * Per host frame: BeginFrame, SetState (game state), then the flight
 * renderer calls Frame / PanelFrame / WheelFrame while it renders in
 * stereo, then EndFrame. Pressed buttons come out of PollAction.
 */
#ifndef TIE_REMASTER_FLIGHT_VR_CONSOLE_H
#define TIE_REMASTER_FLIGHT_VR_CONSOLE_H

#include "aeron/render.h"
#include "aeron/xr.h"
#include "tie_runtime/input/vr_console_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Held this long with no chord, the left grip opens the quick wheel. */
#define TIE_VR_WHEEL_DELAY_S 0.25f

void TieVrConsole_BeginFrame(float dt_seconds);
void TieVrConsole_SetState(const TieVrConsoleState* state);

/* The lap console: handles the hands and, when it changed, redraws its
 * panel layer (AERON_XR_PANEL_CONSOLE, width x height). Fills out[0] with
 * its quad and returns the count for Aeron_XrStereoCommitPanelLayer: 0 when
 * it is not shown or out of view. `views` are the stereo frame's eyes. */
int TieVrConsole_Frame(AeronCommandBuffer* cmd, int width, int height, const AeronXrView views[2],
					   AeronXrPanel out[2]);
/* The side panel, on AERON_XR_PANEL_CARD. Call after Frame. */
int TieVrConsole_PanelFrame(AeronCommandBuffer* cmd, int width, int height, const AeronXrView views[2],
							AeronXrPanel out[2]);
/* The quick wheel, on AERON_XR_PANEL_CARD2. Call after PanelFrame (the
 * panels have had their turn with the grips). */
int TieVrConsole_WheelFrame(AeronCommandBuffer* cmd, int width, int height, const AeronXrView views[2],
							AeronXrPanel out[2]);
int TieVrConsole_WheelOpen(void);

/* While hand 0/1 points at a panel: the ray from the hand to where it
 * lands (LOCAL space); returns 1 (2: the quick wheel's pointer). */
int TieVrConsole_RayBeam(int hand, float start[3], float end[3]);

/* Releases triggers and grips when the panels were not shown this frame,
 * and hands the grip / trigger ownership to the engine and vr_controls. */
void TieVrConsole_EndFrame(void);

/* After the player moved the lap console: the new XWA_XR_CONSOLE_POSE
 * value, once, to be saved. */
int TieVrConsole_TakePlacement(char* out, int size);
/* The side panel shows another tab: its XWA_XR_PANEL_TAB value, once. */
int TieVrConsole_TakePanelTab(char* out, int size);

/* Pops a pressed action. Returns 0 when none is queued. */
int TieVrConsole_PollAction(TieVrConsoleAction* action, int* arg);

/* The voice tab's list (the voice stream's phrase table lives in tie_app):
 * `count` commands, the label and example phrase of each. */
void TieVrConsole_SetVoiceList(int (*count)(void), const char* (*label)(int index),
							   const char* (*phrase)(int index, int phrase));

void TieVrConsole_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

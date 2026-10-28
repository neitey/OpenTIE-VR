#ifndef TIE_APP_TEST_TEST_HARNESS_H
#define TIE_APP_TEST_TEST_HARNESS_H

/* Headless test facility for overnight runs without a human or a headset.
 * Everything is switched on by environment variables and does nothing
 * (one flag test per call) when none of them is set:
 *
 *   TIE_TEST_SCRIPT=<file>        scripted input (syntax: top of test_harness.c,
 *                                 examples: tools/vrtest/scripts)
 *   TIE_TEST_CAPTURE=<dir>        PNG captures (window composition, XR eyes/HUD)
 *   TIE_TEST_CAPTURE_FRAMES=list  "100,250,400" or "every:N" (default: script
 *                                 'capture' lines only)
 *   TIE_TEST_EXIT_FRAME=N         clean exit after host frame N
 *   TIE_TEST_WINDOW=WxH           window size (default: Aeron's auto size)
 *   TIE_TEST_XR=1                 fake XR headset: Aeron_XrIsPresenting = 1,
 *                                 Aeron_XrStereoBegin hands out real eye/HUD/
 *                                 panel render targets with scripted head poses
 *   TIE_TEST_XR_EYE=WxH           fake eye size (default 512x512)
 *   TIE_TEST_XR_HEAD=yaw,pitch[,roll[,x,y,z]]  initial fake head pose (deg, m)
 *   TIE_TEST_MISSION=<m>          skip the frontend: create pilot TEST and fly
 *                                 at once. <m> = train[:ship[:level]] (training
 *                                 course; ship 0 T/F .. 6, level 0..) or a
 *                                 MISSION/<m>.TIE name flown as a combat-chamber
 *                                 mission (HF1W historical, B1M1FM battle 1 ...)
 *   TIE_TEST_LOG=1                verbose per-frame log
 *
 * Log lines start with "[tie-test]" for grepping. */

#include "aeron/aeron.h"
#include "aeron/input.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads the environment. Call before Aeron_Init. Returns true when any
 * test switch is on. */
bool TieTest_Init(void);
bool TieTest_Active(void);
/* Window size override (TIE_TEST_WINDOW). */
void TieTest_ConfigureAeron(AeronConfig* config);
/* After TieRuntime_Init: TIE_TEST_MISSION (redirects the shell's first
 * scene to a flight). */
void TieTest_AfterRuntimeInit(void);
/* After Aeron_BeginFrame / Aeron_InputSnapshot: applies this frame's
 * scripted input to the snapshot (keys, text, mouse, virtual gamepad). */
void TieTest_ApplyInput(AeronInputSnapshot* input);
/* Right before Aeron_Present: arms the window capture when due. */
void TieTest_BeforePresent(void);
/* Right after Aeron_Present: XR captures, frame counter. Returns true when
 * the run should end now (quit line, TIE_TEST_EXIT_FRAME, failed wait). */
bool TieTest_AfterPresent(void);
/* Host frames run so far (the script's frame clock). */
uint64_t TieTest_Frame(void);
/* Process exit status for the run (0 ok, 3 a wait timed out). */
int TieTest_ExitStatus(void);
void TieTest_Shutdown(void);

/* ---- Implemented in test_wrap.c (link-time wraps, Linux desktop only) ---- */

/* 1 when the build links the capture/XR wraps (TIE_TEST_WRAP). */
int TieTestWrap_Available(void);
/* Captures the next Aeron_Present window composition into `path` (PNG). */
void TieTestWrap_RequestWindowCapture(const char* path);
/* Fake XR headset. */
void TieTestXr_Enable(int eye_width, int eye_height);
int TieTestXr_Enabled(void);
void TieTestXr_SetHead(const float yaw_pitch_roll_deg[3], const float position_m[3]);
/* After Aeron_Present: writes <prefix>_eye_left.png etc. for whatever was
 * committed this frame plus <prefix>_xr.txt (the committed layers), when
 * `prefix` is non-NULL; then clears the per-frame commit state. Returns the
 * number of images written. */
int TieTestXr_EndFrame(const char* prefix);
/* Scripted XR controller input: a flight-button press for
 * Aeron_XrPollButton (AeronXrButton), a menu latch (0 A, 1 B, 2 X, 3 Y) for
 * Aeron_XrTake*Pressed, the throttle lever (0..1), a hand's trigger/grip. */
void TieTestXr_QueueButton(int button);
void TieTestXr_MenuPress(int index);
void TieTestXr_SetThrottle(float value);
void TieTestXr_SetHandInput(int hand, float trigger, float grip);
/* A hand's buttons held (AERON_XR_HAND_BUTTON_*, Aeron_XrGetHand). */
void TieTestXr_SetHandButtons(int hand, uint32_t buttons);
/* A hand's pose (x, y, z metres LOCAL, yaw, pitch degrees), replacing
 * TIE_TEST_XR_LHAND/RHAND from now on. */
void TieTestXr_SetHandPose(int hand, const float pose[5]);
/* The game's flight stick (Aeron_XrSetVirtualStick): 1 while grabbed, with
 * x (right), y (forward = nose down), roll in -1..1. */
int TieTestXr_VirtualStick(float out[3]);
/* 1 while the game has the thumbsticks swapped (Aeron_XrSetSwapSticks). */
int TieTestXr_SwapSticks(void);
/* Frames on which stereo was committed so far. */
uint64_t TieTestXr_CommittedFrames(void);

#ifdef __cplusplus
}
#endif

#endif

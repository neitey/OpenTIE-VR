#ifndef TIE_RUNTIME_RUNTIME_FLIGHT_REQUESTS_H
#define TIE_RUNTIME_RUNTIME_FLIGHT_REQUESTS_H

/* Request and handoff channels between the recovered in-flight input
 * dispatcher (user_userinterface / user_inputforplane) and the
 * cooperative flight tasks. The original code entered the pause wait,
 * the in-flight info room and the replay viewer synchronously from the
 * key handlers; the port records the request and the flight task step
 * pushes the corresponding task after tie_doframe returns. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sub-modal result handoff shared by every leaf sub-modal task (msgroom,
 * goals, maproom, damage, wingman, help, option, inflightinfo). The
 * producer publishes its return code on the tick it finishes; the parent
 * task reads it once in its post-sub-modal phase. Values mirror the
 * original synchronous returns:
 *   -1 / 0xFFFF : navigate to previous tab
 *    0          : exit info room
 *   +1          : navigate to next tab
 *   other       : sub-modal-specific (e.g. option's exit codes) */
void TieFlightRequest_SetSubmodalResult(int32_t result);
int32_t TieFlightRequest_SubmodalResult(void);

/* Info-room request: a key handler records the screen index (0..6);
 * the flight/replay task consumes it (returning -1 when none is pending)
 * and pushes TieInflightInfo_Begin. */
void TieFlightRequest_InfoRoom(int32_t screen);
int32_t TieFlightRequest_ConsumeInfoRoom(void);

/* Key left by an info room that closed on the wingmen screen (4), such as
 * a wingman order chosen there. The original dispatched it again before
 * the frame continued; the port runs the room as a task, so the info task
 * records the key and the next user_inputforplane dispatches it before
 * that frame's own key. Consume returns false when none is pending. */
void TieFlightRequest_SetRoomKey(int16_t key);
bool TieFlightRequest_ConsumeRoomKey(int16_t* key);

/* Replay-viewer request from the 'v' key; the flight task consumes it and
 * pushes TieReplaySession_Begin, then posts the RESUMED banner after the
 * viewer pops. */
void TieFlightRequest_ReplayViewer(void);
bool TieFlightRequest_ConsumeReplayViewer(void);

/* Pause: the original blocked in user_userinterface until a key arrived.
 * The port enters a paused state that tie_doframe observes, services the
 * pending keys on each later user_userinterface call, and resumes on the
 * mapped Pause command. */
void TieFlightPause_Enter(void);
void TieFlightPause_Service(void);
bool TieFlightPause_IsActive(void);

/* VR launch hold, after OpenXWA-VR's "Press Any Key to Launch"
 * (XWA_HANGAR_PROMPT): in the headset a mission would otherwise start
 * while the player is still settling in. TieFlightRuntime_PrepareSimulator
 * arms it for a mission flight (not film playback); the flight task takes
 * the arm after the mission's first frame and enters the game's own pause
 * with a launch prompt in the message bar (sim, mission clock and music
 * stopped; the scene and cockpit keep rendering, so the player can look
 * around). Any new key, controller button, trigger or click launches.
 * Enabled = XWA_HANGAR_PROMPT (0 off, anything else on) or, unset, while
 * a headset presents. */
bool TieFlightLaunch_Enabled(void);
void TieFlightLaunch_Arm(bool armed);
bool TieFlightLaunch_TakeArmed(void);
void TieFlightLaunch_EnterHold(void);
bool TieFlightLaunch_HoldActive(void);
/* Message-bar text for MSG_PAUSED while the hold is on, else NULL; and the
 * centring pad applied to the expanded body (msg.c). */
const char* TieFlightLaunch_Message(void);
/* The trigger that launched does not also fire: joystick buttons read as
 * released until they all are (TieInput_ReadJoystick). */
void TieFlightLaunch_FilterButtons(uint16_t* buttons);
void TieFlightLaunch_CenterBody(char* body, size_t size);

#ifdef __cplusplus
}
#endif

#endif

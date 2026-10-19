#ifndef TIE_APP_QUEST_VR_VOICE_H
#define TIE_APP_QUEST_VR_VOICE_H

/*
 * VR voice commands (ported from OpenXWA-VR's vr_voice.c): push-to-talk on
 * the Touch controllers' thumb rests (double-tap, then speak; or hold while
 * speaking), recognized on the headset (voice_core.c), matched to one of
 * TIE's input actions (voice_commands.c) and dispatched like a key
 * binding, with a confirmation line in the flight message bar.
 * Only listens in flight.
 *
 * XWA_XR_VOICE: 0 off, doubletap (default), hold.
 * XWA_VOICE_LIB, XWA_VOICE_MODEL_DIR (desktop), XWA_VOICE_DEBUG.
 * TIE_VOICE_INJECT="next target;full throttle" (tests, any platform, no
 * headset needed): handles these as if heard, one every 90 flight frames.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Once per host frame, before the game tick. `blocked`: the host menu is
 * open or the game is paused by the host. */
void TieVrVoice_Update(bool blocked);

void TieVrVoice_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

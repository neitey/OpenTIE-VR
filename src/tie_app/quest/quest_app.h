#ifndef TIE_APP_QUEST_QUEST_APP_H
#define TIE_APP_QUEST_QUEST_APP_H

/* Standalone headsets (Meta Quest, Pico, Steam Frame; Android):
 * settings file, log, crash report, headset defaults and the game-data
 * folders. Everything here is a no-op on the desktop builds. */

#include "tie_app/application.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Before Aeron_Init: opentie-quest.cfg -> environment, log and crash files,
 * headset tier defaults (eye size, refresh, foveation), the Vulkan shim.
 * Fills unset game-data folders in *launch with the app's own
 *   files/TIE95  (1995 Collector's CD-ROM edition)
 *   files/TIE98  (1998 Windows edition)
 * when they exist. Returns 1 when VR should be requested. */
int TieQuest_PreInit(TieLaunchOptions* launch);

/* After Aeron_Init: shim check, pilots copied by the PC launcher. */
void TieQuest_PostInit(void);

/* Once per host frame (foveation shim log and start guard). */
void TieQuest_Frame(void);

/* Writes KEY=VALUE into opentie-quest.cfg (replacing an existing line). */
void TieQuest_SaveSetting(const char* key, const char* value);

/* The app's external files folder ("" on the desktop). */
const char* TieQuest_ExternalDir(void);

#ifdef __cplusplus
}
#endif

#endif

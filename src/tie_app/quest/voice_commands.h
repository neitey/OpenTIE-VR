#ifndef TIE_APP_QUEST_VOICE_COMMANDS_H
#define TIE_APP_QUEST_VOICE_COMMANDS_H

/* Voice commands, the matcher: what the recognizer heard (lower case
 * English words) -> one of TIE Fighter's input actions. Pure C, no
 * platform code, so it is unit-tested on the desktop (tie_voice_test).
 * Same approach as OpenXWA-VR's voice_core.c: a phrase table per command,
 * synonyms folded, fillers dropped, a word alignment cost with spelling
 * distance, and a "clearly better than the runner-up" rule. */

#include "tie_runtime/input/actions.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The command a heard phrase means. Returns 1 with the action and a short
 * label for the confirmation ("Wingmen: attack my target"); 0 when nothing
 * matches well enough. */
int TieVoiceCommands_Match(const char* heard, TieInputAction* action, const char** label);

/* For help texts and tests: the commands, their action, label and first
 * phrase. */
int TieVoiceCommands_Count(void);
TieInputAction TieVoiceCommands_Action(int index);
const char* TieVoiceCommands_Label(int index);
const char* TieVoiceCommands_Phrase(int index, int phrase);

#ifdef __cplusplus
}
#endif

#endif

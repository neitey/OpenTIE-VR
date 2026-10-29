#ifndef TIE_RUNTIME_RUNTIME_SHELL_TASK_H
#define TIE_RUNTIME_RUNTIME_SHELL_TASK_H

#include "tie/shellext.h"
#include <stdbool.h>
typedef enum ShellPhase {
	SHELL_PHASE_DISPATCH,
	SHELL_PHASE_FLIGHT_BEGIN,
	SHELL_PHASE_AFTER_FLIGHT,
	SHELL_PHASE_AWAITING,
	SHELL_PHASE_AFTER_SUDDEN_FADE,
} ShellPhase;
typedef struct ShellTask {
	SceneHeadStruct the_head;
	int16_t cur_scene;
	int16_t next_scene;
	int16_t exit_flag;
	int16_t script;
	bool diff_disabled;
	bool owns_tie98_music;
	bool started;
	bool finished;
	ShellPhase phase;
} ShellTask;
bool TieShell_PrepareScene(int16_t scene);
/* Push the sudden-scene-end fade; the caller yields until it completes. */
void TieShell_PushSuddenSceneFadeTask(void);

#include <stdint.h>

void TieShell_Begin(int16_t scene, int16_t script);

/* The scene the shell is running now (0 before the first). */
int16_t TieShell_CurrentScene(void);
void TieShell_NoteScene(int16_t scene);
/* The startup films and the title crawl, before the TIE Fighter logo. */
bool TieShell_IsIntroScene(int16_t scene);
/* VR menu button in the intro: when the current scene ends, the shell goes
 * on to the TIE Fighter logo (SCENE_TIELOGO) instead of the next intro film. */
void TieShell_RequestIntroSkip(void);
/* The shell's next scene once `cur_scene` has ended, with a pending intro
 * skip applied (and cleared). */
int16_t TieShell_ApplyIntroSkip(int16_t cur_scene, int16_t next_scene);

#endif

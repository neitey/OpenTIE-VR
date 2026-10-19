#ifndef TIE_APP_QUEST_ERROR_SCREEN_H
#define TIE_APP_QUEST_ERROR_SCREEN_H

enum {
	TIE_ERROR_SCREEN_FAILED = 0,
	TIE_ERROR_SCREEN_RETRY = 1,
	TIE_ERROR_SCREEN_QUIT = 2,
};

/* Shows title + message with a retry button (retry_label, or none) and Quit
 * on the virtual screen until one is clicked. during_fatal: the engine is
 * already quitting (a fatal error); show it anyway. */
int TieErrorScreen_Run(const char* title, const char* message, const char* retry_label, int during_fatal);

/* For Aeron_SetFatalErrorHandler on the headset. */
void TieErrorScreen_FatalHandler(const char* title, const char* message);

#endif

#ifndef TIE_APP_QUEST_CRASH_REPORT_H
#define TIE_APP_QUEST_CRASH_REPORT_H

/* Writes a native crash report (fault + backtrace) to `path` when the
 * process dies on a fatal signal. No-op on platforms without support. */
void TieCrashReport_Install(const char* path);

#endif

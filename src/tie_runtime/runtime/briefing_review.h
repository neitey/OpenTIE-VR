#ifndef TIE_RUNTIME_RUNTIME_BRIEFING_REVIEW_H
#define TIE_RUNTIME_RUNTIME_BRIEFING_REVIEW_H

/* The in-flight "Briefing" page of the settings overlay (VR), after
 * OpenXWA-VR's pause-menu "Review Briefing": the current mission's briefing
 * captions and the officers' pre-mission answers, read from the mission's
 * .TIE file (the strings the briefing room shows), plus the live mission
 * goals as the goals room lists them. */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TIE_BRIEFING_REVIEW_QA_MAX 10

typedef struct TieBriefingReviewQa {
	char question[160];
	char answer[1280]; /* paragraphs separated by '\n' */
	bool secret_order; /* asked of the Emperor's agent, not the flight officer */
} TieBriefingReviewQa;

typedef struct TieBriefingReview {
	char mission[16]; /* "HF1W" */
	char title[160];
	char captions[3072]; /* map captions, one paragraph per line */
	int qa_count;
	TieBriefingReviewQa qa[TIE_BRIEFING_REVIEW_QA_MAX];
	/* Goals room text: one goal per line; a line starting with '\001' is a
	 * category title ("Primary goals ..."). */
	char goals[3072];
} TieBriefingReview;

/* True in a mission flight (not training, not a film replay) while a headset
 * presents, or with TIE_XR_BRIEFING_PAGE=1 (0 hides it). */
bool TieBriefingReview_Available(void);

/* The page's text; NULL when not available. The file strings are read once
 * per mission; the goals are captured again when refresh_goals is set. */
const TieBriefingReview* TieBriefingReview_Get(bool refresh_goals);

#ifdef __cplusplus
}
#endif

#endif

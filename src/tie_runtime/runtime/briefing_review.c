#include "tie_runtime/runtime/briefing_review.h"

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "tie/goals.h"
#include "tie/shipext.h"
#include "tie/tie.h"
#include "tie_runtime/snapshot/snapshot.h"

#include <SDL3/SDL_stdinc.h>
#include <stdio.h>
#include <landru/file.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* .TIE layout (as player_Load_Display_Map reads it): word -1 (new format),
 * flight-group count, message count, global-goal count, the rest of the
 * header, flight groups, messages, goals, the briefing page, then 32 tag
 * strings, 32 caption strings and 20 officer strings (10 pre-mission:
 * 5 flight officer + 5 secret order; 10 debrief), each word-length
 * prefixed. The old format has fixed 40/160-byte strings and no officers. */
enum {
	BRIEF_HEADER_REST = 450,
	BRIEF_FG_SIZE = 292,
	BRIEF_MESSAGE_SIZE = 90,
	BRIEF_GOAL_SIZE = 28,
	BRIEF_PAGE_SIZE = 810,
	BRIEF_FILE_MAX = 1 << 20,
};

static TieBriefingReview s_review;
static char s_loaded_file[64];
static bool s_loaded_ok;

typedef struct BriefReader {
	const uint8_t* data;
	size_t size;
	size_t pos;
	bool ok;
} BriefReader;

static int BriefReader_Word(BriefReader* r) {
	if (r->pos + 2 > r->size) {
		r->ok = false;
		return 0;
	}
	const int v = (int16_t)(r->data[r->pos] | (r->data[r->pos + 1] << 8));
	r->pos += 2;
	return v;
}

static void BriefReader_Skip(BriefReader* r, size_t n) {
	if (r->pos + n > r->size) {
		r->ok = false;
		r->pos = r->size;
		return;
	}
	r->pos += n;
}

/* One string: (pointer, length) into the file, empty on a short read. */
static const uint8_t* BriefReader_String(BriefReader* r, bool prefixed, size_t fixed, size_t* len) {
	size_t n = fixed;
	if (prefixed) {
		const int w = BriefReader_Word(r);
		n = w > 0 ? (size_t)w : 0;
	}
	*len = 0;
	if (!r->ok || r->pos + n > r->size) {
		r->ok = false;
		return NULL;
	}
	const uint8_t* s = r->data + r->pos;
	r->pos += n;
	size_t l = 0;
	while (l < n && s[l])
		++l;
	*len = l;
	return s;
}

/* Appends cleaned text: briefing highlight marks ('[' ']' '>'), officer
 * control codes (1/2 bold, 3 + mood, 4/5 + condition) and non-ASCII bytes
 * dropped. The officer text is pre-wrapped for the talk box: a line that
 * starts with spaces opens a paragraph, other line breaks join with a space. */
static void Brief_AppendClean(char* out, size_t cap, const uint8_t* s, size_t n) {
	size_t len = strlen(out);
	bool line_start = len == 0;
	for (size_t i = 0; i < n && len + 1 < cap; ++i) {
		uint8_t ch = s[i];
		if (ch == 3 || ch == 4 || ch == 5) {
			++i;
			continue;
		}
		if (ch == '\n') {
			const bool paragraph = i + 1 < n && s[i + 1] == ' ';
			if (len && out[len - 1] != '\n' && out[len - 1] != ' ')
				out[len++] = paragraph ? '\n' : ' ';
			else if (len && paragraph && out[len - 1] == ' ')
				out[len - 1] = '\n';
			line_start = true;
			continue;
		}
		if (ch < 0x20 || ch >= 0x7F || ch == '[' || ch == ']' || ch == '>')
			continue;
		if (ch == ' ' && (line_start || (len && (out[len - 1] == ' ' || out[len - 1] == '\n'))))
			continue;
		line_start = false;
		out[len++] = (char)ch;
	}
	while (len && out[len - 1] == ' ')
		--len;
	out[len] = '\0';
}

static void Brief_AppendLine(char* out, size_t cap, const uint8_t* s, size_t n) {
	size_t len = strlen(out);
	if (len && out[len - 1] != '\n' && len + 1 < cap) {
		out[len++] = '\n';
		out[len] = '\0';
	}
	Brief_AppendClean(out, cap, s, n);
}

static bool Brief_Parse(const uint8_t* data, size_t size) {
	BriefReader r = { data, size, 0, true };
	int version = BriefReader_Word(&r);
	int fg_count;
	if (version >= 0) {
		fg_count = version;
		version = 0;
	} else {
		fg_count = BriefReader_Word(&r);
	}
	const int message_count = BriefReader_Word(&r);
	const int goal_count = BriefReader_Word(&r);
	if (!r.ok || fg_count < 0 || fg_count > 255 || message_count < 0 || message_count > 255 ||
		goal_count < 0 || goal_count > 16)
		return false;
	BriefReader_Skip(&r, BRIEF_HEADER_REST + (size_t)fg_count * BRIEF_FG_SIZE +
							 (size_t)message_count * BRIEF_MESSAGE_SIZE +
							 (size_t)goal_count * BRIEF_GOAL_SIZE + BRIEF_PAGE_SIZE);
	const bool prefixed = version != 0;
	size_t n;
	for (int i = 0; i < 32 && r.ok; ++i)
		(void)BriefReader_String(&r, prefixed, 40, &n); /* map tags */
	bool title_done = false;
	for (int i = 0; i < 32 && r.ok; ++i) {
		const uint8_t* s = BriefReader_String(&r, prefixed, 160, &n);
		if (!s || !n)
			continue;
		/* The opening caption, marked '>', is the mission's title. */
		if (!title_done && !s_review.captions[0] && s[0] == '>') {
			Brief_AppendClean(s_review.title, sizeof s_review.title, s, n);
			title_done = true;
			continue;
		}
		title_done = true;
		Brief_AppendLine(s_review.captions, sizeof s_review.captions, s, n);
	}
	if (!r.ok)
		return s_review.captions[0] != '\0';
	if (!prefixed)
		return true;
	for (int i = 0; i < 10 && r.ok; ++i) {
		const uint8_t* s = BriefReader_String(&r, true, 0, &n);
		if (!s || !n || s_review.qa_count >= TIE_BRIEFING_REVIEW_QA_MAX)
			continue;
		size_t q = 0;
		while (q < n && s[q] != '\n')
			++q;
		TieBriefingReviewQa* qa = &s_review.qa[s_review.qa_count];
		memset(qa, 0, sizeof *qa);
		Brief_AppendClean(qa->question, sizeof qa->question, s, q);
		if (q < n)
			Brief_AppendClean(qa->answer, sizeof qa->answer, s + q + 1, n - q - 1);
		qa->secret_order = i >= 5;
		if (qa->question[0])
			++s_review.qa_count;
	}
	return true;
}

static void TieBriefingReview_Load(void) {
	memset(&s_review, 0, sizeof s_review);
	snprintf(s_loaded_file, sizeof s_loaded_file, "%s", missionfilename);
	/* "mission/HF1W.tie" -> "HF1W" */
	const char* base = strrchr(missionfilename, '/');
	base = base ? base + 1 : missionfilename;
	size_t i = 0;
	for (; base[i] && base[i] != '.' && i + 1 < sizeof s_review.mission; ++i)
		s_review.mission[i] = (char)SDL_toupper((unsigned char)base[i]);
	s_review.mission[i] = '\0';

	s_loaded_ok = false;
	LandruFile* fp = xfile_Open_File(LANDRU_FILE_ROOT_AUXILIARY_ASSET, missionfilename, "rb");
	if (!fp) {
		Aeron_LogWarn("tie.briefing", "briefing review: cannot open %s", missionfilename);
		return;
	}
	uint8_t* data = NULL;
	int32_t size = 0;
	(void)xfile_Seek_File(fp, 0, LANDRU_SEEK_END);
	size = xfile_Tell_File(fp);
	(void)xfile_Seek_File(fp, 0, LANDRU_SEEK_SET);
	if (size > 0 && size <= BRIEF_FILE_MAX && (data = (uint8_t*)malloc((size_t)size)) != NULL &&
		xfile_Read_Data_From_File(fp, data, size))
		s_loaded_ok = Brief_Parse(data, (size_t)size);
	free(data);
	xfile_Close_File(fp);
	Aeron_LogInfo("tie.briefing", "briefing review: %s %s (%d officer answers)", missionfilename,
				  s_loaded_ok ? "read" : "unreadable", s_review.qa_count);
}

bool TieBriefingReview_Available(void) {
	const char* env = SDL_getenv("TIE_XR_BRIEFING_PAGE");
	if (env && env[0] == '0')
		return false;
	if (!(env && env[0] == '1') && !Aeron_XrIsPresenting())
		return false;
	const TieSnapshot* snapshot = TieSnapshot_Current();
	return snapshot && snapshot->scene_kind == TIE_SCENE_FLIGHT && snapshot->replay_mode != 2 &&
		   !replayviewmode && !mission.train_craft_type && missionfilename[0];
}

const TieBriefingReview* TieBriefingReview_Get(bool refresh_goals) {
	if (!TieBriefingReview_Available())
		return NULL;
	const bool reload = strcmp(s_loaded_file, missionfilename) != 0;
	if (reload)
		TieBriefingReview_Load();
	if (reload || refresh_goals)
		(void)goals_capturetext(s_review.goals, sizeof s_review.goals);
	return &s_review;
}

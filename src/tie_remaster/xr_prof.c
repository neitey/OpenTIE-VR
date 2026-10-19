#include "tie_remaster/xr_prof.h"

#include <SDL3/SDL.h>
#include <string.h>
#include "aeron/aeron.h"

static struct {
	int mode; /* -1 unchecked, 0 off, 1 while presenting, 2 always */
	double ms[TIE_XR_PROF_COUNT];
	unsigned counts[TIE_XR_PROF_N_COUNT];
	unsigned frames;
	uint64_t window_ns;
	uint64_t frame_ns;
	double frame_max_ms;
	uint64_t added_ns; /* everything added so far (for outer sections) */
} s_prof = { .mode = -1 };

static int TieXrProf_Mode(void) {
	if (s_prof.mode < 0) {
		const char* env = SDL_getenv("TIE_XR_PROFILE");
		s_prof.mode = env && env[0] == '0' ? 0 : env && env[0] == '1' ? 2 : 1;
	}
	return s_prof.mode;
}

uint64_t TieXrProf_Now(void) {
	return TieXrProf_Mode() ? SDL_GetTicksNS() : 0;
}

uint64_t TieXrProf_Add(TieXrProfSection section, uint64_t t0) {
	if (!t0 || section < 0 || section >= TIE_XR_PROF_COUNT)
		return 0;
	const uint64_t now = SDL_GetTicksNS();
	s_prof.ms[section] += (double)(now - t0) * 1e-6;
	s_prof.added_ns += now - t0;
	return now;
}

uint64_t TieXrProf_Mark(void) {
	return s_prof.added_ns;
}

uint64_t TieXrProf_AddOuter(TieXrProfSection section, uint64_t t0, uint64_t mark) {
	if (!t0 || section < 0 || section >= TIE_XR_PROF_COUNT)
		return 0;
	const uint64_t now = SDL_GetTicksNS();
	const uint64_t inner = s_prof.added_ns - mark;
	const uint64_t own = now - t0 > inner ? now - t0 - inner : 0;
	s_prof.ms[section] += (double)own * 1e-6;
	s_prof.added_ns += own;
	return now;
}

void TieXrProf_Count(TieXrProfCounter counter) {
	if (counter >= 0 && counter < TIE_XR_PROF_N_COUNT)
		s_prof.counts[counter]++;
}

void TieXrProf_FrameEnd(void) {
	const int mode = TieXrProf_Mode();
	if (!mode)
		return;
	const uint64_t now = SDL_GetTicksNS();
	if (s_prof.frame_ns) {
		const double frame_ms = (double)(now - s_prof.frame_ns) * 1e-6;
		if (frame_ms > s_prof.frame_max_ms)
			s_prof.frame_max_ms = frame_ms;
	}
	s_prof.frame_ns = now;
	if (!s_prof.window_ns) {
		/* First frame: start the window clean (startup is not a frame). */
		memset(s_prof.ms, 0, sizeof s_prof.ms);
		memset(s_prof.counts, 0, sizeof s_prof.counts);
		s_prof.frame_max_ms = 0.0;
		s_prof.window_ns = now;
		return;
	}
	s_prof.frames++;
	const double window_s = (double)(now - s_prof.window_ns) * 1e-9;
	if (window_s < 5.0)
		return;
	if ((mode == 2 || Aeron_XrIsPresenting()) && s_prof.frames > 0) {
		const double n = (double)s_prof.frames;
		double sum = 0.0;
		for (int i = 0; i < TIE_XR_PROF_COUNT; ++i)
			sum += s_prof.ms[i];
		const double frame_ms = window_s * 1000.0 / n;
		Aeron_LogInfo("tie.perf",
					  "CPU per frame over %.0f s (%u frames, %.1f fps, %.2f ms, worst %.1f ms): wait %.2f, input %.2f, "
					  "sim %.2f, prepare %.2f, cockpit %.2f, eyes %.2f, hud %.2f, panels %.2f, submit %.2f, env %.2f, "
					  "layers %.2f, ui %.2f, present %.2f, other %.2f ms | per frame: stereo %.2f, atlas %.2f, "
					  "pip %.2f, menu scene %.2f",
					  window_s, s_prof.frames, n / window_s, frame_ms, s_prof.frame_max_ms,
					  s_prof.ms[TIE_XR_PROF_WAIT] / n, s_prof.ms[TIE_XR_PROF_INPUT] / n, s_prof.ms[TIE_XR_PROF_SIM] / n,
					  s_prof.ms[TIE_XR_PROF_PREPARE] / n, s_prof.ms[TIE_XR_PROF_COCKPIT] / n,
					  s_prof.ms[TIE_XR_PROF_EYES] / n, s_prof.ms[TIE_XR_PROF_HUD] / n,
					  s_prof.ms[TIE_XR_PROF_PANELS] / n, s_prof.ms[TIE_XR_PROF_SUBMIT] / n,
					  s_prof.ms[TIE_XR_PROF_ENV] / n, s_prof.ms[TIE_XR_PROF_LAYERS] / n, s_prof.ms[TIE_XR_PROF_UI] / n,
					  s_prof.ms[TIE_XR_PROF_PRESENT] / n, frame_ms - sum / n,
					  s_prof.counts[TIE_XR_PROF_N_STEREO] / n, s_prof.counts[TIE_XR_PROF_N_ATLAS] / n,
					  s_prof.counts[TIE_XR_PROF_N_PIP] / n, s_prof.counts[TIE_XR_PROF_N_ENV] / n);
	}
	memset(s_prof.ms, 0, sizeof s_prof.ms);
	memset(s_prof.counts, 0, sizeof s_prof.counts);
	s_prof.frames = 0;
	s_prof.frame_max_ms = 0.0;
	s_prof.window_ns = now;
}

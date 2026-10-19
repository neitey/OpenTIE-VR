/* VR laser pointer steadying; see vr_pointer.h. */

#include "tie_runtime/input/vr_pointer.h"

#include <math.h>
#include <string.h>

#include "aeron/aeron.h"
#include "aeron/xr.h"

/* Smoothing time constant (s): strong while the hand is nearly still, almost
 * none on a quick sweep (logical px per second, 1080 high). */
#define TIE_VR_POINTER_TAU_SLOW 0.10f
#define TIE_VR_POINTER_TAU_FAST 0.012f
#define TIE_VR_POINTER_SPEED_SLOW 120.0f
#define TIE_VR_POINTER_SPEED_FAST 1400.0f
/* The shown pointer moves only once the filtered one is this far away. */
#define TIE_VR_POINTER_DEAD_BAND 2.5f
/* Click lock: the press takes the pointer from this long before it (the
 * trigger pull starts before the button counts as down), and holds it until
 * the ray is this far away (~1 degree on the Quest screen: 1080 px = 1.6 m
 * at 3.2 m). */
#define TIE_VR_POINTER_PRESS_LOOKBACK_S 0.06f
#define TIE_VR_POINTER_LOCK_RADIUS 36.0f
#define TIE_VR_POINTER_HISTORY 16

typedef struct TieVrPointerState {
	bool valid;
	float raw[2];
	float filtered[2];
	float out[2];
	bool locked;
	bool dragging;
	float lock_pos[2];
	double clock_s;
	/* The shown pointer over the last frames, for the press lookback. */
	float hist_pos[TIE_VR_POINTER_HISTORY][2];
	double hist_t[TIE_VR_POINTER_HISTORY];
	int hist_head, hist_count;
	bool offset_valid;
	float offset[2];
} TieVrPointerState;

static TieVrPointerState s_ptr;

void TieVrPointer_Reset(void) {
	const double clock = s_ptr.clock_s;
	memset(&s_ptr, 0, sizeof s_ptr);
	s_ptr.clock_s = clock;
}

bool TieVrPointer_Offset(float* dx, float* dy) {
	if (!s_ptr.offset_valid)
		return false;
	if (dx)
		*dx = s_ptr.offset[0];
	if (dy)
		*dy = s_ptr.offset[1];
	return true;
}

static float TieVrPointer_Dist(const float a[2], const float b[2]) {
	return hypotf(a[0] - b[0], a[1] - b[1]);
}

static void TieVrPointer_Remember(const float pos[2]) {
	s_ptr.hist_head = (s_ptr.hist_head + 1) % TIE_VR_POINTER_HISTORY;
	s_ptr.hist_pos[s_ptr.hist_head][0] = pos[0];
	s_ptr.hist_pos[s_ptr.hist_head][1] = pos[1];
	s_ptr.hist_t[s_ptr.hist_head] = s_ptr.clock_s;
	if (s_ptr.hist_count < TIE_VR_POINTER_HISTORY)
		++s_ptr.hist_count;
}

/* The shown pointer at about `seconds` ago; the current one when the frames
 * are too slow for that to mean anything. */
static void TieVrPointer_Recall(float seconds, float out[2]) {
	out[0] = s_ptr.out[0];
	out[1] = s_ptr.out[1];
	for (int i = 0; i < s_ptr.hist_count; ++i) {
		const int k = (s_ptr.hist_head - i + TIE_VR_POINTER_HISTORY) % TIE_VR_POINTER_HISTORY;
		const double age = s_ptr.clock_s - s_ptr.hist_t[k];
		if (age > 2.5 * (double)seconds)
			break;
		out[0] = s_ptr.hist_pos[k][0];
		out[1] = s_ptr.hist_pos[k][1];
		if (age >= (double)seconds)
			break;
	}
}

void TieVrPointer_Steady(AeronInputSnapshot* in, float dt_seconds) {
	s_ptr.offset_valid = false;
	if (!in || !Aeron_XrIsPresenting() || !in->mouse.inside_content) {
		TieVrPointer_Reset();
		return;
	}
	const float dt = dt_seconds < 1.0f / 240.0f ? 1.0f / 240.0f : dt_seconds > 0.5f ? 0.5f : dt_seconds;
	s_ptr.clock_s += dt;
	const float raw[2] = { (float)in->mouse.x, (float)in->mouse.y };
	if (!s_ptr.valid) {
		s_ptr.valid = true;
		s_ptr.locked = false;
		memcpy(s_ptr.raw, raw, sizeof raw);
		memcpy(s_ptr.filtered, raw, sizeof raw);
		memcpy(s_ptr.out, raw, sizeof raw);
		s_ptr.hist_count = 0;
	}

	/* Speed-adaptive low-pass, then the dead band. */
	const float speed = TieVrPointer_Dist(raw, s_ptr.raw) / dt;
	float k = (speed - TIE_VR_POINTER_SPEED_SLOW) / (TIE_VR_POINTER_SPEED_FAST - TIE_VR_POINTER_SPEED_SLOW);
	k = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
	/* A drag (button held, the click lock let go) follows closely. */
	if (s_ptr.dragging)
		k = 1.0f;
	const float tau = TIE_VR_POINTER_TAU_SLOW + (TIE_VR_POINTER_TAU_FAST - TIE_VR_POINTER_TAU_SLOW) * k;
	const float a = 1.0f - expf(-dt / tau);
	for (int c = 0; c < 2; ++c)
		s_ptr.filtered[c] += a * (raw[c] - s_ptr.filtered[c]);
	memcpy(s_ptr.raw, raw, sizeof raw);
	if (TieVrPointer_Dist(s_ptr.filtered, s_ptr.out) > TIE_VR_POINTER_DEAD_BAND)
		memcpy(s_ptr.out, s_ptr.filtered, sizeof s_ptr.out);

	/* Click lock. */
	const uint32_t left = AERON_MOUSE_BUTTON_LEFT;
	if (in->mouse.pressed_buttons & left) {
		TieVrPointer_Recall(TIE_VR_POINTER_PRESS_LOOKBACK_S, s_ptr.lock_pos);
		s_ptr.locked = true;
	}
	if (s_ptr.locked && TieVrPointer_Dist(raw, s_ptr.lock_pos) > TIE_VR_POINTER_LOCK_RADIUS) {
		/* Moved away on purpose: a drag (slider, scrollbar). */
		s_ptr.locked = false;
		s_ptr.dragging = true;
		memcpy(s_ptr.filtered, raw, sizeof raw);
		memcpy(s_ptr.out, raw, sizeof raw);
	}
	float shown[2] = { s_ptr.out[0], s_ptr.out[1] };
	if (s_ptr.locked) {
		shown[0] = s_ptr.lock_pos[0];
		shown[1] = s_ptr.lock_pos[1];
		if (!(in->mouse.buttons & left)) {
			/* Released at the pressed spot; the pointer stays there until the
			 * ray moves on. */
			s_ptr.locked = false;
			memcpy(s_ptr.filtered, s_ptr.lock_pos, sizeof s_ptr.filtered);
			memcpy(s_ptr.out, s_ptr.lock_pos, sizeof s_ptr.out);
		}
	}
	if (!(in->mouse.buttons & left))
		s_ptr.dragging = false;
	TieVrPointer_Remember(s_ptr.out);

	in->mouse.x = (int)lroundf(shown[0]);
	in->mouse.y = (int)lroundf(shown[1]);
	s_ptr.offset[0] = shown[0] - raw[0];
	s_ptr.offset[1] = shown[1] - raw[1];
	s_ptr.offset_valid = true;
}

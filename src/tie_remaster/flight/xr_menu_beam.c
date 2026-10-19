/* VR menu pointer beam; see xr_menu_beam.h. */

#include "tie_remaster/flight/xr_menu_beam.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "aeron/aeron.h"
#include "aeron/scene/draw_list2d.h"
#include "tie_remaster/vr/vr_gloves.h"

/* The beam texture: a vertical strip in the target's top-left corner, the
 * screen end at the top. */
#define TIE_BEAM_TEX_W 48
#define TIE_BEAM_WIDTH_M 0.016f /* the whole strip; the bright core is a quarter */
#define TIE_BEAM_MIN_LENGTH_M 0.005f

static AeronDrawList2D* s_beam_list;

static float TieFlightXrBeam_Dot(const float a[3], const float b[3]) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static bool TieFlightXrBeam_Normalize(float v[3]) {
	const float l = sqrtf(TieFlightXrBeam_Dot(v, v));
	if (l < 1e-6f)
		return false;
	v[0] /= l;
	v[1] /= l;
	v[2] /= l;
	return true;
}

/* Rotation with columns x, y, z -> quaternion (x, y, z, w). */
static void TieFlightXrBeam_BasisQuat(const float x[3], const float y[3], const float z[3], float q[4]) {
	const float m00 = x[0], m11 = y[1], m22 = z[2];
	const float trace = m00 + m11 + m22;
	if (trace > 0.0f) {
		const float s = sqrtf(trace + 1.0f) * 2.0f;
		q[3] = 0.25f * s;
		q[0] = (y[2] - z[1]) / s;
		q[1] = (z[0] - x[2]) / s;
		q[2] = (x[1] - y[0]) / s;
	} else if (m00 > m11 && m00 > m22) {
		const float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
		q[3] = (y[2] - z[1]) / s;
		q[0] = 0.25f * s;
		q[1] = (y[0] + x[1]) / s;
		q[2] = (z[0] + x[2]) / s;
	} else if (m11 > m22) {
		const float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
		q[3] = (z[0] - x[2]) / s;
		q[0] = (y[0] + x[1]) / s;
		q[1] = 0.25f * s;
		q[2] = (z[1] + y[2]) / s;
	} else {
		const float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
		q[3] = (x[1] - y[0]) / s;
		q[0] = (z[0] + x[2]) / s;
		q[1] = (z[1] + y[2]) / s;
		q[2] = 0.25f * s;
	}
}

static void TieFlightXrBeam_Strip(float x, float w, float radius, float soft, const float top[4],
								  const float bottom[4], int h) {
	AeronDrawList2DRRect r;
	memset(&r, 0, sizeof r);
	r.dst_x = x;
	r.dst_y = 0.0f;
	r.dst_w = w;
	r.dst_h = (float)h;
	r.radius_px = radius;
	r.soft_px = soft;
	memcpy(r.fill_top, top, sizeof r.fill_top);
	memcpy(r.fill_bottom, bottom, sizeof r.fill_bottom);
	AeronDrawList_AddRRect(s_beam_list, &r);
}

static float TieFlightXrBeam_EnvFloat(const char* name, float fallback) {
	const char* value = getenv(name);
	return value && value[0] ? (float)atof(value) : fallback;
}

static bool TieFlightXrBeam_DrawStrip(AeronCommandBuffer* cmd, AeronRenderTarget* target, int width, int height) {
	if (!cmd || !target || width < TIE_BEAM_TEX_W || height <= 0)
		return false;
	if (!s_beam_list) {
		s_beam_list = AeronDrawList_Create(16);
		if (!s_beam_list)
			return false;
	}
	AeronDrawList_Begin(s_beam_list, target, width, height, AERON_DRAWLIST2D_CLEAR, NULL);
	/* Premultiplied: a soft Imperial-red glow around a bright core, strongest
	 * at the screen end; translucent so the screen shows through. */
	float k = TieFlightXrBeam_EnvFloat("XWA_XR_BEAM_OPACITY", 0.5f);
	k = k < 0.05f ? 0.05f : (k > 1.0f ? 1.0f : k);
	const float glow_top[4] = { 0.85f * 0.45f * k, 0.12f * 0.45f * k, 0.08f * 0.45f * k, 0.45f * k };
	const float glow_bottom[4] = { 0.85f * 0.12f * k, 0.12f * 0.12f * k, 0.08f * 0.12f * k, 0.12f * k };
	const float core_top[4] = { k, 0.82f * k, 0.75f * k, k };
	const float core_bottom[4] = { 0.35f * k, 0.25f * k, 0.22f * k, 0.35f * k };
	TieFlightXrBeam_Strip(4.0f, TIE_BEAM_TEX_W - 8.0f, 20.0f, 16.0f, glow_top, glow_bottom, height);
	TieFlightXrBeam_Strip(TIE_BEAM_TEX_W * 0.5f - 6.0f, 12.0f, 6.0f, 3.0f, core_top, core_bottom, height);
	AeronDrawList_Render(s_beam_list, cmd);
	return true;
}

/* The beam's quad from `start` to `end` (LOCAL space), turned about its own
 * axis to face the eyes. */
static bool TieFlightXrBeam_Pose(const float start[3], const float end[3], const AeronXrView views[2], int height,
								 AeronXrPanel* out) {
	float along[3] = { end[0] - start[0], end[1] - start[1], end[2] - start[2] };
	const float length = sqrtf(TieFlightXrBeam_Dot(along, along));
	if (length < TIE_BEAM_MIN_LENGTH_M || !TieFlightXrBeam_Normalize(along))
		return false;
	const float mid[3] = { 0.5f * (start[0] + end[0]), 0.5f * (start[1] + end[1]), 0.5f * (start[2] + end[2]) };
	float to_eye[3];
	for (int k = 0; k < 3; ++k)
		to_eye[k] = 0.5f * (views[0].position[k] + views[1].position[k]) - mid[k];
	const float d = TieFlightXrBeam_Dot(to_eye, along);
	float z[3] = { to_eye[0] - d * along[0], to_eye[1] - d * along[1], to_eye[2] - d * along[2] };
	if (!TieFlightXrBeam_Normalize(z))
		return false; /* looking straight down the beam */
	const float x[3] = { along[1] * z[2] - along[2] * z[1], along[2] * z[0] - along[0] * z[2],
						 along[0] * z[1] - along[1] * z[0] };
	memset(out, 0, sizeof *out);
	out->rect[0] = 0;
	out->rect[1] = 0;
	out->rect[2] = TIE_BEAM_TEX_W;
	out->rect[3] = height;
	memcpy(out->position, mid, sizeof mid);
	TieFlightXrBeam_BasisQuat(x, along, z, out->orientation);
	out->size[0] = TIE_BEAM_WIDTH_M;
	out->size[1] = length;
	return true;
}

bool TieFlightXrBeam_Frame(AeronCommandBuffer* cmd, AeronRenderTarget* target, int width, int height,
						   const AeronXrView views[2], AeronXrPanel* out) {
	float hit[3], start[3];
	if (!cmd || !target || !views || !out || !Aeron_XrPointerHit(hit))
		return false;
	/* From the glove's index fingertip, else from the controller. */
	int pointing = 1;
	if (!TieVrGloves_IndexTip(1, start, &pointing)) {
		AeronXrHand hand;
		if (!Aeron_XrGetHand(1, &hand))
			return false;
		memcpy(start, hand.position, sizeof start);
	}
	/* It ends just in front of the screen's glass. */
	const float depth = TieFlightXrBeam_EnvFloat("XWA_XR_CURSOR_DEPTH", 0.02f);
	const float glass_z = hit[2] + depth; /* LOCAL: -z ahead */
	if (depth > 0.0f && start[2] > glass_z && hit[2] < glass_z) {
		const float f = (glass_z - start[2]) / (hit[2] - start[2]);
		for (int a = 0; a < 3; ++a)
			hit[a] = start[a] + (hit[a] - start[a]) * f;
	}
	return TieFlightXrBeam_Pose(start, hit, views, height, out) &&
		   TieFlightXrBeam_DrawStrip(cmd, target, width, height);
}

void TieFlightXrBeam_Shutdown(void) {
	if (s_beam_list) {
		AeronDrawList_Destroy(s_beam_list);
		s_beam_list = NULL;
	}
}

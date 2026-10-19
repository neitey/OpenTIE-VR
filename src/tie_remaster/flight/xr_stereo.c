/* OpenXR stereo flight helpers. See xr_stereo.h. */

#include "tie_remaster/flight/xr_stereo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "aeron/asset/opt_model.h"

float TieFlightXr_Env(const char* name, float fallback) {
	const char* value = getenv(name);
	if (value && value[0]) {
		const float parsed = (float)atof(value);
		if (parsed > 0.0f)
			return parsed;
	}
	return fallback;
}

bool TieFlightXr_StereoEnabled(void) {
	const char* env = getenv("XWA_XR_STEREO");
	return !(env && env[0] == '0');
}

TieFlightXrHudLayout TieFlightXr_HudLayout(void) {
	const char* layout = getenv("XWA_XR_HUD_LAYOUT");
	const char* headlock = getenv("XWA_XR_HUD_HEADLOCK");
	if ((layout && strcmp(layout, "fixed") == 0) || (headlock && headlock[0] == '0'))
		return TIE_FLIGHT_XR_HUD_FIXED;
	return TIE_FLIGHT_XR_HUD_HELMET;
}

float TieFlightXr_DepthBoost(void) {
	const float boost = TieFlightXr_Env("XWA_XR_DEPTH_BOOST", 1.0f);
	return boost < 1.0f ? 1.0f : (boost > 20.0f ? 20.0f : boost);
}

float TieFlightXr_UnitsPerMeter(void) {
	return AERON_OPT_UNITS_PER_METER * TieFlightXr_Env("XWA_XR_WORLD_SCALE", 1.0f);
}

/* Row-major rotation (rows = eye axes in world) -> quaternion (w, x, y, z),
 * the inverse of TieRenderMath_QuaternionToMat3. */
static void TieFlightXr_RowsToQuat(const float m[9], float q[4]) {
	const float trace = m[0] + m[4] + m[8];
	if (trace > 0.0f) {
		const float sq = sqrtf(trace + 1.0f) * 2.0f;
		q[0] = 0.25f * sq;
		q[1] = (m[7] - m[5]) / sq;
		q[2] = (m[2] - m[6]) / sq;
		q[3] = (m[3] - m[1]) / sq;
	} else if (m[0] > m[4] && m[0] > m[8]) {
		const float sq = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2.0f;
		q[0] = (m[7] - m[5]) / sq;
		q[1] = 0.25f * sq;
		q[2] = (m[1] + m[3]) / sq;
		q[3] = (m[2] + m[6]) / sq;
	} else if (m[4] > m[8]) {
		const float sq = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2.0f;
		q[0] = (m[2] - m[6]) / sq;
		q[1] = (m[1] + m[3]) / sq;
		q[2] = 0.25f * sq;
		q[3] = (m[5] + m[7]) / sq;
	} else {
		const float sq = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2.0f;
		q[0] = (m[3] - m[1]) / sq;
		q[1] = (m[2] + m[6]) / sq;
		q[2] = (m[5] + m[7]) / sq;
		q[3] = 0.25f * sq;
	}
}

/* Engine eye space is x right, y down, z forward; OpenXR view space is x
 * right, y up, -z forward, so the axis change is C = diag(1, -1, -1). */
void TieFlightXr_EyeCamera(const AeronSceneCamera* base, const AeronXrView* v, AeronSceneCamera* out) {
	static const float c[3] = { 1.0f, -1.0f, -1.0f };
	const float qx = v->orientation[0], qy = v->orientation[1], qz = v->orientation[2], qw = v->orientation[3];
	const float xr[9] = {
		1.0f - 2.0f * (qy * qy + qz * qz), 2.0f * (qx * qy - qw * qz),        2.0f * (qx * qz + qw * qy),
		2.0f * (qx * qy + qw * qz),        1.0f - 2.0f * (qx * qx + qz * qz), 2.0f * (qy * qz - qw * qx),
		2.0f * (qx * qz - qw * qy),        2.0f * (qy * qz + qw * qx),        1.0f - 2.0f * (qx * qx + qy * qy),
	};
	float r[9];
	float h[9];
	float e[9];
	float p[3];
	const float units = TieFlightXr_UnitsPerMeter();

	*out = *base;
	TieRenderMath_QuaternionToMat3(base->ori, r); /* rows: world -> craft eye */
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++)
			h[i * 3 + j] = c[i] * c[j] * xr[i * 3 + j]; /* head -> craft eye */
	}
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++)
			e[i * 3 + j] = h[0 * 3 + i] * r[0 * 3 + j] + h[1 * 3 + i] * r[1 * 3 + j] + h[2 * 3 + i] * r[2 * 3 + j];
	}
	TieFlightXr_RowsToQuat(e, out->ori);
	p[0] = v->position[0] * units;
	p[1] = -v->position[1] * units;
	p[2] = -v->position[2] * units;
	for (int j = 0; j < 3; j++)
		out->pos[j] = base->pos[j] + r[0 * 3 + j] * p[0] + r[1 * 3 + j] * p[1] + r[2 * 3 + j] * p[2];
	{
		const float tl = tanf(v->fov[0]), tr = tanf(v->fov[1]);
		const float tu = tanf(v->fov[2]), td = tanf(v->fov[3]);
		out->h_half_rad = atanf((tr - tl) * 0.5f);
		out->v_half_rad = atanf((tu - td) * 0.5f);
		out->proj_x_offset = -(tr + tl) / (tr - tl);
		out->proj_y_offset = -(tu + td) / (tu - td);
	}
}

static void TieFlightXr_FinishCamera(TieFlightCamera* fc, int w, int h) {
	fc->camera.viewport = (AeronRectI) { 0, 0, w, h };
	fc->h_half_rad = fc->camera.h_half_rad;
	fc->v_half_rad = fc->camera.v_half_rad;
	fc->proj_x_offset = fc->camera.proj_x_offset;
	fc->proj_y_offset = fc->camera.proj_y_offset;
	fc->flight_vp_x = 0.0f;
	fc->flight_vp_y = 0.0f;
	fc->flight_vp_w = (float)w;
	fc->flight_vp_h = (float)h;
	fc->rt_w = w;
	fc->rt_h = h;
	fc->xr = true;
	AeronScene_ComputeViewProj(&fc->camera, fc->view_proj);
}

void TieFlightXr_BuildEyes(const TieFlightCamera* base, const AeronXrStereoFrame* frame, TieFlightCamera eyes[2],
						   TieFlightCamera* both) {
	const float boost = TieFlightXr_DepthBoost();
	AeronXrView boosted[2] = { frame->views[0], frame->views[1] };
	AeronXrView span = frame->views[0];
	for (int a = 0; a < 3; a++) {
		const float centre = 0.5f * (frame->views[0].position[a] + frame->views[1].position[a]);
		for (int e = 0; e < 2; e++)
			boosted[e].position[a] = centre + boost * (frame->views[e].position[a] - centre);
		span.position[a] = centre;
	}
	for (int e = 0; e < 2; e++) {
		eyes[e] = *base;
		TieFlightXr_EyeCamera(&base->camera, &boosted[e], &eyes[e].camera);
		TieFlightXr_FinishCamera(&eyes[e], frame->width, frame->height);
	}
	/* One view covering both eyes (their FOVs are asymmetric): culling and
	 * the shared shadow atlas must see what either eye sees. */
	span.fov[0] = fminf(frame->views[0].fov[0], frame->views[1].fov[0]);
	span.fov[1] = fmaxf(frame->views[0].fov[1], frame->views[1].fov[1]);
	span.fov[2] = fmaxf(frame->views[0].fov[2], frame->views[1].fov[2]);
	span.fov[3] = fminf(frame->views[0].fov[3], frame->views[1].fov[3]);
	*both = *base;
	TieFlightXr_EyeCamera(&base->camera, &span, &both->camera);
	/* Pull the culling eye back by the boosted half separation so the union
	 * frustum also contains both (wider apart) eye positions. */
	{
		float half = 0.0f;
		for (int a = 0; a < 3; a++) {
			const float d = boosted[1].position[a] - boosted[0].position[a];
			half += d * d;
		}
		half = 0.5f * sqrtf(half) * TieFlightXr_UnitsPerMeter();
		const float t = fmaxf(tanf(-span.fov[0]), tanf(span.fov[1]));
		if (t > 1e-3f) {
			float rows[9];
			TieRenderMath_QuaternionToMat3(both->camera.ori, rows);
			const float back = half / t;
			for (int j = 0; j < 3; j++)
				both->camera.pos[j] -= rows[6 + j] * back;
		}
	}
	TieFlightXr_FinishCamera(both, frame->width, frame->height);
}

void TieFlightXr_HudQuad(const TieFlightCamera* hc, int head, int world_far, AeronXrHudQuad* out) {
	const float th = tanf(hc->h_half_rad);
	const float tv = tanf(hc->v_half_rad);
	memset(out, 0, sizeof *out);
	out->visible = 1;
	if (head) {
		const float d = TieFlightXr_Env("XWA_XR_HUD_DISTANCE", 3.0f);
		const float scale = TieFlightXr_Env("XWA_XR_HUD_SCALE", 0.65f);
		const char* smooth = getenv("XWA_XR_HUD_SMOOTH");
		out->size[0] = 2.0f * d * th * scale;
		out->size[1] = 2.0f * d * tv * scale;
		out->position[2] = -d;
		out->head_locked = 1;
		out->follow_s = 0.18f;
		if (smooth && smooth[0]) {
			const float value = (float)atof(smooth);
			out->follow_s = value > 0.0f ? value : 0.0f; /* 0 = rigid */
		}
		return;
	}
	/* With a depth boost the world reads closer by that factor; keep the
	 * brackets at a matching depth. */
	const float d = world_far ? TieFlightXr_Env("XWA_XR_HUD_WORLD_DISTANCE", 500.0f) / TieFlightXr_DepthBoost()
							  : TieFlightXr_Env("XWA_XR_HUD_DISTANCE", 3.0f);
	out->size[0] = 2.0f * d * th;
	out->size[1] = 2.0f * d * tv;
	out->position[0] = -hc->proj_x_offset * th * d;
	out->position[1] = -hc->proj_y_offset * tv * d;
	out->position[2] = -d;
}

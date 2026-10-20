/* VR cockpit HUD panels; see vr_cockpit_hud.h. Ported from OpenXWA-VR's
 * xwa_remaster/vr_cockpit_hud.c (placement, grab, per-cockpit layouts); the
 * panel contents come from TIE's instrument atlas instead of XWA's HUD
 * widgets.
 *
 * Panel definitions, all in tables here (to redesign the HUD, change these):
 *   CK_* enum + k_ids   the panels and their saved-layout ids
 *   k_rules             which TIE instruments go on which panel
 *   k_pad               margin of TIE's cockpit art around each panel's instruments
 *   k_spec_tie/_sim/_2d where each panel floats per cockpit and its size
 *                       (metres per TIE cockpit pixel, capped by a box) */

#include "tie_remaster/vr/vr_cockpit_hud.h"

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "aeron/scene/draw_list2d.h"
#include "tie_remaster/flight/cockpit/renderer.h"
#include "tie_remaster/vr/vr_hands.h"
#include "tie_runtime/snapshot/snapshot.h"

#include <SDL3/SDL_stdinc.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	CK_POD_L,
	CK_POD_R,
	CK_MSG,
	CK_THREAT,
	CK_TOPBAR,
	CK_CMD,
	CK_POWER_L,
	CK_POWER_R,
	CK_GAUGES_L,
	CK_GAUGES_R,
	CK_READOUTS_L,
	CK_READOUTS_R,
	CK_COUNT
};

_Static_assert(CK_COUNT <= TIE_VR_COCKPIT_HUD_MAX_PANELS, "panel count");

/* Placement relative to the seated eye point: yaw (+ right) and pitch (+ up)
 * in degrees, distance in metres. Size: m_per_px metres per pixel of TIE's
 * 640x480 cockpit (the message bar: per 1/480 of the HUD height), so a panel
 * is as big as its instruments are in TIE (whatever the craft's 2D layout),
 * at most box_w x box_h (aspect kept). */
typedef struct CkSpec {
	float yaw, pitch, distance;
	float box_w, box_h;
	float m_per_px;
} CkSpec;

/* TIE fighter / bomber pod (XWA's TIECOCKPIT, TIEBOMBERCOCKPIT: a 1.46 m ball,
 * round front window): the sensors either side of the window like OpenXWA's
 * radar pods, the power bars under them, the CMD over the dashboard's centre
 * console with the T/A, T/D top bar (lasers, warheads, speed, clock) on it
 * like OpenXWA's readout row, the shield/hull and speed/throttle/clock
 * groups beside it, the threat readouts above. */
static const CkSpec k_spec_tie[CK_COUNT] = {
	[CK_POD_L] = { -37.0f, 1.0f, 0.75f, 0.21f, 0.21f, 0.0018f },
	[CK_POD_R] = { 37.0f, 1.0f, 0.75f, 0.21f, 0.21f, 0.0018f },
	[CK_MSG] = { 0.0f, -9.0f, 0.95f, 0.0f, 0.0f, 0.0022f },
	[CK_THREAT] = { 0.0f, -13.5f, 0.78f, 0.30f, 0.05f, 0.0013f },
	[CK_TOPBAR] = { 0.0f, -19.5f, 0.68f, 0.40f, 0.07f, 0.0013f },
	[CK_CMD] = { 0.0f, -30.0f, 0.62f, 0.30f, 0.22f, 0.00115f },
	[CK_POWER_L] = { -42.0f, -16.0f, 0.72f, 0.07f, 0.17f, 0.0016f },
	[CK_POWER_R] = { 42.0f, -16.0f, 0.72f, 0.07f, 0.17f, 0.0016f },
	[CK_GAUGES_L] = { -21.0f, -33.0f, 0.60f, 0.15f, 0.15f, 0.00145f },
	[CK_GAUGES_R] = { 21.0f, -33.0f, 0.60f, 0.15f, 0.15f, 0.00145f },
	[CK_READOUTS_L] = { -33.0f, -26.0f, 0.62f, 0.12f, 0.20f, 0.00145f },
	[CK_READOUTS_R] = { 33.0f, -26.0f, 0.62f, 0.12f, 0.20f, 0.00145f },
};

/* Gunboat / missile boat (COMBATSIMCOCKPIT): a wider, flatter canopy and a
 * lower dashboard. */
static const CkSpec k_spec_sim[CK_COUNT] = {
	[CK_POD_L] = { -36.0f, 4.0f, 0.78f, 0.21f, 0.21f, 0.0018f },
	[CK_POD_R] = { 36.0f, 4.0f, 0.78f, 0.21f, 0.21f, 0.0018f },
	[CK_MSG] = { 0.0f, -8.0f, 0.95f, 0.0f, 0.0f, 0.0022f },
	[CK_THREAT] = { 0.0f, -12.5f, 0.80f, 0.30f, 0.05f, 0.0013f },
	[CK_TOPBAR] = { 0.0f, -18.0f, 0.70f, 0.40f, 0.07f, 0.0013f },
	[CK_CMD] = { 0.0f, -28.0f, 0.64f, 0.30f, 0.22f, 0.00115f },
	[CK_POWER_L] = { -42.0f, -14.0f, 0.74f, 0.07f, 0.17f, 0.0016f },
	[CK_POWER_R] = { 42.0f, -14.0f, 0.74f, 0.07f, 0.17f, 0.0016f },
	[CK_GAUGES_L] = { -21.0f, -31.0f, 0.62f, 0.15f, 0.15f, 0.00145f },
	[CK_GAUGES_R] = { 21.0f, -31.0f, 0.62f, 0.15f, 0.15f, 0.00145f },
	[CK_READOUTS_L] = { -33.0f, -24.0f, 0.64f, 0.12f, 0.20f, 0.00145f },
	[CK_READOUTS_R] = { 33.0f, -24.0f, 0.64f, 0.12f, 0.20f, 0.00145f },
};

/* 2D cockpit mode (no cockpit model, open space around): TIE's own screen
 * arrangement around the line of sight, sensors up in the corners. */
static const CkSpec k_spec_2d[CK_COUNT] = {
	[CK_POD_L] = { -33.0f, 14.0f, 0.80f, 0.21f, 0.21f, 0.0018f },
	[CK_POD_R] = { 33.0f, 14.0f, 0.80f, 0.21f, 0.21f, 0.0018f },
	[CK_MSG] = { 0.0f, -10.0f, 0.95f, 0.0f, 0.0f, 0.0022f },
	[CK_THREAT] = { 0.0f, -14.0f, 0.80f, 0.30f, 0.05f, 0.0013f },
	[CK_TOPBAR] = { 0.0f, 20.0f, 0.80f, 0.40f, 0.07f, 0.0013f },
	[CK_CMD] = { 0.0f, -29.0f, 0.64f, 0.30f, 0.22f, 0.00115f },
	[CK_POWER_L] = { -40.0f, -6.0f, 0.75f, 0.07f, 0.17f, 0.0016f },
	[CK_POWER_R] = { 40.0f, -6.0f, 0.75f, 0.07f, 0.17f, 0.0016f },
	[CK_GAUGES_L] = { -21.0f, -31.0f, 0.62f, 0.15f, 0.15f, 0.00145f },
	[CK_GAUGES_R] = { 21.0f, -31.0f, 0.62f, 0.15f, 0.15f, 0.00145f },
	[CK_READOUTS_L] = { -32.0f, -24.0f, 0.64f, 0.12f, 0.20f, 0.00145f },
	[CK_READOUTS_R] = { 32.0f, -24.0f, 0.64f, 0.12f, 0.20f, 0.00145f },
};

/* Short ids for the saved layout. */
static const char* const k_ids[CK_COUNT] = {
	[CK_POD_L] = "pod_l",	  [CK_POD_R] = "pod_r",		  [CK_MSG] = "msg",
	[CK_THREAT] = "threat",	  [CK_TOPBAR] = "topbar",	  [CK_CMD] = "cmd",
	[CK_POWER_L] = "power_l", [CK_POWER_R] = "power_r",	  [CK_GAUGES_L] = "gauges_l",
	[CK_GAUGES_R] = "gauges_r", [CK_READOUTS_L] = "readouts_l", [CK_READOUTS_R] = "readouts_r",
};

#define CK_GRAB_MARGIN_M 0.025f
#define CK_GRAB_DEPTH_M 0.06f
#define CK_GRIP_ON 0.6f
#define CK_GRIP_OFF 0.35f
#define CK_PAD_PX 4
/* Padding around the instruments' boxes, atlas UV (about 3 classic px). */
#define CK_PAD_U 0.005f
#define CK_PAD_V 0.0065f

/* Extra padding per panel (UV): the art frames around the drawn gauges. */
static const float k_pad[CK_COUNT][2] = {
	[CK_CMD] = { 0.012f, 0.022f },
	[CK_POWER_L] = { 0.004f, 0.006f },
	[CK_POWER_R] = { 0.004f, 0.006f },
	[CK_GAUGES_L] = { 0.030f, 0.028f },
	[CK_GAUGES_R] = { 0.030f, 0.028f },
	[CK_READOUTS_L] = { 0.016f, 0.010f },
	[CK_READOUTS_R] = { 0.016f, 0.010f },
};

static struct {
	/* Latched atlas rectangles (UV, grow-only per mission and cockpit). */
	uint64_t latch_key;
	float latched[CK_COUNT][4];
	uint8_t latched_valid[CK_COUNT];
	/* This frame's content: rectangles in the source target, visibility. */
	int rect[CK_COUNT][4];
	int source[CK_COUNT];
	uint8_t visible[CK_COUNT];
	int content_ready;
	float msg_m_per_px;
	/* Packing: the sizes it was made for, the result. */
	int pack_w, pack_h, pack_y0;
	float pack_src[CK_COUNT][2];
	int packed[CK_COUNT][4];
	float pack_scale;
	AeronDrawList2D* list;
	char logged[512];
	/* Placement per cockpit (XWA_XR_HUD_POSE_<COCKPIT>): LOCAL-space poses
	 * that replace the defaults. */
	char cockpit_key[96];
	char pose_env[1024];
	const CkSpec* spec;
	uint8_t custom[CK_COUNT];
	float cpos[CK_COUNT][3], cq[CK_COUNT][4];
	/* Grabbing: panel held by each hand (-1 none), the panel relative to the
	 * hand, hovered panel per hand, last grip value. */
	int grab_panel[2];
	float grab_pos[2][3], grab_q[2][4];
	int hover_panel[2];
	float grip_prev[2];
	int layout_dirty;
	char layout_text[1024];
	int initialized;
} s_ck;

static void ck_init(void) {
	if (s_ck.initialized)
		return;
	s_ck.grab_panel[0] = s_ck.grab_panel[1] = -1;
	s_ck.hover_panel[0] = s_ck.hover_panel[1] = -1;
	s_ck.initialized = 1;
}

static void ck_union(float acc[4], int* valid, const float b[4]) {
	if (!*valid) {
		memcpy(acc, b, sizeof(float) * 4);
		*valid = 1;
		return;
	}
	acc[0] = fminf(acc[0], b[0]);
	acc[1] = fminf(acc[1], b[1]);
	acc[2] = fmaxf(acc[2], b[2]);
	acc[3] = fmaxf(acc[3], b[3]);
}

/* ---- quaternion helpers (as OpenXWA-VR) ---- */

static void ck_rotate(const float q[4], const float v[3], float out[3]) {
	const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
	const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
	const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
	out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
	out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
	out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

static void ck_qmul(const float a[4], const float b[4], float out[4]) {
	const float r[4] = {
		a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
		a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
		a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
		a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
	};
	memcpy(out, r, sizeof r);
}

static void ck_qnorm(float q[4]) {
	const float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (l > 1e-6f) {
		for (int k = 0; k < 4; ++k)
			q[k] /= l;
	} else {
		q[0] = q[1] = q[2] = 0.0f;
		q[3] = 1.0f;
	}
}

static void ck_pose(const CkSpec* spec, float world_scale, AeronXrPanel* out) {
	const float deg = 3.14159265f / 180.0f;
	const float yaw = spec->yaw * deg, pitch = spec->pitch * deg;
	const float d = spec->distance / world_scale;
	/* LOCAL space: +X right, +Y up, -Z forward. */
	const float dir[3] = { sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch) };
	out->position[0] = dir[0] * d;
	out->position[1] = dir[1] * d;
	out->position[2] = dir[2] * d;
	/* The quad's +Z faces the eye point; its X stays level. */
	const float z[3] = { -dir[0], -dir[1], -dir[2] };
	float x[3] = { z[2], 0.0f, -z[0] };
	const float xl = sqrtf(x[0] * x[0] + x[2] * x[2]);
	x[0] /= xl;
	x[2] /= xl;
	const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
	const float m00 = x[0], m01 = y[0], m02 = z[0];
	const float m10 = x[1], m11 = y[1], m12 = z[1];
	const float m20 = x[2], m21 = y[2], m22 = z[2];
	const float trace = m00 + m11 + m22;
	float qx, qy, qz, qw;
	if (trace > 0.0f) {
		const float t = sqrtf(trace + 1.0f) * 2.0f;
		qw = 0.25f * t;
		qx = (m21 - m12) / t;
		qy = (m02 - m20) / t;
		qz = (m10 - m01) / t;
	} else if (m00 > m11 && m00 > m22) {
		const float t = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
		qw = (m21 - m12) / t;
		qx = 0.25f * t;
		qy = (m01 + m10) / t;
		qz = (m02 + m20) / t;
	} else if (m11 > m22) {
		const float t = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
		qw = (m02 - m20) / t;
		qx = (m01 + m10) / t;
		qy = 0.25f * t;
		qz = (m12 + m21) / t;
	} else {
		const float t = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
		qw = (m10 - m01) / t;
		qx = (m02 + m20) / t;
		qy = (m12 + m21) / t;
		qz = 0.25f * t;
	}
	out->orientation[0] = qx;
	out->orientation[1] = qy;
	out->orientation[2] = qz;
	out->orientation[3] = qw;
}

/* ---- per-cockpit layouts ---- */

static void ck_layout_key(const char* name, char* out, size_t size) {
	size_t n = (size_t)snprintf(out, size, "XWA_XR_HUD_POSE_");
	for (const char* c = name && name[0] ? name : "2D"; *c && n + 1 < size; ++c)
		if (isalnum((unsigned char)*c))
			out[n++] = (char)toupper((unsigned char)*c);
	out[n] = '\0';
}

static const CkSpec* ck_spec_for(const char* key) {
	if (strcmp(key, "XWA_XR_HUD_POSE_COMBATSIMCOCKPIT") == 0)
		return k_spec_sim;
	if (strcmp(key, "XWA_XR_HUD_POSE_2D") == 0)
		return k_spec_2d;
	return k_spec_tie; /* TIECOCKPIT, TIEBOMBERCOCKPIT and any other */
}

/* Reads this cockpit's layout ("id=x,y,z,qx,qy,qz,qw;...") when it changed
 * (also when the settings page cleared it). */
static void ck_read_layout(const char* cockpit) {
	char key[96];
	ck_layout_key(cockpit, key, sizeof key);
	const char* v = SDL_getenv(key);
	if (!v)
		v = "";
	if (strcmp(key, s_ck.cockpit_key) == 0 && strcmp(v, s_ck.pose_env) == 0 && s_ck.spec)
		return;
	snprintf(s_ck.cockpit_key, sizeof s_ck.cockpit_key, "%s", key);
	snprintf(s_ck.pose_env, sizeof s_ck.pose_env, "%s", v);
	s_ck.spec = ck_spec_for(key);
	memset(s_ck.custom, 0, sizeof s_ck.custom);
	const char* p = v;
	while (*p) {
		char id[24];
		float f[7];
		int used = 0;
		if (sscanf(p, "%23[^=]=%f,%f,%f,%f,%f,%f,%f%n", id, &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &used) ==
				8 &&
			used > 0) {
			for (int i = 0; i < CK_COUNT; ++i) {
				if (strcmp(id, k_ids[i]) == 0 && fabsf(f[0]) < 3.0f && fabsf(f[1]) < 3.0f && fabsf(f[2]) < 3.0f) {
					memcpy(s_ck.cpos[i], f, sizeof s_ck.cpos[i]);
					memcpy(s_ck.cq[i], &f[3], sizeof s_ck.cq[i]);
					ck_qnorm(s_ck.cq[i]);
					s_ck.custom[i] = 1;
				}
			}
			p += used;
		} else {
			const char* next = strchr(p, ';');
			if (!next)
				break;
			p = next;
		}
		while (*p == ';' || *p == ' ')
			p++;
	}
}

static void ck_save_layout(void) {
	int n = 0;
	s_ck.layout_text[0] = '\0';
	for (int i = 0; i < CK_COUNT; ++i) {
		if (!s_ck.custom[i])
			continue;
		n += snprintf(s_ck.layout_text + n, sizeof s_ck.layout_text - (size_t)n,
					  "%s%s=%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f", n ? ";" : "", k_ids[i], s_ck.cpos[i][0],
					  s_ck.cpos[i][1], s_ck.cpos[i][2], s_ck.cq[i][0], s_ck.cq[i][1], s_ck.cq[i][2], s_ck.cq[i][3]);
		if (n >= (int)sizeof s_ck.layout_text - 1)
			break;
	}
	SDL_setenv_unsafe(s_ck.cockpit_key, s_ck.layout_text, 1);
	snprintf(s_ck.pose_env, sizeof s_ck.pose_env, "%s", s_ck.layout_text);
	s_ck.layout_dirty = 1;
	Aeron_LogInfo("tie.vr", "VR HUD layout saved: %s=%s", s_ck.cockpit_key, s_ck.layout_text);
}

int TieVrCockpitHud_TakeLayout(char* key, int key_size, char* value, int value_size) {
	if (!s_ck.layout_dirty || !key || !value || key_size <= 0 || value_size <= 0)
		return 0;
	snprintf(key, (size_t)key_size, "%s", s_ck.cockpit_key);
	snprintf(value, (size_t)value_size, "%s", s_ck.layout_text);
	s_ck.layout_dirty = 0;
	return 1;
}

/* ---- content ---- */

/* What goes on which panel: the first rule that matches an instrument wins.
 * group: the cockpit renderer's classification (TieCockpitPanelId 0..4;
 * -2 unsorted; -3 the T/A, T/D top bar above the gunsight; -1 the gunsight
 * cluster, which stays on the far world quad; -7 any of 0..4; -9 any, a
 * subsystem check only). slider: 1 power bars only, 0
 * none of them, -1 either. first..last: instrument ids (TIE_HUDI_*), -1 any.
 * needs: subsystem bits (snapshot hud.subsystem_active) without which the
 * instrument is left out (TIE covers it up). drawn_only: only what it drew
 * counts (no anchor estimate). top/right: other panels when the instrument
 * sits in the top 20 % of TIE's cockpit / right of the CMD (-1 = panel). */
typedef struct CkRule {
	int8_t group, slider;
	int16_t first, last;
	uint16_t needs;
	uint8_t drawn_only;
	int8_t panel, top, right;
} CkRule;

#define CK_SF_BEAM 0x0100u	 /* SF_TRACTOR_BEAM */
#define CK_SF_SHIELDS 0x0001u /* SF_SHIELDS */

static const CkRule k_rules[] = {
	/* Tractor beam gauges only on craft that have one, the shield slider only
	 * with shields (else TIE shows a cover plate there). */
	{ -9, -1, TIE_HUDI_POWER_BEAM, TIE_HUDI_POWER_BEAM, CK_SF_BEAM, 0, -1, -1, -1 },
	{ -9, -1, TIE_HUDI_BEAM_ARC, TIE_HUDI_BEAM_ARC, CK_SF_BEAM, 0, -1, -1, -1 },
	{ -9, -1, TIE_HUDI_BEAM_FIRE, TIE_HUDI_BEAM_FIRE, CK_SF_BEAM, 0, -1, -1, -1 },
	{ -9, -1, TIE_HUDI_POWER_SHIELDS, TIE_HUDI_POWER_SHIELDS, CK_SF_SHIELDS, 0, -1, -1, -1 },
	{ TIE_COCKPIT_PANEL_SENSOR_FWD, -1, -1, -1, 0, 0, CK_POD_L, -1, -1 },
	{ TIE_COCKPIT_PANEL_SENSOR_REAR, -1, -1, -1, 0, 0, CK_POD_R, -1, -1 },
	/* Power sliders and speed/throttle/clock/recorder by the side of the CMD
	 * they are on, also where TIE's layout put them next to the CRT (T/I,
	 * gunboat, missile boat). */
	{ -7, 1, -1, -1, 0, 0, CK_POWER_L, -1, CK_POWER_R },
	{ -7, -1, TIE_HUDI_SPEED_DIGITS, TIE_HUDI_THROTTLE_DIGITS, 0, 0, CK_READOUTS_L, -1, CK_READOUTS_R },
	{ -7, -1, TIE_HUDI_CLOCK_DIGITS, TIE_HUDI_REC_PCT, 0, 0, CK_READOUTS_L, -1, CK_READOUTS_R },
	{ TIE_COCKPIT_PANEL_CMD, -1, -1, -1, 0, 0, CK_CMD, -1, -1 },
	{ TIE_COCKPIT_PANEL_GAUGES_L, -1, -1, -1, 0, 0, CK_GAUGES_L, -1, -1 },
	{ TIE_COCKPIT_PANEL_GAUGES_R, -1, -1, -1, 0, 0, CK_GAUGES_R, -1, -1 },
	{ -3, -1, -1, -1, 0, 0, CK_TOPBAR, -1, -1 },
	/* Unsorted instruments that showed something. */
	{ -2, -1, TIE_HUDI_THREAT_ION, TIE_HUDI_THREAT_HULL_PCT, 0, 1, CK_THREAT, -1, -1 },
	{ -2, -1, TIE_HUDI_THREAT_DIST_KM_INT, TIE_HUDI_THREAT_DIST_KM_FRAC, 0, 1, CK_THREAT, CK_TOPBAR, -1 },
	{ -2, -1, TIE_HUDI_REC_LED, TIE_HUDI_REC_PCT, 0, 1, CK_READOUTS_L, CK_TOPBAR, CK_READOUTS_R },
};

/* Which panel an instrument belongs to (-1 none) and whether only what it
 * drew counts (not its anchor estimate). A rule with group -9 matches any
 * group (used for the subsystem checks). */
static int ck_panel_of(int id, const TieCockpitInstrumentBox* b, float cmd_cu, uint16_t subsystems, int* drawn_only) {
	*drawn_only = 0;
	for (size_t i = 0; i < sizeof k_rules / sizeof k_rules[0]; ++i) {
		const CkRule* r = &k_rules[i];
		if (r->group == -7 ? (b->group < 0 || b->group >= TIE_COCKPIT_PANEL_COUNT)
						   : (r->group != -9 && r->group != b->group))
			continue;
		if (r->slider >= 0 && r->slider != (b->slider ? 1 : 0))
			continue;
		if (r->first >= 0 && (id < r->first || id > r->last))
			continue;
		if (r->needs && !(subsystems & r->needs))
			return -1;
		if (r->group == -9)
			continue; /* only a check */
		if (r->drawn_only && !b->drawn)
			return -1;
		*drawn_only = r->drawn_only;
		const float* box = b->drawn ? b->drawn_uv : b->est;
		if (r->top >= 0 && 0.5f * (box[1] + box[3]) < 0.2f)
			return r->top;
		if (r->right >= 0 && 0.5f * (box[0] + box[2]) > cmd_cu)
			return r->right;
		return r->panel;
	}
	return -1;
}

static uint64_t ck_hash(uint64_t h, const void* data, size_t size) {
	const unsigned char* p = (const unsigned char*)data;
	for (size_t i = 0; i < size; ++i)
		h = (h ^ p[i]) * 0x100000001b3ull;
	return h;
}

/* Shelf packing of the panels (sizes in atlas px times `scale`) into w x h;
 * 1 when all fit. */
static int ck_pack(float src[CK_COUNT][2], const uint8_t use[CK_COUNT], float scale, int w, int h,
				   int out[CK_COUNT][4]) {
	int order[CK_COUNT], n = 0;
	for (int p = 0; p < CK_COUNT; ++p)
		if (use[p])
			order[n++] = p;
	/* Tallest first. */
	for (int i = 1; i < n; ++i)
		for (int j = i; j > 0 && src[order[j]][1] > src[order[j - 1]][1]; --j) {
			const int t = order[j];
			order[j] = order[j - 1];
			order[j - 1] = t;
		}
	int x = CK_PAD_PX, y = CK_PAD_PX, row_h = 0;
	for (int i = 0; i < n; ++i) {
		const int p = order[i];
		const int pw = (int)ceilf(src[p][0] * scale), ph = (int)ceilf(src[p][1] * scale);
		if (pw + 2 * CK_PAD_PX > w || ph + 2 * CK_PAD_PX > h)
			return 0;
		if (x + pw + CK_PAD_PX > w) {
			x = CK_PAD_PX;
			y += row_h + 2 * CK_PAD_PX;
			row_h = 0;
		}
		if (y + ph + CK_PAD_PX > h)
			return 0;
		out[p][0] = x;
		out[p][1] = y;
		out[p][2] = pw;
		out[p][3] = ph;
		x += pw + 2 * CK_PAD_PX;
		if (ph > row_h)
			row_h = ph;
	}
	return 1;
}

int TieVrCockpitHud_RenderContent(AeronCommandBuffer* cmd, const AeronXrStereoFrame* xr,
								  TieCockpitRenderer* cockpit, const TieSnapshot* snap, const int hud_area[4]) {
	ck_init();
	s_ck.content_ready = 0;
	memset(s_ck.visible, 0, sizeof s_ck.visible);
	if (!cmd || !xr || !cockpit || !snap || !xr->hud_target || xr->hud_width <= 0 || xr->hud_height <= 0)
		return 0;
	static TieCockpitInstrumentBox boxes[TIE_MAX_HUD_INSTRUMENTS];
	AeronTexture* atlas = TieCockpitRenderer_InstrumentBoxes(cockpit, boxes, TIE_MAX_HUD_INSTRUMENTS);
	if (!atlas)
		return 0;
	const int atlas_w = Aeron_TextureGetWidth(atlas), atlas_h = Aeron_TextureGetHeight(atlas);
	if (atlas_w <= 0 || atlas_h <= 0)
		return 0;

	/* A new mission, craft or cockpit view starts the latches over. */
	uint64_t key = 0xcbf29ce484222325ull;
	key = ck_hash(key, &snap->mission_load_generation, sizeof snap->mission_load_generation);
	key = ck_hash(key, snap->cockpit.view_name, sizeof snap->cockpit.view_name);
	if (key != s_ck.latch_key) {
		s_ck.latch_key = key;
		memset(s_ck.latched_valid, 0, sizeof s_ck.latched_valid);
	}

	/* The CMD's centre splits the unsorted instruments left/right. */
	float cmd_cu = 0.5f;
	if (boxes[TIE_HUDI_CMD_3D_CRT].valid)
		cmd_cu = 0.5f * (boxes[TIE_HUDI_CMD_3D_CRT].est[0] + boxes[TIE_HUDI_CMD_3D_CRT].est[2]);
	float now[CK_COUNT][4];
	int now_valid[CK_COUNT] = { 0 };
	for (int i = 0; i < TIE_MAX_HUD_INSTRUMENTS; ++i) {
		const TieCockpitInstrumentBox* b = &boxes[i];
		if (!b->valid && !b->drawn)
			continue;
		int drawn_only = 0;
		const int p = ck_panel_of(i, b, cmd_cu, snap->hud.subsystem_active, &drawn_only);
		if (p < 0)
			continue;
		if (b->valid && !drawn_only)
			ck_union(now[p], &now_valid[p], b->est);
		if (b->drawn)
			ck_union(now[p], &now_valid[p], b->drawn_uv);
	}
	/* The CMD's readouts stand out of their anchors' boxes (text): the panel
	 * is made symmetric about the CRT, which is. */
	if (now_valid[CK_CMD] && boxes[TIE_HUDI_CMD_3D_CRT].valid) {
		const float half = fmaxf(cmd_cu - now[CK_CMD][0], now[CK_CMD][2] - cmd_cu);
		now[CK_CMD][0] = cmd_cu - half;
		now[CK_CMD][2] = cmd_cu + half;
	}
	for (int p = 0; p < CK_COUNT; ++p) {
		if (!now_valid[p])
			continue;
		/* The gauges' frames are panel art around what the instruments draw. */
		const float pu = CK_PAD_U + k_pad[p][0], pv = CK_PAD_V + k_pad[p][1];
		float r[4] = { fmaxf(0.0f, now[p][0] - pu), fmaxf(0.0f, now[p][1] - pv), fminf(1.0f, now[p][2] + pu),
					   fminf(1.0f, now[p][3] + pv) };
		if (r[2] - r[0] < 0.004f || r[3] - r[1] < 0.004f)
			continue;
		int latched = s_ck.latched_valid[p];
		ck_union(s_ck.latched[p], &latched, r);
		s_ck.latched_valid[p] = 1;
		s_ck.visible[p] = 1;
	}

	/* The message bar's text, where the cockpit renderer drew it. */
	int msg[4];
	if (TieCockpitRenderer_XrMessageRect(cockpit, snap, msg)) {
		memcpy(s_ck.rect[CK_MSG], msg, sizeof msg);
		s_ck.source[CK_MSG] = 0;
		s_ck.visible[CK_MSG] = 1;
	}
	s_ck.msg_m_per_px = hud_area && hud_area[3] > 0 ? 480.0f / (float)hud_area[3] : 1.0f;

	/* Pack the atlas rectangles into the aux HUD target (or hud_target's rows
	 * above the message bar). */
	AeronRenderTarget* target = xr->hud_aux_target;
	int tw = xr->hud_width, th = xr->hud_height, y0 = 0;
	if (!target) {
		target = xr->hud_target;
		th = hud_area ? hud_area[1] + (int)(0.92f * (float)hud_area[3]) : (int)(0.9f * (float)th);
	}
	float src[CK_COUNT][2];
	uint8_t use[CK_COUNT] = { 0 };
	int any = 0;
	for (int p = 0; p < CK_COUNT; ++p) {
		src[p][0] = src[p][1] = 0.0f;
		if (p == CK_MSG || !s_ck.visible[p])
			continue;
		src[p][0] = (s_ck.latched[p][2] - s_ck.latched[p][0]) * (float)atlas_w;
		src[p][1] = (s_ck.latched[p][3] - s_ck.latched[p][1]) * (float)atlas_h;
		use[p] = 1;
		any = 1;
	}
	if (memcmp(src, s_ck.pack_src, sizeof src) != 0 || tw != s_ck.pack_w || th != s_ck.pack_h ||
		y0 != s_ck.pack_y0) {
		memcpy(s_ck.pack_src, src, sizeof src);
		s_ck.pack_w = tw;
		s_ck.pack_h = th;
		s_ck.pack_y0 = y0;
		s_ck.pack_scale = 0.0f;
		/* As large as fits, up to 1.25 atlas px per target px. */
		for (float scale = 1.25f; scale > 0.1f; scale *= 0.92f) {
			if (ck_pack(src, use, scale, tw, th, s_ck.packed)) {
				s_ck.pack_scale = scale;
				break;
			}
		}
	}
	if (any && s_ck.pack_scale <= 0.0f)
		any = 0;
	for (int p = 0; p < CK_COUNT; ++p) {
		if (p == CK_MSG)
			continue;
		if (!any || !use[p]) {
			s_ck.visible[p] = 0;
			continue;
		}
		memcpy(s_ck.rect[p], s_ck.packed[p], sizeof s_ck.rect[p]);
		s_ck.source[p] = target == xr->hud_aux_target ? 1 : 0;
	}

	if (any) {
		if (!s_ck.list)
			s_ck.list = AeronDrawList_Create(64);
		if (!s_ck.list)
			return 0;
		AeronDrawList_Begin(s_ck.list, NULL, xr->hud_width, xr->hud_height, AERON_DRAWLIST2D_LOAD, NULL);
		for (int p = 0; p < CK_COUNT; ++p) {
			if (p == CK_MSG || !s_ck.visible[p])
				continue;
			const int* r = s_ck.rect[p];
			AeronDrawList2DSprite sprite;
			memset(&sprite, 0, sizeof sprite);
			sprite.texture = atlas;
			sprite.src_u0 = s_ck.latched[p][0];
			sprite.src_v0 = s_ck.latched[p][1];
			sprite.src_u1 = s_ck.latched[p][2];
			sprite.src_v1 = s_ck.latched[p][3];
			sprite.dst_x = (float)r[0];
			sprite.dst_y = (float)r[1];
			sprite.dst_w = (float)r[2];
			sprite.dst_h = (float)r[3];
			sprite.tint[0] = sprite.tint[1] = sprite.tint[2] = sprite.tint[3] = 1.0f;
			sprite.blend = AERON_BLIT2D_BLEND_NONE;
			sprite.filter = AERON_BLIT2D_FILTER_LINEAR;
			AeronDrawList_AddSprite(s_ck.list, &sprite);
			/* A frame around the panel a hand is on or holds. */
			const int held = s_ck.grab_panel[0] == p || s_ck.grab_panel[1] == p;
			const int hover = s_ck.hover_panel[0] == p || s_ck.hover_panel[1] == p;
			if (held || hover) {
				const float a = held ? 0.95f : 0.7f;
				const float c[4] = { 0.30f * a, 1.0f * a, 0.45f * a, a };
				AeronDrawList_AddFrame(s_ck.list, (float)r[0], (float)r[1], (float)r[2], (float)r[3],
									   held ? 3.0f : 2.0f, c, AERON_BLIT2D_BLEND_PMA, NULL);
			}
		}
		if (!AeronDrawList_Prepare(s_ck.list, cmd))
			return 0;
		const int aux = target == xr->hud_aux_target;
		AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
			.color_target = target,
			.clear_color = aux,
			.clear_color_rgba = { 0.0f, 0.0f, 0.0f, 0.0f },
			.command_buffer = cmd,
			.debug_label = "TIE VR cockpit HUD panels",
		});
		if (!pass)
			return 0;
		AeronDrawList_RenderIntoPass(s_ck.list, cmd, pass, target);
		Aeron_EndRenderPass(pass);
	}

	char log[512];
	int log_len = 0;
	for (int p = 0; p < CK_COUNT && log_len < (int)sizeof log - 64; ++p)
		if (s_ck.visible[p] && p != CK_MSG)
			log_len += snprintf(log + log_len, sizeof log - (size_t)log_len, "%s%s %dx%d@%d,%d", log_len ? "; " : "",
								k_ids[p], s_ck.rect[p][2], s_ck.rect[p][3], s_ck.rect[p][0], s_ck.rect[p][1]);
	if (log_len && strcmp(log, s_ck.logged) != 0) {
		memcpy(s_ck.logged, log, sizeof s_ck.logged);
		Aeron_LogInfo("tie.vr", "VR cockpit HUD panels (%s %dx%d, atlas %dx%d, x%.2f): %s",
					  target == xr->hud_aux_target ? "aux" : "hud", tw, th, atlas_w, atlas_h, (double)s_ck.pack_scale,
					  log);
	}
	s_ck.content_ready = any || s_ck.visible[CK_MSG];
	return s_ck.content_ready;
}

/* ---- placement and grabbing ---- */

static void ck_apply_custom(AeronXrPanel* panel, int p) {
	memcpy(panel->position, s_ck.cpos[p], sizeof panel->position);
	memcpy(panel->orientation, s_ck.cq[p], sizeof panel->orientation);
}

/* Squeezing the grip with a hand on a panel picks it up; it follows the hand
 * until the grip is let go, then the layout is saved for this cockpit. */
static void ck_update_grab(const AeronXrPanel* out, const int* index_of) {
	static const float ux[3] = { 1, 0, 0 }, uy[3] = { 0, 1, 0 }, uz[3] = { 0, 0, 1 };
	for (int hand = 0; hand < 2; ++hand) {
		AeronXrHand xh;
		const int was_hover = s_ck.hover_panel[hand];
		s_ck.hover_panel[hand] = -1;
		if (!Aeron_XrGetHand(hand, &xh) || !xh.valid) {
			if (s_ck.grab_panel[hand] >= 0) {
				s_ck.grab_panel[hand] = -1;
				ck_save_layout();
			}
			s_ck.grip_prev[hand] = 0.0f;
			continue;
		}
		/* The flight stick (or a VR panel) already has this grip. */
		const int taken = s_ck.grab_panel[hand] < 0 && TieVrHands_GripCaptured(hand);
		int best = -1;
		float best_depth = 1e9f;
		for (int p = 0; p < CK_COUNT && !taken; ++p) {
			if (index_of[p] < 0)
				continue;
			const AeronXrPanel* panel = &out[index_of[p]];
			float ax[3], ay[3], az[3];
			ck_rotate(panel->orientation, ux, ax);
			ck_rotate(panel->orientation, uy, ay);
			ck_rotate(panel->orientation, uz, az);
			const float rel[3] = { xh.position[0] - panel->position[0], xh.position[1] - panel->position[1],
								   xh.position[2] - panel->position[2] };
			const float lx = rel[0] * ax[0] + rel[1] * ax[1] + rel[2] * ax[2];
			const float ly = rel[0] * ay[0] + rel[1] * ay[1] + rel[2] * ay[2];
			const float depth = rel[0] * az[0] + rel[1] * az[1] + rel[2] * az[2];
			if (fabsf(lx) < 0.5f * panel->size[0] + CK_GRAB_MARGIN_M &&
				fabsf(ly) < 0.5f * panel->size[1] + CK_GRAB_MARGIN_M && fabsf(depth) < CK_GRAB_DEPTH_M &&
				fabsf(depth) < best_depth) {
				best = p;
				best_depth = fabsf(depth);
			}
		}
		if (best >= 0 && best != s_ck.grab_panel[1 - hand])
			s_ck.hover_panel[hand] = best;
		const int held = s_ck.grab_panel[hand];
		if (held >= 0) {
			if (xh.grip < CK_GRIP_OFF) {
				s_ck.grab_panel[hand] = -1;
				Aeron_XrHapticPulse(hand, 0.3f, 20);
				ck_save_layout();
			} else {
				float off[3];
				ck_rotate(xh.orientation, s_ck.grab_pos[hand], off);
				for (int k = 0; k < 3; ++k)
					s_ck.cpos[held][k] = xh.position[k] + off[k];
				ck_qmul(xh.orientation, s_ck.grab_q[hand], s_ck.cq[held]);
				ck_qnorm(s_ck.cq[held]);
				s_ck.custom[held] = 1;
			}
		} else if (s_ck.hover_panel[hand] >= 0 && xh.grip >= CK_GRIP_ON && s_ck.grip_prev[hand] < CK_GRIP_OFF) {
			const int p = s_ck.hover_panel[hand];
			const AeronXrPanel* panel = &out[index_of[p]];
			const float inv[4] = { -xh.orientation[0], -xh.orientation[1], -xh.orientation[2], xh.orientation[3] };
			const float to_panel[3] = { panel->position[0] - xh.position[0], panel->position[1] - xh.position[1],
										panel->position[2] - xh.position[2] };
			ck_rotate(inv, to_panel, s_ck.grab_pos[hand]);
			ck_qmul(inv, panel->orientation, s_ck.grab_q[hand]);
			ck_qnorm(s_ck.grab_q[hand]);
			memcpy(s_ck.cpos[p], panel->position, sizeof s_ck.cpos[p]);
			memcpy(s_ck.cq[p], panel->orientation, sizeof s_ck.cq[p]);
			s_ck.custom[p] = 1;
			s_ck.grab_panel[hand] = p;
			Aeron_XrHapticPulse(hand, 0.6f, 30);
			Aeron_LogInfo("tie.vr", "VR HUD: %s hand took %s", hand ? "right" : "left", k_ids[p]);
		} else if (s_ck.hover_panel[hand] >= 0 && s_ck.hover_panel[hand] != was_hover) {
			Aeron_XrHapticPulse(hand, 0.15f, 10); /* a light tick: on a panel */
		}
		s_ck.grip_prev[hand] = xh.grip;
		/* The grip moves the panel, not the game (no grip layer, wheel or
		 * target-in-sights while the hand is on a panel). */
		if (s_ck.hover_panel[hand] >= 0 || s_ck.grab_panel[hand] >= 0)
			TieVrHands_CaptureGrip(hand);
	}
}

int TieVrCockpitHud_Build(const AeronXrStereoFrame* xr, const char* cockpit_name, AeronXrPanel* out, int max_out) {
	ck_init();
	if (s_ck.grab_panel[0] < 0 && s_ck.grab_panel[1] < 0)
		ck_read_layout(cockpit_name);
	if (!xr || !out || max_out <= 0 || !s_ck.content_ready || !s_ck.spec)
		return 0;
	const char* ws = SDL_getenv("XWA_XR_WORLD_SCALE");
	float world_scale = ws && ws[0] ? (float)atof(ws) : 1.0f;
	if (!(world_scale > 0.05f))
		world_scale = 1.0f;
	int index_of[CK_COUNT];
	int count = 0;
	for (int p = 0; p < CK_COUNT; ++p) {
		index_of[p] = -1;
		const int* r = s_ck.rect[p];
		if (!s_ck.visible[p] || r[2] < 2 || r[3] < 2 || count >= max_out)
			continue;
		const CkSpec* spec = &s_ck.spec[p];
		AeronXrPanel* panel = &out[count];
		memset(panel, 0, sizeof *panel);
		memcpy(panel->rect, r, sizeof panel->rect);
		panel->source = s_ck.source[p];
		/* As big as in TIE's 640x480 cockpit times the panel's scale, capped
		 * by its box. */
		float cw, ch;
		if (p == CK_MSG) {
			cw = (float)r[2] * s_ck.msg_m_per_px;
			ch = (float)r[3] * s_ck.msg_m_per_px;
		} else {
			cw = (s_ck.latched[p][2] - s_ck.latched[p][0]) * 640.0f;
			ch = (s_ck.latched[p][3] - s_ck.latched[p][1]) * 480.0f;
		}
		float size_w = cw * spec->m_per_px, size_h = ch * spec->m_per_px;
		if (spec->box_w > 0.0f && size_w > spec->box_w) {
			size_h *= spec->box_w / size_w;
			size_w = spec->box_w;
		}
		if (spec->box_h > 0.0f && size_h > spec->box_h) {
			size_w *= spec->box_h / size_h;
			size_h = spec->box_h;
		}
		panel->size[0] = size_w / world_scale;
		panel->size[1] = size_h / world_scale;
		ck_pose(spec, world_scale, panel);
		if (s_ck.custom[p])
			ck_apply_custom(panel, p);
		index_of[p] = count++;
	}
	ck_update_grab(out, index_of);
	/* Show a held panel where the hand put it this frame. */
	for (int hand = 0; hand < 2; ++hand) {
		const int p = s_ck.grab_panel[hand];
		if (p >= 0 && index_of[p] >= 0)
			ck_apply_custom(&out[index_of[p]], p);
	}
	return count;
}

void TieVrCockpitHud_Idle(void) {
	ck_init();
	s_ck.content_ready = 0;
	for (int hand = 0; hand < 2; ++hand) {
		s_ck.hover_panel[hand] = -1;
		if (s_ck.grab_panel[hand] >= 0) {
			s_ck.grab_panel[hand] = -1;
			ck_save_layout();
		}
	}
}

void TieVrCockpitHud_Shutdown(void) {
	if (s_ck.list)
		AeronDrawList_Destroy(s_ck.list);
	memset(&s_ck, 0, sizeof s_ck);
}

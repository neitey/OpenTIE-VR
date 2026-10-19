/* VR lap console, quick wheel and side panel; see vr_console.h. Port of
 * OpenXWA-VR's xwa_remaster/vr_lap_console.c: the panel framework
 * (placement, grab handles, poke and ray, cached panel images) is theirs,
 * the content is TIE Fighter's. */

#include "tie_remaster/flight/vr_console.h"
#include "tie_remaster/vr/vr_hands.h"

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "aeron/scene/draw_list2d.h"
#include "aeron/scene/font_atlas.h"
#include "aeron/scene/text2d.h"
#include "aeron/vfs.h"
#include <SDL3/SDL_stdinc.h>
#include "tie_runtime/input/actions.h"
#include "tie_runtime/input/vr_buttons.h"
#include "tie_runtime/input/vr_controls.h"
#include "tie_runtime/snapshot/snapshot.h"
#include "tie_runtime/species_id.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Placement -------------------------------------------------------- */

/* Default spot: straight ahead, 52 degrees below eye level, 0.52 m from
 * the eyes (over the lap, within easy reach), facing the eyes. 51 x 17 cm. */
#define VC_PITCH_DEG -52.0f
#define VC_DISTANCE_M 0.52f
#define VC_WIDTH_M 0.51f
#define VC_HEIGHT_M 0.17f

/* Look-at wake: awake when the gaze comes within WAKE degrees of the
 * console's centre, asleep again beyond SLEEP. */
#define VC_WAKE_DEG 32.0f
#define VC_SLEEP_DEG 40.0f
#define VC_SLEEP_ALPHA 0.22f

/* Grab zones: the left and right ends of a panel. */
#define VC_GRAB_FORGIVE (1.15f * 1.10f)
#define VC_GRAB_X_M (0.06f * VC_GRAB_FORGIVE)
#define VC_GRAB_Y_M (0.05f * VC_GRAB_FORGIVE)
#define VC_GRAB_FRONT_M (0.09f * VC_GRAB_FORGIVE)
#define VC_GRAB_BACK_M (0.07f * VC_GRAB_FORGIVE)
/* Side panel: 40 x 30 cm, to the right, just out of view. */
#define VC_CARD_W_M 0.40f
#define VC_CARD_H_M 0.30f
#define VC_CARD_YAW_DEG 80.0f
#define VC_CARD_PITCH_DEG -12.0f
#define VC_CARD_DISTANCE_M 0.60f
#define VC_CARD_WAKE_DEG 30.0f
#define VC_CARD_SLEEP_DEG 38.0f

#define VC_GRIP_ON 0.6f
#define VC_GRIP_OFF 0.35f

/* Poke: the controller tip is TIP metres ahead of the aim pose; a press is
 * PRESS metres through the surface, released when back above RELEASE.
 * Deeper than BLOCK (or coming from behind) is not a press. */
#define VC_TIP_M 0.02f
#define VC_HOVER_M 0.12f
#define VC_PRESS_M 0.006f
#define VC_RELEASE_M 0.002f
#define VC_BLOCK_M 0.08f
#define VC_RAY_MAX_M 1.5f

#define VC_HOLD_S 0.7f
#define VC_FLASH_S 0.15f

#define VC_MAX_BUTTONS 72
#define VC_MAX_LABELS 24
#define VC_ACTION_QUEUE 32

#define VC_PI 3.14159265f

typedef struct VcButton {
	float x, y, w, h;
	const char* label;
	const char* sub; /* small state line, or NULL */
	TieVrConsoleAction action;
	int arg;
	/* Holding the button (VC_HOLD_S) does hold_action / hold_arg instead. */
	TieVrConsoleAction hold_action;
	int hold_arg;
	int enabled;
	int lit;
	int level;       /* level cell: >0 when the cell is at or below the current level */
	float font_size; /* 0: the default for its height */
	float radius;    /* corner radius, 0: the default */
	float base_fill; /* the fill's alpha at rest at least this (the wheel) */
} VcButton;

typedef struct VcLabel {
	float x, y;
	const char* text;
	int size;
	int align;
	int section;
	int dim;
} VcLabel;

typedef struct VcHand {
	int poke_down;
	int blocked;
	int trigger_down;
	int press_button;
	float press_time;
	int hold_fired;
	int cursor;      /* 0 none, 1 poke hover, 2 ray */
	float cx, cy;    /* cursor in console pixels */
	int over_button; /* button under the cursor, -1 none */
	float ray_start[3], ray_end[3];
	int captured;
} VcHand;

/* A panel the player can pick up by its ends and put anywhere. */
typedef struct VcPlace {
	int custom; /* placed by hand (else the default spot) */
	float cpos[3], cq[4];
	int grab_hand; /* hand + 1, 0 = not held */
	int grab_side;
	float grab_pos[3], grab_q[4]; /* the panel relative to the holding hand */
	int hover_side[2];
	float grip_prev[2];
} VcPlace;

/* The side panel. */
typedef struct VcCard {
	VcPlace place;
	AeronDrawList2D* list[2];
	int awake;
	float alpha;
	unsigned int mission;
	int drawn;
	int logged;
	uint64_t hash;
	int hash_valid;
} VcCard;

static struct {
	float dt;
	float wall_dt; /* the frame's real length (slow frames too), for the wheel's delay */
	int drawn;
	TieVrConsoleState state;
	int awake;
	float alpha;

	VcButton buttons[VC_MAX_BUTTONS];
	int button_count;
	VcLabel labels[VC_MAX_LABELS];
	int label_count;
	float flash[VC_MAX_BUTTONS];

	VcHand hands[2];

	TieVrConsoleAction queue_action[VC_ACTION_QUEUE];
	int queue_arg[VC_ACTION_QUEUE];
	int queue_head, queue_count;

	AeronDrawList2D* list;
	AeronDrawList2D* list_r;
	AeronFontAtlas font;
	int font_state; /* 0 not tried, 1 loaded, -1 failed */
	int logged;
	VcPlace lap;
	VcCard card;
	char pose_env[160];
	int placement_dirty;
	char placement[160];
	/* The panel being drawn: its list and its size in pixels. */
	AeronDrawList2D* cur_list;
	const VcPlace* cur_place;
	float cur_w_px, cur_h_px;
	uint64_t hash;
	int hash_valid;
	/* Grips the panels own this frame (handles reached), applied in
	 * EndFrame; last frame's (a squeeze there is a grab, not the wheel). */
	int grip_capture[2];
	int grip_capture_prev[2];
	float clock; /* seconds, for pulsing */
	float hud[3], hud_dark[3], hud_pale[3];
	/* The voice tab's list (tie_app). */
	int (*voice_count)(void);
	const char* (*voice_label)(int);
	const char* (*voice_phrase)(int, int);
} s_vc;

/* The quick wheel. */
#define VW_MAX_ITEMS 10
#define VW_CENTER (-2) /* hover: the centre button */
static struct {
	int open;
	int hand;      /* physical hand (0 left, 1 right) */
	int grip_held; /* its grip, with hysteresis */
	float held_s;  /* how long it has been held (not yet open) */
	int vetoed;    /* a chord used this squeeze: no wheel until it is let go */
	uint32_t serial; /* vr_controls' grip-layer uses when the squeeze began */
	int level;     /* 0 the four categories, else 1 + the category */
	int hover;     /* item, VW_CENTER or -1 */
	int armed;     /* the trigger was let go since the last pick */
	int center_lock;
	int drawn;
	float anim;
	float fwd[3];
	float center[3], ax[3], ay[3], az[3], q[4];
	int hit;
	float hit_px, hit_py;
	float ray_start[3], ray_end[3];
	VcButton items[VW_MAX_ITEMS];
	int item_count;
	VcButton center_button;
	float flash[VW_MAX_ITEMS + 1];
	AeronDrawList2D* list[2];
	uint64_t hash;
	int hash_valid;
	int logged;
} s_vw;

static void vw_close(void);

/* ---- Small math ------------------------------------------------------- */

static void vc_rotate(const float q[4], const float v[3], float out[3]) {
	const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
	const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
	const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
	out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
	out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
	out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

static float vc_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void vc_qmul(const float a[4], const float b[4], float out[4]) {
	const float r[4] = {
		a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
		a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
		a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
		a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
	};
	memcpy(out, r, sizeof r);
}

static void vc_qnorm(float q[4]) {
	const float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (l > 1e-6f) {
		for (int k = 0; k < 4; ++k)
			q[k] /= l;
	} else {
		q[0] = q[1] = q[2] = 0.0f;
		q[3] = 1.0f;
	}
}

static float vc_env(const char* name, float fallback) {
	const char* v = SDL_getenv(name);
	return v && v[0] ? (float)atof(v) : fallback;
}

static int vc_env_off(const char* name) {
	const char* v = SDL_getenv(name);
	return v && (v[0] == '0' || v[0] == 'o' || v[0] == 'O');
}

/* The hand-placed pose from XWA_XR_CONSOLE_POSE ("x,y,z,qx,qy,qz,qw";
 * empty = the default spot), read again whenever it changes. */
static void vc_read_pose_env(void) {
	const char* v = SDL_getenv("XWA_XR_CONSOLE_POSE");
	if (!v)
		v = "";
	if (strcmp(v, s_vc.pose_env) == 0)
		return;
	snprintf(s_vc.pose_env, sizeof s_vc.pose_env, "%s", v);
	float p[3], q[4];
	s_vc.lap.custom = sscanf(v, "%f,%f,%f,%f,%f,%f,%f", &p[0], &p[1], &p[2], &q[0], &q[1], &q[2], &q[3]) == 7 &&
					  fabsf(p[0]) < 2.0f && fabsf(p[1]) < 2.0f && fabsf(p[2]) < 2.0f;
	if (s_vc.lap.custom) {
		memcpy(s_vc.lap.cpos, p, sizeof p);
		vc_qnorm(q);
		memcpy(s_vc.lap.cq, q, sizeof q);
	}
}

/* Where the player put a panel: 1 with its pose and axes. */
static int vc_place_pose(const VcPlace* pl, float w, float h, AeronXrPanel* out, float axis_x[3], float axis_y[3],
						 float axis_z[3]) {
	static const float ux[3] = { 1, 0, 0 }, uy[3] = { 0, 1, 0 }, uz[3] = { 0, 0, 1 };
	if (!pl->custom)
		return 0;
	memcpy(out->position, pl->cpos, sizeof pl->cpos);
	memcpy(out->orientation, pl->cq, sizeof pl->cq);
	vc_rotate(pl->cq, ux, axis_x);
	vc_rotate(pl->cq, uy, axis_y);
	vc_rotate(pl->cq, uz, axis_z);
	out->size[0] = w;
	out->size[1] = h;
	return 1;
}

/* Rotation with columns x, y, z -> quaternion. */
static void vc_basis_quat(const float x[3], const float y[3], const float z[3], float q[4]) {
	const float trace = x[0] + y[1] + z[2];
	if (trace > 0.0f) {
		const float t = sqrtf(trace + 1.0f) * 2.0f;
		q[3] = 0.25f * t;
		q[0] = (y[2] - z[1]) / t;
		q[1] = (z[0] - x[2]) / t;
		q[2] = (x[1] - y[0]) / t;
	} else if (x[0] > y[1] && x[0] > z[2]) {
		const float t = sqrtf(1.0f + x[0] - y[1] - z[2]) * 2.0f;
		q[3] = (y[2] - z[1]) / t;
		q[0] = 0.25f * t;
		q[1] = (y[0] + x[1]) / t;
		q[2] = (z[0] + x[2]) / t;
	} else if (y[1] > z[2]) {
		const float t = sqrtf(1.0f + y[1] - x[0] - z[2]) * 2.0f;
		q[3] = (z[0] - x[2]) / t;
		q[0] = (y[0] + x[1]) / t;
		q[1] = 0.25f * t;
		q[2] = (z[1] + y[2]) / t;
	} else {
		const float t = sqrtf(1.0f + z[2] - x[0] - y[1]) * 2.0f;
		q[3] = (x[1] - y[0]) / t;
		q[0] = (z[0] + x[2]) / t;
		q[1] = (z[1] + y[2]) / t;
		q[2] = 0.25f * t;
	}
}

/* A quad `d` metres from the seated eye point at yaw (+ right) / pitch
 * (+ up) degrees, facing it, X kept level. */
static void vc_facing_pose(float yaw_deg, float pitch_deg, float d, float w, float h, AeronXrPanel* out,
						   float axis_x[3], float axis_y[3], float axis_z[3]) {
	const float deg = VC_PI / 180.0f;
	const float yaw = yaw_deg * deg, pitch = pitch_deg * deg;
	const float dir[3] = { sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch) };
	for (int k = 0; k < 3; ++k)
		out->position[k] = dir[k] * d;
	const float z[3] = { -dir[0], -dir[1], -dir[2] };
	float x[3] = { z[2], 0.0f, -z[0] };
	const float xl = sqrtf(x[0] * x[0] + x[2] * x[2]);
	x[0] /= xl;
	x[2] /= xl;
	const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
	memcpy(axis_x, x, sizeof x);
	memcpy(axis_y, y, sizeof y);
	memcpy(axis_z, z, sizeof z);
	vc_basis_quat(x, y, z, out->orientation);
	out->size[0] = w;
	out->size[1] = h;
}

/* The lap console: facing the seated eye point, X kept level; or where the
 * player put it. */
static void vc_pose(AeronXrPanel* out, float axis_x[3], float axis_y[3], float axis_z[3]) {
	if (vc_place_pose(&s_vc.lap, VC_WIDTH_M, VC_HEIGHT_M, out, axis_x, axis_y, axis_z))
		return;
	vc_facing_pose(0.0f, vc_env("XWA_XR_CONSOLE_PITCH", VC_PITCH_DEG), vc_env("XWA_XR_CONSOLE_DISTANCE", VC_DISTANCE_M),
				   VC_WIDTH_M, VC_HEIGHT_M, out, axis_x, axis_y, axis_z);
}

/* ---- Actions ---------------------------------------------------------- */

static void vc_push(TieVrConsoleAction action, int arg) {
	if (action == TIE_VRC_NONE || s_vc.queue_count >= VC_ACTION_QUEUE)
		return;
	const int i = (s_vc.queue_head + s_vc.queue_count) % VC_ACTION_QUEUE;
	s_vc.queue_action[i] = action;
	s_vc.queue_arg[i] = arg;
	s_vc.queue_count++;
}

int TieVrConsole_PollAction(TieVrConsoleAction* action, int* arg) {
	if (s_vc.queue_count <= 0)
		return 0;
	if (action)
		*action = s_vc.queue_action[s_vc.queue_head];
	if (arg)
		*arg = s_vc.queue_arg[s_vc.queue_head];
	s_vc.queue_head = (s_vc.queue_head + 1) % VC_ACTION_QUEUE;
	s_vc.queue_count--;
	return 1;
}

/* ---- Layout ----------------------------------------------------------- */

static VcButton* vc_button(float x, float y, float w, float h, const char* label, TieVrConsoleAction action,
						   int arg) {
	if (s_vc.button_count >= VC_MAX_BUTTONS) {
		static VcButton spare;
		memset(&spare, 0, sizeof spare);
		return &spare;
	}
	VcButton* b = &s_vc.buttons[s_vc.button_count++];
	memset(b, 0, sizeof *b);
	b->x = x;
	b->y = y;
	b->w = w;
	b->h = h;
	b->label = label;
	b->action = action;
	b->arg = arg;
	b->enabled = 1;
	return b;
}

/* A button that sends one of TIE's input actions. */
static VcButton* vc_key(float x, float y, float w, float h, const char* label, TieInputAction action) {
	return vc_button(x, y, w, h, label, TIE_VRC_INPUT, (int)action);
}

static void vc_label(float x, float y, const char* text, int size, int align, int section, int dim) {
	if (s_vc.label_count >= VC_MAX_LABELS)
		return;
	VcLabel* l = &s_vc.labels[s_vc.label_count++];
	l->x = x;
	l->y = y;
	l->text = text;
	l->size = size;
	l->align = align;
	l->section = section;
	l->dim = dim;
}

/* A row of equal input-action buttons across [x, x + w). */
static void vc_key_row(float x, float y, float w, float h, int n, const char* const* labels,
					   const TieInputAction* actions, VcButton** out) {
	const float gap = 10.0f;
	const float bw = (w - gap * (float)(n - 1)) / (float)n;
	for (int i = 0; i < n; ++i) {
		VcButton* b = vc_key(x + (bw + gap) * (float)i, y, bw, h, labels[i], actions[i]);
		if (out)
			out[i] = b;
	}
}

/* Five level cells (0 off .. 4 max) after a row label. */
static void vc_levels(float x, float y, float w, float h, const char* name, TieVrConsoleAction action, int level,
					  int enabled) {
	static const char* const cells[5] = { "OFF", "LOW", "NRM", "HI", "MAX" };
	const float label_w = 128.0f;
	const float gap = 8.0f;
	const float cw = (w - label_w - gap * 4.0f) / 5.0f;
	vc_label(x, y + h * 0.5f - 12.0f, name, 22, 0, 0, !enabled);
	for (int i = 0; i < 5; ++i) {
		VcButton* b = vc_button(x + label_w + (cw + gap) * (float)i, y, cw, h, cells[i], action, i);
		b->enabled = enabled;
		b->level = enabled && level >= i && level > 0 ? 1 : 0;
		b->lit = enabled && level == i;
	}
}

#define VC_W 1536.0f
#define VC_H 512.0f

/* TIE's lap console: ENERGY | TARGETING / WEAPONS | WINGMEN / SHIP. */
static void vc_layout(void) {
	const TieVrConsoleState* st = &s_vc.state;
	VcButton* row[5];
	s_vc.button_count = 0;
	s_vc.label_count = 0;

	/* Energy */
	{
		const float x = 34.0f, w = 470.0f;
		vc_label(x, 22.0f, "ENERGY", 28, 0, 1, 0);
		vc_levels(x, 64.0f, w, 52.0f, "LASERS", TIE_VRC_LASER_LEVEL, st->laser_level, st->has_lasers);
		vc_levels(x, 124.0f, w, 52.0f, "SHIELDS", TIE_VRC_SHIELD_LEVEL, st->shield_level, st->has_shields);
		vc_levels(x, 184.0f, w, 52.0f, "BEAM", TIE_VRC_BEAM_LEVEL, st->beam_level, st->beam_on);
		vc_label(x, 250.0f, "SHIELD DIRECTION", 20, 0, 0, !st->has_shields);
		{
			static const char* const l[3] = { "FRONT", "EVEN", "REAR" };
			const float bw = (w - 20.0f) / 3.0f;
			for (int i = 0; i < 3; ++i) {
				VcButton* b = vc_button(x + (bw + 10.0f) * (float)i, 276.0f, bw, 54.0f, l[i], TIE_VRC_SHIELD_MODE, i);
				b->enabled = st->has_shields;
				b->lit = st->has_shields && st->shield_mode == i;
			}
		}
		{
			static const char* const l[3] = { "LASERS TO\nSHIELDS", "SHIELDS TO\nLASERS", "BEAM\nON / OFF" };
			static const TieInputAction a[3] = { TIE_INPUT_ACTION_XFER_CANNON_TO_SHIELDS,
												 TIE_INPUT_ACTION_XFER_SHIELDS_TO_CANNON, TIE_INPUT_ACTION_TOGGLE_BEAM };
			vc_key_row(x, 340.0f, w, 56.0f, 3, l, a, row);
			row[0]->enabled = row[1]->enabled = st->has_shields;
			row[2]->enabled = st->has_beam;
			row[2]->lit = st->beam_on;
		}
		/* Rows 410..458: the gauges (vc_draw_gauges). */
	}

	/* Targeting and weapons */
	{
		const float x = 538.0f, w = 460.0f;
		vc_label(x, 22.0f, "TARGETING", 28, 0, 1, 0);
		{
			static const char* const l[4] = { "NEXT", "PREVIOUS", "NEAREST\nFIGHTER", "MY\nATTACKER" };
			static const TieInputAction a[4] = { TIE_INPUT_ACTION_TARGET_NEXT, TIE_INPUT_ACTION_TARGET_PREV,
												 TIE_INPUT_ACTION_TARGET_NEAREST_FIGHTER_OR_MINE,
												 TIE_INPUT_ACTION_TARGET_MY_ATTACKER };
			vc_key_row(x, 62.0f, w, 66.0f, 4, l, a, NULL);
		}
		{
			static const char* const l[4] = { "ATTACKING\nMY TARGET", "NEWEST", "IN SIGHTS", "CLEAR" };
			static const TieInputAction a[4] = { TIE_INPUT_ACTION_TARGET_ATTACKER_CHAIN,
												 TIE_INPUT_ACTION_TARGET_NEWEST_CRAFT, TIE_INPUT_ACTION_AUTO_TARGET,
												 TIE_INPUT_ACTION_TARGET_CLEAR };
			vc_key_row(x, 136.0f, w, 48.0f, 4, l, a, row);
			row[0]->enabled = row[3]->enabled = st->has_target;
		}
		{
			VcButton* prev = vc_key(x, 192.0f, 70.0f, 48.0f, "<", TIE_INPUT_ACTION_TARGET_COMPONENT_PREV);
			VcButton* next = vc_key(x + w - 70.0f, 192.0f, 70.0f, 48.0f, ">", TIE_INPUT_ACTION_TARGET_COMPONENT_NEXT);
			prev->enabled = next->enabled = st->has_target;
			vc_label(x + w * 0.5f, 202.0f, "COMPONENT", 22, 1, 0, !st->has_target);
		}
		{
			vc_label(x, 251.0f, "TARGET", 20, 0, 0, 0);
			vc_label(x, 273.0f, "MEMORY", 20, 0, 0, 0);
			static const char* const l[3] = { "F5", "F6", "F7" };
			static const TieInputAction recall[3] = { TIE_INPUT_ACTION_TARGET_PRESET_RECALL_1,
													  TIE_INPUT_ACTION_TARGET_PRESET_RECALL_2,
													  TIE_INPUT_ACTION_TARGET_PRESET_RECALL_3 };
			static const TieInputAction store[3] = { TIE_INPUT_ACTION_TARGET_PRESET_STORE_1,
													 TIE_INPUT_ACTION_TARGET_PRESET_STORE_2,
													 TIE_INPUT_ACTION_TARGET_PRESET_STORE_3 };
			const float bw = (w - 110.0f - 20.0f) / 3.0f;
			for (int i = 0; i < 3; ++i) {
				VcButton* b = vc_key(x + 110.0f + (bw + 10.0f) * (float)i, 248.0f, bw, 48.0f, l[i], recall[i]);
				b->hold_action = TIE_VRC_INPUT;
				b->hold_arg = (int)store[i];
				b->lit = st->preset_set[i];
				b->sub = st->preset_set[i] ? "recall / hold: store" : "hold: store target";
			}
		}
		/* Weapons: its own section under the targeting computer. */
		{
			vc_label(x, 318.0f, "WEAPONS", 28, 0, 1, 0);
			static const char* const l[3] = { "CHANGE\nWEAPON", "FIRING\nMODE", "MATCH\nSPEED" };
			static const TieInputAction a[3] = { TIE_INPUT_ACTION_CYCLE_WEAPON_GROUP,
												 TIE_INPUT_ACTION_CYCLE_WEAPON_FIRING_MODE,
												 TIE_INPUT_ACTION_MATCH_SPEED };
			vc_key_row(x, 356.0f, w, 60.0f, 3, l, a, row);
			row[2]->enabled = st->has_target;
		}
	}

	/* Wingmen and ship */
	{
		const float x = 1032.0f, w = 470.0f;
		vc_label(x, 22.0f, "WINGMEN", 28, 0, 1, 0);
		{
			/* Shift+A / C / I: the player's own flight group. */
			static const char* const l[3] = { "ATTACK\nMY TARGET", "COVER ME", "IGNORE\nMY TARGET" };
			static const TieInputAction a[3] = { TIE_INPUT_ACTION_COMM_ASSIGN_TARGET, TIE_INPUT_ACTION_COMM_COVER_ME,
												 TIE_INPUT_ACTION_COMM_IGNORE_TARGET };
			vc_key_row(x, 60.0f, w, 60.0f, 3, l, a, row);
			row[0]->enabled = row[2]->enabled = st->has_target;
		}
		{
			/* Shift+E / W / G / H / R: the targeted wingman. */
			vc_label(x, 128.0f, "TARGETED WINGMAN", 18, 0, 0, !st->has_target);
			static const char* const l[5] = { "EVADE", "WAIT", "GO AHEAD", "GO HOME", "REPORT" };
			static const TieInputAction a[5] = { TIE_INPUT_ACTION_COMM_EVADE, TIE_INPUT_ACTION_COMM_STOP_AND_WAIT,
												 TIE_INPUT_ACTION_COMM_CONTINUE_MISSION, TIE_INPUT_ACTION_COMM_HEAD_HOME,
												 TIE_INPUT_ACTION_COMM_REPORT_ORDERS };
			vc_key_row(x, 152.0f, w, 46.0f, 5, l, a, row);
			for (int i = 0; i < 5; ++i)
				row[i]->enabled = st->has_target;
		}
		{
			static const char* const l[3] = { "RESUPPLY", "REINFORCE", "COMMANDS" };
			static const TieInputAction a[3] = { TIE_INPUT_ACTION_COMM_REQUEST_RESUPPLY,
												 TIE_INPUT_ACTION_COMM_REINFORCEMENTS,
												 TIE_INPUT_ACTION_INFO_WINGMAN_COMMANDS };
			vc_key_row(x, 206.0f, w, 46.0f, 3, l, a, NULL);
		}
		{
			vc_label(x, 268.0f, "SHIP", 28, 0, 1, 0);
			vc_label(x, 318.0f, "THROTTLE", 18, 0, 0, 0);
			static const char* const l[4] = { "0", "1/3", "2/3", "FULL" };
			static const TieInputAction a[4] = { TIE_INPUT_ACTION_THROTTLE_ZERO, TIE_INPUT_ACTION_THROTTLE_ONE_THIRD,
												 TIE_INPUT_ACTION_THROTTLE_TWO_THIRDS, TIE_INPUT_ACTION_THROTTLE_FULL };
			static const int pct[4] = { 0, 33, 67, 100 };
			vc_key_row(x + 110.0f, 306.0f, w - 110.0f, 46.0f, 4, l, a, row);
			for (int i = 0; i < 4; ++i)
				row[i]->lit = abs(st->throttle - pct[i]) <= 2;
		}
		{
			static const char* const l[5] = { "MAP", "GOALS", "DAMAGE", "HYPER-\nSPACE", "END\nMISSION" };
			static const TieInputAction a[5] = { TIE_INPUT_ACTION_INFO_MAP, TIE_INPUT_ACTION_INFO_GOALS,
												 TIE_INPUT_ACTION_INFO_DAMAGE, TIE_INPUT_ACTION_HYPERSPACE,
												 TIE_INPUT_ACTION_END_MISSION };
			vc_key_row(x, 362.0f, w, 58.0f, 5, l, a, row);
			row[3]->enabled = st->has_hyperdrive;
			/* The game asks to confirm with Space (both triggers). */
			row[4]->sub = st->confirm_offered ? "confirm: triggers" : NULL;
		}
		{
			/* The prompt the game waits on (dock, end mission, ...). */
			VcButton* b = vc_key(x, 430.0f, w, 44.0f, "CONFIRM (SPACE)", TIE_INPUT_ACTION_CONFIRM);
			b->enabled = st->confirm_offered;
			b->lit = st->confirm_offered;
		}
	}
}

static int vc_button_at(float px, float py) {
	for (int i = 0; i < s_vc.button_count; ++i) {
		const VcButton* b = &s_vc.buttons[i];
		if (px >= b->x && px < b->x + b->w && py >= b->y && py < b->y + b->h)
			return i;
	}
	return -1;
}

/* ---- Pressing --------------------------------------------------------- */

static void vc_press(int hand, int button) {
	VcHand* h = &s_vc.hands[hand];
	h->press_button = button;
	h->press_time = 0.0f;
	h->hold_fired = 0;
	if (button < 0)
		return;
	const VcButton* b = &s_vc.buttons[button];
	if (!b->enabled) {
		Aeron_XrHapticPulse(hand, 0.15f, 15);
		h->press_button = -1;
		return;
	}
	Aeron_XrHapticPulse(hand, 0.45f, 25);
	s_vc.flash[button] = VC_FLASH_S;
	if (b->hold_action == TIE_VRC_NONE)
		vc_push(b->action, b->arg);
}

static void vc_hold_tick(int hand) {
	VcHand* h = &s_vc.hands[hand];
	if (h->press_button < 0 || h->hold_fired)
		return;
	const VcButton* b = &s_vc.buttons[h->press_button];
	if (b->hold_action == TIE_VRC_NONE)
		return;
	h->press_time += s_vc.wall_dt;
	if (h->press_time >= VC_HOLD_S) {
		h->hold_fired = 1;
		Aeron_XrHapticPulse(hand, 0.9f, 60);
		s_vc.flash[h->press_button] = VC_FLASH_S * 2.0f;
		vc_push(b->hold_action, b->hold_arg);
	}
}

static void vc_release(int hand) {
	VcHand* h = &s_vc.hands[hand];
	if (h->press_button >= 0 && h->press_button < s_vc.button_count) {
		const VcButton* b = &s_vc.buttons[h->press_button];
		if (b->hold_action != TIE_VRC_NONE && !h->hold_fired)
			vc_push(b->action, b->arg); /* a tap */
	}
	h->press_button = -1;
	h->hold_fired = 0;
}

/* The controller's aim ray (-Z) and the tip in front of it. */
static void vc_hand_ray(const AeronXrHand* xh, float fwd[3], float tip[3]) {
	static const float minus_z[3] = { 0.0f, 0.0f, -1.0f };
	vc_rotate(xh->orientation, minus_z, fwd);
	for (int k = 0; k < 3; ++k)
		tip[k] = xh->position[k] + fwd[k] * VC_TIP_M;
}

/* Where the ray from `from` along `dir` meets the panel (pose, axes, size in
 * metres and pixels): 1 with the panel pixel and the LOCAL point. */
static int vc_ray_hit(const float from_world[3], const float dir[3], const AeronXrPanel* pose, const float ax[3],
					  const float ay[3], const float az[3], float w_m, float h_m, float w_px, float h_px, float* px,
					  float* py, float hit_world[3]) {
	const float from[3] = { from_world[0] - pose->position[0], from_world[1] - pose->position[1],
							from_world[2] - pose->position[2] };
	const float d0 = vc_dot(from, az);
	const float dn = vc_dot(dir, az);
	if (!(d0 > 0.0f && dn < -1.0e-3f))
		return 0;
	const float t = -d0 / dn;
	if (!(t > 0.0f && t < VC_RAY_MAX_M))
		return 0;
	const float hit[3] = { from[0] + dir[0] * t, from[1] + dir[1] * t, from[2] + dir[2] * t };
	const float rx = (vc_dot(hit, ax) / w_m + 0.5f) * w_px;
	const float ry = (0.5f - vc_dot(hit, ay) / h_m) * h_px;
	if (!(rx >= 0.0f && rx < w_px && ry >= 0.0f && ry < h_px))
		return 0;
	*px = rx;
	*py = ry;
	for (int k = 0; k < 3; ++k)
		hit_world[k] = pose->position[k] + hit[k];
	return 1;
}

static void vc_update_hand(int hand, const AeronXrPanel* pose, const float ax[3], const float ay[3],
						   const float az[3]) {
	VcHand* h = &s_vc.hands[hand];
	AeronXrHand xh;
	const int was_down = h->poke_down || h->trigger_down;
	h->cursor = 0;
	h->over_button = -1;
	if (!s_vc.awake || s_vc.lap.grab_hand == hand + 1 || !Aeron_XrGetHand(hand, &xh) || !xh.valid) {
		if (was_down)
			vc_release(hand);
		h->poke_down = h->trigger_down = 0;
		h->blocked = 0;
		h->captured = 0;
		return;
	}
	float fwd[3], tip[3];
	vc_hand_ray(&xh, fwd, tip);
	const float rel[3] = { tip[0] - pose->position[0], tip[1] - pose->position[1], tip[2] - pose->position[2] };
	const float lx = vc_dot(rel, ax), ly = vc_dot(rel, ay), depth = vc_dot(rel, az);
	const float px = (lx / VC_WIDTH_M + 0.5f) * VC_W;
	const float py = (0.5f - ly / VC_HEIGHT_M) * VC_H;
	const int inside = px >= 0.0f && px < VC_W && py >= 0.0f && py < VC_H;

	/* Poke. */
	if (h->poke_down) {
		if (depth > -VC_RELEASE_M || !inside) {
			h->poke_down = 0;
			vc_release(hand);
		}
	} else if (inside && depth < -VC_PRESS_M && !h->blocked && !h->trigger_down) {
		if (depth < -VC_BLOCK_M) {
			h->blocked = 1; /* arrived from behind or far too deep */
		} else {
			h->poke_down = 1;
			vc_press(hand, vc_button_at(px, py));
		}
	}
	if (h->blocked && (depth > 0.0f || !inside))
		h->blocked = 0;
	if (inside && depth < VC_HOVER_M && depth > -VC_BLOCK_M) {
		h->cursor = 1;
		h->cx = px;
		h->cy = py;
	}

	/* Ray: also up close; not while a poke holds a button. */
	int ray_hit = 0;
	if (!h->poke_down) {
		float rx, ry, hit[3];
		if (vc_ray_hit(xh.position, fwd, pose, ax, ay, az, VC_WIDTH_M, VC_HEIGHT_M, VC_W, VC_H, &rx, &ry, hit)) {
			ray_hit = 1;
			h->cursor = 2;
			memcpy(h->ray_start, tip, sizeof tip);
			memcpy(h->ray_end, hit, sizeof hit);
			h->cx = rx;
			h->cy = ry;
		}
	}
	/* The trigger belongs to the console while the ray is on it (and until
	 * a pull that started there is released). */
	h->captured = ray_hit || h->trigger_down;
	if (h->trigger_down) {
		if (xh.trigger < 0.35f) {
			h->trigger_down = 0;
			vc_release(hand);
		}
	} else if (ray_hit && xh.trigger >= 0.6f) {
		h->trigger_down = 1;
		vc_press(hand, vc_button_at(h->cx, h->cy));
	}
	if (h->cursor)
		h->over_button = vc_button_at(h->cx, h->cy);
	vc_hold_tick(hand);
}

/* ---- Grabbing --------------------------------------------------------- */

static void vc_grab_end(VcPlace* pl) {
	if (!pl->grab_hand)
		return;
	Aeron_XrHapticPulse(pl->grab_hand - 1, 0.3f, 20);
	pl->grab_hand = 0;
	pl->grab_side = 0;
	if (pl != &s_vc.lap)
		return;
	snprintf(s_vc.placement, sizeof s_vc.placement, "%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f", pl->cpos[0], pl->cpos[1],
			 pl->cpos[2], pl->cq[0], pl->cq[1], pl->cq[2], pl->cq[3]);
	SDL_setenv_unsafe("XWA_XR_CONSOLE_POSE", s_vc.placement, 1);
	snprintf(s_vc.pose_env, sizeof s_vc.pose_env, "%s", s_vc.placement);
	s_vc.placement_dirty = 1;
	Aeron_LogInfo("tie.vrconsole", "lap console moved to %s", s_vc.placement);
}

/* Whether `hand` holds a panel other than `pl`. */
static int vc_hand_holds_other(const VcPlace* pl, int hand) {
	if (pl != &s_vc.lap && s_vc.lap.grab_hand == hand + 1)
		return 1;
	return pl != &s_vc.card.place && s_vc.card.place.grab_hand == hand + 1;
}

/* Squeezing the grip at either end of a panel (w x h metres) picks it up;
 * it follows the hand freely until the grip is let go. Not while the quick
 * wheel is open on that hand. */
static void vc_update_grab(VcPlace* pl, const AeronXrPanel* pose, const float ax[3], const float ay[3],
						   const float az[3], float w, float h) {
	for (int hand = 0; hand < 2; ++hand) {
		AeronXrHand xh;
		pl->hover_side[hand] = 0;
		if (!Aeron_XrGetHand(hand, &xh) || !xh.valid || (s_vw.open && s_vw.hand == hand)) {
			if (pl->grab_hand == hand + 1)
				vc_grab_end(pl);
			pl->grip_prev[hand] = Aeron_XrGetHand(hand, &xh) ? xh.grip : 0.0f;
			continue;
		}
		const float rel[3] = { xh.position[0] - pose->position[0], xh.position[1] - pose->position[1],
							   xh.position[2] - pose->position[2] };
		const float lx = vc_dot(rel, ax), ly = vc_dot(rel, ay), depth = vc_dot(rel, az);
		const float edge = fabsf(lx) - 0.5f * w;
		const int in_zone = edge > -VC_GRAB_X_M && edge < VC_GRAB_X_M && fabsf(ly) < 0.5f * h + VC_GRAB_Y_M &&
							depth < VC_GRAB_FRONT_M && depth > -VC_GRAB_BACK_M && !vc_hand_holds_other(pl, hand);
		if (in_zone)
			pl->hover_side[hand] = lx < 0.0f ? -1 : 1;
		if (pl->grab_hand == hand + 1) {
			if (xh.grip < VC_GRIP_OFF) {
				vc_grab_end(pl);
			} else {
				/* panel = hand * relative */
				float off[3];
				vc_rotate(xh.orientation, pl->grab_pos, off);
				for (int k = 0; k < 3; ++k)
					pl->cpos[k] = xh.position[k] + off[k];
				vc_qmul(xh.orientation, pl->grab_q, pl->cq);
				vc_qnorm(pl->cq);
				pl->custom = 1;
			}
		} else if (!pl->grab_hand && in_zone && xh.grip >= VC_GRIP_ON && pl->grip_prev[hand] < VC_GRIP_OFF) {
			/* relative = inverse(hand) * panel */
			const float inv[4] = { -xh.orientation[0], -xh.orientation[1], -xh.orientation[2], xh.orientation[3] };
			const float to_panel[3] = { -rel[0], -rel[1], -rel[2] };
			vc_rotate(inv, to_panel, pl->grab_pos);
			vc_qmul(inv, pose->orientation, pl->grab_q);
			vc_qnorm(pl->grab_q);
			memcpy(pl->cpos, pose->position, sizeof pl->cpos);
			memcpy(pl->cq, pose->orientation, sizeof pl->cq);
			vc_qnorm(pl->cq);
			pl->custom = 1;
			pl->grab_hand = hand + 1;
			pl->grab_side = lx < 0.0f ? -1 : 1;
			Aeron_XrHapticPulse(hand, 0.6f, 30);
		}
		pl->grip_prev[hand] = xh.grip;
		/* The grip at a grab zone belongs to the panel, not the game. */
		if (in_zone || pl->grab_hand == hand + 1)
			s_vc.grip_capture[hand] = 1;
	}
}

static int vp_ray_beam(int hand, float start[3], float end[3]);

int TieVrConsole_RayBeam(int hand, float start[3], float end[3]) {
	if (s_vw.open && s_vw.drawn && hand == s_vw.hand) {
		memcpy(start, s_vw.ray_start, sizeof s_vw.ray_start);
		memcpy(end, s_vw.ray_end, sizeof s_vw.ray_end);
		return 2;
	}
	if (hand < 0 || hand > 1)
		return 0;
	if (!s_vc.drawn || !s_vc.awake || s_vc.hands[hand].cursor != 2)
		return vp_ray_beam(hand, start, end);
	memcpy(start, s_vc.hands[hand].ray_start, sizeof s_vc.hands[hand].ray_start);
	memcpy(end, s_vc.hands[hand].ray_end, sizeof s_vc.hands[hand].ray_end);
	return 1;
}

int TieVrConsole_TakePlacement(char* out, int size) {
	if (!s_vc.placement_dirty || !out || size <= 0)
		return 0;
	snprintf(out, (size_t)size, "%s", s_vc.placement);
	s_vc.placement_dirty = 0;
	return 1;
}

/* ---- Drawing ---------------------------------------------------------- */

static float s_alpha_draw = 1.0f; /* the panel being drawn: its fade */

static void vc_pma(float out[4], float r, float g, float b, float a) {
	a *= s_alpha_draw;
	out[0] = r * a;
	out[1] = g * a;
	out[2] = b * a;
	out[3] = a;
}

static void vc_rrect(float x, float y, float w, float h, float radius, float border_px, float soft,
					 const float fill[4], const float border[4]) {
	AeronDrawList2DRRect r;
	memset(&r, 0, sizeof r);
	r.dst_x = x;
	r.dst_y = y;
	r.dst_w = w;
	r.dst_h = h;
	r.radius_px = radius;
	r.border_px = border_px;
	r.soft_px = soft;
	memcpy(r.fill_top, fill, sizeof r.fill_top);
	memcpy(r.fill_bottom, fill, sizeof r.fill_bottom);
	if (border)
		memcpy(r.border, border, sizeof r.border);
	AeronDrawList_AddRRect(s_vc.cur_list, &r);
}

static void vc_line(float x0, float y0, float x1, float y1, float width, const float c[4]) {
	AeronDrawList_AddLine(s_vc.cur_list, x0, y0, x1, y1, width, c, AERON_BLIT2D_BLEND_PMA, NULL);
}

static void vc_fill(float x, float y, float w, float h, const float c[4]) {
	AeronDrawList_AddFill(s_vc.cur_list, x, y, w, h, c, AERON_BLIT2D_BLEND_PMA, NULL);
}

static float vc_text_width(const char* text, float size_px) {
	if (s_vc.font_state != 1 || !text)
		return 0.0f;
	AeronTextStyle probe;
	memset(&probe, 0, sizeof probe);
	probe.font = &s_vc.font;
	probe.scale = size_px / (float)s_vc.font.cell_h;
	return AeronText_MeasureWidth(&probe, text, -1);
}

static void vc_text(float x, float y, const char* text, float size_px, int align, float r, float g, float b,
					float a) {
	if (s_vc.font_state != 1 || !text || !text[0])
		return;
	AeronTextStyle style;
	memset(&style, 0, sizeof style);
	style.font = &s_vc.font;
	style.scale = size_px / (float)s_vc.font.cell_h;
	style.tint[0] = r;
	style.tint[1] = g;
	style.tint[2] = b;
	style.tint[3] = a * s_alpha_draw;
	style.filter = AERON_BLIT2D_FILTER_LINEAR;
	AeronText_Draw(s_vc.cur_list, &style, x, y, (AeronTextAlign)align, text, -1, NULL);
}

/* The panels' holo colour: TIE's targeting-computer green, or
 * TIE_XR_CONSOLE_COLOR=RRGGBB. A darker tone for plates, a pale one for
 * text. */
#define VC_HUD s_vc.hud[0], s_vc.hud[1], s_vc.hud[2]
#define VC_HUD_DARK s_vc.hud_dark[0], s_vc.hud_dark[1], s_vc.hud_dark[2]
#define VC_HUD_PALE s_vc.hud_pale[0], s_vc.hud_pale[1], s_vc.hud_pale[2]
#define VC_AMBER 1.0f, 0.710f, 0.278f

static void vc_hud_colors(void) {
	float c[3] = { 0.45f, 1.0f, 0.62f };
	const char* env = SDL_getenv("TIE_XR_CONSOLE_COLOR");
	if (env && strlen(env) >= 6) {
		const unsigned long rgb = strtoul(env[0] == '#' ? env + 1 : env, NULL, 16);
		for (int i = 0; i < 3; ++i)
			c[i] = (float)((rgb >> (16 - 8 * i)) & 255u) / 255.0f;
	}
	for (int i = 0; i < 3; ++i) {
		s_vc.hud[i] = c[i];
		s_vc.hud_dark[i] = c[i] * 0.75f;
		s_vc.hud_pale[i] = 0.55f * c[i] + 0.45f * 0.92f;
	}
}

/* Lap console button i: under a cursor, pressed. */
static void vc_button_hands(int i, int* hover, int* pressed) {
	*hover = *pressed = 0;
	for (int h = 0; h < 2; ++h) {
		*hover |= s_vc.hands[h].over_button == i;
		*pressed |= s_vc.hands[h].press_button == i;
	}
}

static void vc_draw_button_look(const VcButton* b, int hover, int pressed, float flash) {
	float fill[4], border[4];
	if (!b->enabled) {
		vc_pma(fill, 0.42f, 0.48f, 0.50f, 0.05f);
		vc_pma(border, 0.42f, 0.48f, 0.50f, 0.35f);
	} else {
		const float on = fmaxf(b->lit ? 0.30f : b->level ? 0.16f : 0.08f, b->base_fill);
		vc_pma(fill, VC_HUD, on + 0.22f * (float)hover + 0.25f * (float)(pressed || flash > 0.0f));
		if (hover)
			vc_pma(border, 1.0f, 1.0f, 1.0f, 1.0f);
		else
			vc_pma(border, VC_HUD, b->lit ? 1.0f : 0.62f);
	}
	const float radius = b->radius > 0.0f ? b->radius : 7.0f;
	if (b->enabled && (b->lit || hover || pressed || flash > 0.0f)) {
		float glow[4];
		const float transparent[4] = { 0, 0, 0, 0 };
		vc_pma(glow, VC_HUD, b->lit ? 0.45f : hover ? 0.55f : 0.3f);
		vc_rrect(b->x - 6.0f, b->y - 6.0f, b->w + 12.0f, b->h + 12.0f, radius + 5.0f, 6.0f, 10.0f, transparent, glow);
	}
	vc_rrect(b->x, b->y, b->w, b->h, radius, 2.0f, 1.0f, fill, border);

	const float tr = !b->enabled ? 0.45f : 0.88f;
	const float tg = !b->enabled ? 0.52f : 1.0f;
	const float tb = !b->enabled ? 0.55f : 0.92f;
	const float text_a = b->lit || pressed ? 1.0f : 0.9f;
	float size = b->font_size > 0.0f ? b->font_size : b->h >= 60.0f ? 22.0f : 20.0f;
	/* "LINE ONE\nLINE TWO": two centred lines. */
	char line1[48], line2[48];
	const char* nl = strchr(b->label, '\n');
	line2[0] = '\0';
	if (nl) {
		snprintf(line1, sizeof line1, "%.*s", (int)(nl - b->label), b->label);
		snprintf(line2, sizeof line2, "%s", nl + 1);
	} else {
		snprintf(line1, sizeof line1, "%s", b->label);
	}
	/* Shrink long labels to fit. */
	{
		const float width = fmaxf(vc_text_width(line1, size), vc_text_width(line2, size));
		if (width > b->w - 14.0f && width > 0.0f)
			size *= (b->w - 14.0f) / width;
	}
	float sub_size = line2[0] ? fmaxf(14.0f, size * 0.5f) : 15.0f;
	if (b->sub) {
		const float width = vc_text_width(b->sub, sub_size);
		if (width > b->w - 10.0f && width > 0.0f)
			sub_size *= (b->w - 10.0f) / width;
	}
	const float cx = b->x + b->w * 0.5f, cy = b->y + b->h * 0.5f;
	if (line2[0] && b->sub) {
		vc_text(cx, cy - size * 1.5f, line1, size, 1, tr, tg, tb, text_a);
		vc_text(cx, cy - size * 0.45f, line2, size, 1, tr, tg, tb, text_a);
		vc_text(cx, cy + size * 0.9f, b->sub, sub_size, 1, tr, tg, tb, 0.65f);
	} else if (line2[0]) {
		vc_text(cx, cy - size * 1.05f, line1, size, 1, tr, tg, tb, text_a);
		vc_text(cx, cy + size * 0.02f, line2, size, 1, tr, tg, tb, text_a);
	} else if (b->sub) {
		vc_text(cx, cy - size * 0.95f, b->label, size, 1, tr, tg, tb, text_a);
		vc_text(cx, cy + 2.0f, b->sub, sub_size, 1, tr, tg, tb, 0.65f);
	} else {
		vc_text(cx, cy - size * 0.55f, b->label, size, 1, tr, tg, tb, text_a);
	}
}

static void vc_draw_button(int i) {
	int hover, pressed;
	vc_button_hands(i, &hover, &pressed);
	vc_draw_button_look(&s_vc.buttons[i], hover, pressed, s_vc.flash[i]);
}

/* Grab handles: thick bars at both ends of the current panel, bright while
 * a hand is at them or holds them. */
static void vc_draw_handles(const VcPlace* pl) {
	const float W = s_vc.cur_w_px, H = s_vc.cur_h_px;
	for (int side = -1; side <= 1; side += 2) {
		int hot = pl->grab_side == side;
		for (int h = 0; h < 2; ++h)
			hot |= pl->hover_side[h] == side;
		const float bar_w = 16.0f;
		const float x = side < 0 ? 9.0f : W - 9.0f - bar_w;
		float bar[4], rim[4];
		vc_pma(bar, VC_HUD, pl->grab_side == side ? 0.85f : hot ? 0.6f : 0.32f);
		vc_pma(rim, VC_HUD, hot ? 1.0f : 0.75f);
		vc_rrect(x, 52.0f, bar_w, H - 104.0f, 8.0f, 2.0f, 1.0f, bar, rim);
		for (int r = -2; r <= 2; ++r) {
			float ridge[4];
			vc_pma(ridge, 0.0f, 0.0f, 0.0f, 0.35f);
			vc_fill(x + 3.0f, H * 0.5f + (float)r * 22.0f - 1.5f, bar_w - 6.0f, 3.0f, ridge);
		}
	}
}

/* One bar of `count` lights, `lit` of them in colour `c`, its label and
 * value above. */
static void vc_gauge(float x, float y, float w, const char* label, const char* value, int count, int lit,
					 const float c[3]) {
	const float gap = 4.0f, h = 18.0f;
	const float cell = (w - gap * (float)(count - 1)) / (float)count;
	vc_text(x, y, label, 16.0f, 0, c[0], c[1], c[2], 0.95f);
	vc_text(x + w, y, value, 16.0f, 2, c[0], c[1], c[2], 0.95f);
	for (int i = 0; i < count; ++i) {
		float fill[4], rim[4];
		if (i < lit) {
			vc_pma(fill, c[0], c[1], c[2], 0.85f);
			vc_pma(rim, c[0], c[1], c[2], 1.0f);
		} else {
			vc_pma(fill, c[0], c[1], c[2], 0.06f);
			vc_pma(rim, c[0], c[1], c[2], 0.40f);
		}
		vc_rrect(x + (float)i * (cell + gap), y + 22.0f, cell, h, 4.0f, 1.5f, 1.0f, fill, rim);
	}
}

/* Shields front / rear, hull and throttle as bars of ten lights. */
static void vc_draw_gauges(float x, float y, float w) {
	const TieVrConsoleState* st = &s_vc.state;
	static const float yellow[3] = { 1.0f, 0.85f, 0.25f };
	static const float red[3] = { 1.0f, 0.36f, 0.28f };
	static const float blue[3] = { 0.36f, 0.66f, 1.0f };
	const float gap = 18.0f, bw = (w - 3.0f * gap) / 4.0f;
	char text[4][16];
	if (st->has_shields) {
		snprintf(text[0], sizeof text[0], "%d%%", st->shield_front);
		snprintf(text[1], sizeof text[1], "%d%%", st->shield_rear);
	} else {
		snprintf(text[0], sizeof text[0], "--");
		snprintf(text[1], sizeof text[1], "--");
	}
	snprintf(text[2], sizeof text[2], "%d%%", st->hull);
	snprintf(text[3], sizeof text[3], "%d%%", st->throttle);
	vc_gauge(x, y, bw, "FRONT", text[0], 10, st->has_shields ? (st->shield_front + 5) / 10 : 0, yellow);
	vc_gauge(x + bw + gap, y, bw, "REAR", text[1], 10, st->has_shields ? (st->shield_rear + 5) / 10 : 0, yellow);
	vc_gauge(x + 2.0f * (bw + gap), y, bw, "HULL", text[2], 10, (st->hull + 5) / 10, red);
	vc_gauge(x + 3.0f * (bw + gap), y, bw, "SPEED", text[3], 10, (st->throttle + 5) / 10, blue);
}

/* The plate every panel stands on: faint tint, bright rim, soft halo, scan
 * lines, the grab handles. */
static void vc_plate(const VcPlace* place) {
	const float W = s_vc.cur_w_px, H = s_vc.cur_h_px;
	const float transparent[4] = { 0, 0, 0, 0 };
	float fill[4], border[4], glow[4];
	vc_pma(glow, VC_HUD, 0.25f);
	vc_rrect(4.0f, 4.0f, W - 8.0f, H - 8.0f, 30.0f, 8.0f, 14.0f, transparent, glow);
	/* A dark tint of the holo colour: the head-locked HUD and the space
	 * behind must not show through the text. */
	vc_pma(fill, s_vc.hud_dark[0] * 0.18f, s_vc.hud_dark[1] * 0.18f, s_vc.hud_dark[2] * 0.18f, 0.72f);
	vc_pma(border, VC_HUD, 0.6f);
	vc_rrect(10.0f, 10.0f, W - 20.0f, H - 20.0f, 26.0f, 2.0f, 1.0f, fill, border);
	if (place)
		vc_draw_handles(place);
	for (float y = 14.0f; y < H - 14.0f; y += 6.0f) {
		float line[4];
		vc_pma(line, VC_HUD, 0.025f);
		vc_fill(14.0f, y, W - 28.0f, 2.0f, line);
	}
}

static void vc_draw(void) {
	vc_hud_colors();
	vc_plate(&s_vc.lap);
	vc_draw_gauges(34.0f, 410.0f, 470.0f);
	/* Column dividers, and the rules between stacked sections. */
	{
		float line[4];
		vc_pma(line, VC_HUD, 0.35f);
		vc_fill(520.0f, 40.0f, 2.0f, VC_H - 80.0f, line);
		vc_fill(1015.0f, 40.0f, 2.0f, VC_H - 80.0f, line);
		vc_fill(538.0f, 308.0f, 460.0f, 2.0f, line);
		vc_fill(1032.0f, 260.0f, 470.0f, 2.0f, line);
	}
	for (int i = 0; i < s_vc.label_count; ++i) {
		const VcLabel* l = &s_vc.labels[i];
		if (l->section)
			vc_text(l->x, l->y, l->text, (float)l->size, l->align, VC_HUD, 1.0f);
		else if (l->dim)
			vc_text(l->x, l->y, l->text, (float)l->size, l->align, 0.42f, 0.48f, 0.50f, 0.8f);
		else
			vc_text(l->x, l->y, l->text, (float)l->size, l->align, VC_HUD_PALE, 0.95f);
	}
	for (int i = 0; i < s_vc.button_count; ++i)
		vc_draw_button(i);
}

/* The lap console's overlay: a dot where a ray lands (a poke lights the
 * button under the finger instead). */
static void vc_console_overlay(void) {
	vc_hud_colors();
	for (int h = 0; h < 2; ++h) {
		const VcHand* hand = &s_vc.hands[h];
		if (hand->cursor != 2)
			continue;
		float c[4], ring[4];
		vc_pma(c, 1.0f, 1.0f, 1.0f, 0.95f);
		vc_pma(ring, VC_HUD, 0.9f);
		vc_rrect(hand->cx - 9.0f, hand->cy - 9.0f, 18.0f, 18.0f, 9.0f, 3.0f, 1.5f, c, ring);
	}
}

/* ---- Frame ------------------------------------------------------------ */

static int vc_enabled(void) { return !vc_env_off("XWA_XR_CONSOLE"); }
static int vp_enabled(void) { return !vc_env_off("XWA_XR_SIDE_PANEL"); }
static int vw_enabled(void) { return !vc_env_off("XWA_XR_WHEEL"); }

static int vc_ensure(void);

void TieVrConsole_BeginFrame(float dt_seconds) {
	s_vc.dt = dt_seconds > 0.0f && dt_seconds < 0.25f ? dt_seconds : 0.0f;
	s_vc.wall_dt = dt_seconds > 0.0f ? fminf(dt_seconds, 1.0f) : 0.0f;
	s_vc.clock = fmodf(s_vc.clock + s_vc.dt, 1000.0f);
	s_vc.drawn = 0;
	s_vw.drawn = 0;
	s_vc.card.drawn = 0;
	/* Load the font here, between frames, rather than while the flight
	 * renderer is recording its command buffer. */
	if (s_vc.state.valid && s_vc.font_state == 0 && (vc_enabled() || vp_enabled() || vw_enabled()) &&
		Aeron_XrIsPresenting())
		vc_ensure();
}

void TieVrConsole_SetState(const TieVrConsoleState* state) {
	if (state)
		s_vc.state = *state;
	else
		memset(&s_vc.state, 0, sizeof s_vc.state);
}

void TieVrConsole_SetVoiceList(int (*count)(void), const char* (*label)(int index),
							   const char* (*phrase)(int index, int phrase)) {
	s_vc.voice_count = count;
	s_vc.voice_label = label;
	s_vc.voice_phrase = phrase;
}

static int vc_ensure(void) {
	if (!s_vc.list) {
		s_vc.list = AeronDrawList_Create(0);
		if (!s_vc.list)
			return 0;
		for (int h = 0; h < 2; ++h)
			s_vc.hands[h].press_button = -1;
	}
	if (s_vc.font_state == 0) {
		AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
		s_vc.font_state = -1;
		if (cmd) {
			/* The settings menu's font (resources/assets/fonts/menu). */
			if (AeronFontAtlas_LoadVfs(&s_vc.font, cmd, Aeron_GetVfs(), AERON_VFS_ROOT_RESOURCE, "assets/fonts/menu",
									   64u * 1024u * 1024u)) {
				if (Aeron_SubmitCommandBuffer(cmd))
					s_vc.font_state = 1;
				else
					AeronFontAtlas_Release(&s_vc.font);
			} else {
				Aeron_CancelCommandBuffer(cmd);
			}
		}
		if (s_vc.font_state != 1)
			Aeron_LogWarn("tie.vrconsole", "font unavailable, the VR panels show no text");
	}
	return 1;
}

/* Whether a panel (pose, w x h metres) is anywhere near the field of view. */
static int vc_in_view(const AeronXrPanel* pose, float w, float h, const AeronXrView views[2]) {
	static const float minus_z[3] = { 0.0f, 0.0f, -1.0f };
	float f[3];
	vc_rotate(views[0].orientation, minus_z, f);
	float to[3];
	for (int k = 0; k < 3; ++k)
		to[k] = pose->position[k] - 0.5f * (views[0].position[k] + views[1].position[k]);
	const float len = sqrtf(vc_dot(to, to));
	if (len < 0.05f)
		return 1;
	const float angle = acosf(fmaxf(-1.0f, fminf(1.0f, vc_dot(f, to) / len)));
	const float radius = atanf(0.5f * sqrtf(w * w + h * h) / len);
	return angle - radius < 62.0f * VC_PI / 180.0f;
}

/* Bright while looked at (within wake_deg of the gaze) or handled. */
static void vc_update_wake(const AeronXrPanel* pose, const AeronXrView views[2], const VcPlace* place,
						   float wake_deg, float sleep_deg, int* awake, float* alpha) {
	static const float minus_z[3] = { 0.0f, 0.0f, -1.0f };
	float eye[3], f[3];
	for (int k = 0; k < 3; ++k)
		eye[k] = 0.5f * (views[0].position[k] + views[1].position[k]);
	vc_rotate(views[0].orientation, minus_z, f);
	const float to[3] = { pose->position[0] - eye[0], pose->position[1] - eye[1], pose->position[2] - eye[2] };
	const float len = sqrtf(vc_dot(to, to));
	const float c = len > 1e-4f ? vc_dot(f, to) / len : 1.0f;
	const float angle = acosf(fmaxf(-1.0f, fminf(1.0f, c))) * 180.0f / VC_PI;
	if (place->grab_hand || place->hover_side[0] || place->hover_side[1])
		*awake = 1;
	else if (*awake && angle > sleep_deg)
		*awake = 0;
	else if (!*awake && angle < wake_deg)
		*awake = 1;
	const float goal = *awake ? 1.0f : VC_SLEEP_ALPHA;
	const float step = s_vc.dt / 0.2f;
	*alpha = *alpha < goal ? fminf(goal, *alpha + step) : fmaxf(goal, *alpha - step);
	if (s_vc.dt <= 0.0f)
		*alpha = goal;
}

/* Records the current panel with `draw` and redraws its layer only when
 * that changed (one image serves both eyes). A panel's own content is kept
 * in an image of its own; the layer is that image plus the overlay
 * (cursors). `key` (nonzero) sums up everything `draw` reads: while it is
 * the same the content is not even recorded again. Fills out[0]'s rect and
 * returns 1, or 0 when out of view. */
typedef struct VcBase {
	AeronRenderTarget* rt;
	AeronDrawList2D* list;
	uint64_t hash;
	uint64_t list_hash;
	int valid, w, h;
	AeronTextureFormat format;
	uint64_t key;
} VcBase;
static VcBase s_base[AERON_XR_PANEL_COUNT];

static int vc_render_panel(AeronCommandBuffer* cmd, int layer, AeronDrawList2D* list, void (*draw)(void),
						   void (*overlay)(void), const AeronXrView views[2], AeronXrPanel out[2], int width,
						   int height, float w_m, float h_m, uint64_t* shown_hash, int* hash_valid, uint64_t key) {
	const AeronXrPanel pose = out[0];
	if (!vc_in_view(&pose, w_m, h_m, views) || layer < 0 || layer >= AERON_XR_PANEL_COUNT)
		return 0; /* the layer keeps its image */
	VcBase* base = &s_base[layer];
	if (key && key == base->key && base->valid && !overlay && *hash_valid) {
		out[0].rect[0] = 0;
		out[0].rect[1] = 0;
		out[0].rect[2] = width;
		out[0].rect[3] = height;
		out[0].eye = 0;
		return 1;
	}
	if (!base->list && !(base->list = AeronDrawList_Create(0)))
		return 0;
	/* The content's own image, in the layer's format. Taking the layer's
	 * target acquires its next image, so only do that to draw into it. */
	static AeronTextureFormat s_panel_format = AERON_TEXTURE_FORMAT_UNKNOWN;
	AeronRenderTarget* target = NULL;
	if (s_panel_format == AERON_TEXTURE_FORMAT_UNKNOWN) {
		target = Aeron_XrStereoPanelTarget(layer, NULL, NULL);
		AeronTexture* target_tex = target ? Aeron_RenderTargetGetTexture(target) : NULL;
		if (!target_tex)
			return 0;
		s_panel_format = Aeron_TextureGetFormat(target_tex);
		*hash_valid = 0; /* acquired: must be drawn this frame */
	}
	const AeronTextureFormat format = s_panel_format;
	if (!base->rt || base->w != width || base->h != height || base->format != format) {
		if (base->rt)
			Aeron_DestroyRenderTarget(base->rt);
		base->rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
			.width = width,
			.height = height,
			.format = format,
			.debug_name = "tie.vr_console.panel",
		});
		base->w = width;
		base->h = height;
		base->format = format;
		base->valid = 0;
		base->key = 0;
		if (!base->rt)
			return 0;
	}
	if (!key || key != base->key) {
		base->key = 0;
		s_vc.cur_list = base->list;
		AeronDrawList_Begin(base->list, NULL, width, height, AERON_DRAWLIST2D_CLEAR, NULL);
		draw();
		base->list_hash = AeronDrawList_Hash(base->list);
	}
	const uint64_t base_hash = base->list_hash;
	AeronDrawList2DSprite content;
	memset(&content, 0, sizeof content);
	content.texture = Aeron_RenderTargetGetTexture(base->rt);
	content.src_u1 = 1.0f;
	content.src_v1 = 1.0f;
	content.dst_w = (float)width;
	content.dst_h = (float)height;
	content.tint[0] = content.tint[1] = content.tint[2] = content.tint[3] = 1.0f;
	content.blend = AERON_BLIT2D_BLEND_NONE;
	content.filter = AERON_BLIT2D_FILTER_NEAREST;
	s_vc.cur_list = list;
	AeronDrawList_Begin(list, NULL, width, height, AERON_DRAWLIST2D_CLEAR, NULL);
	AeronDrawList_AddSprite(list, &content);
	if (overlay)
		overlay();
	const uint64_t hash = base_hash * 1099511628211ull ^ AeronDrawList_Hash(list);
	if (!*hash_valid || hash != *shown_hash) {
		if (!target && !(target = Aeron_XrStereoPanelTarget(layer, NULL, NULL)))
			return 0;
		if (!base->valid || base->hash != base_hash) {
			AeronDrawList_SetTarget(base->list, base->rt);
			AeronDrawList_Render(base->list, cmd);
			base->hash = base_hash;
			base->valid = 1;
		}
		AeronDrawList_SetTarget(list, target);
		if (!AeronDrawList_Prepare(list, cmd))
			return 0;
		AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
			.color_target = target,
			.clear_color = 1,
			.command_buffer = cmd,
			.debug_label = "VR panel",
		});
		if (!pass)
			return 0;
		AeronDrawList_RenderIntoPass(list, cmd, pass, target);
		Aeron_EndRenderPass(pass);
		*shown_hash = hash;
		*hash_valid = 1;
	}
	base->key = key;
	out[0].rect[0] = 0;
	out[0].rect[1] = 0;
	out[0].rect[2] = width;
	out[0].rect[3] = height;
	out[0].eye = 0;
	return 1;
}

static uint64_t vc_mix(uint64_t k, uint32_t v) { return (k ^ v) * 1099511628211ull; }

static uint64_t vc_mix_f(uint64_t k, float f) {
	uint32_t u;
	memcpy(&u, &f, sizeof u);
	return vc_mix(k, u);
}

static uint64_t vc_mix_s(uint64_t k, const char* s) {
	if (!s)
		return vc_mix(k, 0x9e3779b9u);
	for (; *s; ++s)
		k = vc_mix(k, (uint8_t)*s);
	return vc_mix(k, 0u);
}

/* The lap console's key: everything vc_draw reads. */
static uint64_t vc_console_key(int width, int height) {
	uint64_t k = 1469598103934665603ull;
	const unsigned char* st = (const unsigned char*)&s_vc.state;
	for (size_t i = 0; i < sizeof s_vc.state; ++i)
		k = vc_mix(k, st[i]);
	k = vc_mix(k, (uint32_t)width);
	k = vc_mix(k, (uint32_t)height);
	k = vc_mix(k, (uint32_t)s_vc.font_state);
	k = vc_mix_f(k, s_alpha_draw);
	k = vc_mix_s(k, SDL_getenv("TIE_XR_CONSOLE_COLOR"));
	k = vc_mix(k, (uint32_t)s_vc.lap.grab_side);
	k = vc_mix(k, (uint32_t)s_vc.lap.hover_side[0]);
	k = vc_mix(k, (uint32_t)s_vc.lap.hover_side[1]);
	k = vc_mix(k, (uint32_t)s_vc.label_count);
	for (int i = 0; i < s_vc.label_count; ++i) {
		const VcLabel* l = &s_vc.labels[i];
		k = vc_mix_f(k, l->x);
		k = vc_mix_f(k, l->y);
		k = vc_mix_s(k, l->text);
		k = vc_mix(k, (uint32_t)(l->size | l->align << 8 | l->section << 12 | l->dim << 16));
	}
	k = vc_mix(k, (uint32_t)s_vc.button_count);
	for (int i = 0; i < s_vc.button_count; ++i) {
		const VcButton* b = &s_vc.buttons[i];
		int hover, pressed;
		vc_button_hands(i, &hover, &pressed);
		k = vc_mix_f(k, b->x);
		k = vc_mix_f(k, b->y);
		k = vc_mix_f(k, b->w);
		k = vc_mix_f(k, b->h);
		k = vc_mix_s(k, b->label);
		k = vc_mix_s(k, b->sub);
		k = vc_mix(k, (uint32_t)(b->enabled | b->lit << 1 | b->level << 2 | hover << 4 | pressed << 5 |
								 (s_vc.flash[i] > 0.0f) << 6));
		k = vc_mix(k, (uint32_t)b->hold_action);
	}
	return k | 1u; /* never 0 */
}

int TieVrConsole_Frame(AeronCommandBuffer* cmd, int width, int height, const AeronXrView views[2],
					   AeronXrPanel out[2]) {
	if (!cmd || width <= 0 || height <= 0 || !views || !out || !s_vc.state.valid || !vc_enabled() ||
		s_vc.font_state == 0 || !vc_ensure())
		return 0; /* the font loads in BeginFrame first */
	if (s_vw.open)
		return 0; /* hidden while the quick wheel is open */
	s_vc.drawn = 1;
	if (!s_vc.logged) {
		Aeron_LogInfo("tie.vrconsole", "lap console: %dx%d layer, %.0f deg down, %.2f m", width, height,
					  (double)vc_env("XWA_XR_CONSOLE_PITCH", VC_PITCH_DEG),
					  (double)vc_env("XWA_XR_CONSOLE_DISTANCE", VC_DISTANCE_M));
		s_vc.logged = 1;
	}
	memset(out, 0, sizeof *out);
	float ax[3], ay[3], az[3];
	if (!s_vc.lap.grab_hand)
		vc_read_pose_env();
	vc_pose(out, ax, ay, az);
	vc_update_grab(&s_vc.lap, out, ax, ay, az, VC_WIDTH_M, VC_HEIGHT_M);
	vc_pose(out, ax, ay, az); /* moved by the grab */
	vc_update_wake(out, views, &s_vc.lap, VC_WAKE_DEG, VC_SLEEP_DEG, &s_vc.awake, &s_vc.alpha);

	vc_layout();
	for (int i = 0; i < VC_MAX_BUTTONS; ++i)
		if (s_vc.flash[i] > 0.0f)
			s_vc.flash[i] -= s_vc.dt;
	for (int h = 0; h < 2; ++h)
		vc_update_hand(h, out, ax, ay, az);

	s_vc.cur_w_px = VC_W;
	s_vc.cur_h_px = VC_H;
	s_alpha_draw = s_vc.alpha;
	if (!s_vc.list_r && !(s_vc.list_r = AeronDrawList_Create(0)))
		return 0;
	return vc_render_panel(cmd, AERON_XR_PANEL_CONSOLE, s_vc.list_r, vc_draw, vc_console_overlay, views, out, width,
						   height, VC_WIDTH_M, VC_HEIGHT_M, &s_vc.hash, &s_vc.hash_valid,
						   vc_console_key(width, height));
}

/* ---- Quick wheel -------------------------------------------------------
 * Held open with the grip of the hand that has the game's LEFT controls,
 * once held TIE_VR_WHEEL_DELAY_S without a grip + button chord (the grip is
 * TIE's shift layer). It sits VW_AHEAD_M in front of that controller, level
 * and facing the head's yaw, and follows the controller's position but not
 * its turning: where the controller points picks a slice, its trigger takes
 * it and the wheel stays open; letting go of the grip closes it. Level 0:
 * four categories round CONFIRM (Space); a category opens its commands
 * round BACK. Drawn on the CARD2 layer (1024 x 768, the wheel a circle in
 * it). */
#define VW_W 1024.0f
#define VW_H 768.0f
#define VW_H_M 0.26f
#define VW_W_M (VW_H_M * VW_W / VW_H)
#define VW_PX_PER_M (VW_H / VW_H_M)
#define VW_AHEAD_M 0.15f
#define VW_PLATE_R 370.0f
#define VW_CENTER_R 84.0f
#define VW_SELECT_MAX_M 0.40f
#define VW_HYST_DEG 5.0f
#define VW_TRIGGER_ON 0.6f
#define VW_TRIGGER_OFF 0.3f
#define VW_GROW_S 0.09f

enum { VW_CAT_SHIP, VW_CAT_ENERGY, VW_CAT_TARGETING, VW_CAT_WINGMEN, VW_CATS };
static const char* const k_vw_cat_label[VW_CATS] = { "SHIP &\nWEAPONS", "ENERGY", "TARGETING", "WINGMEN" };
static const char* const k_vw_cat_name[VW_CATS] = { "ship & weapons", "energy", "targeting", "wingmen" };
static const char* const k_vw_levels[5] = { "off", "low", "normal", "high", "max" };
static const char* const k_vw_modes[3] = { "front", "even", "rear" };

static void vw_set_open(int open) {
	if (s_vw.open && !open)
		s_vw.hash_valid = 0;
	s_vw.open = open;
}

static void vw_close(void) {
	vw_set_open(0);
	s_vw.hover = -1;
	s_vw.hit = 0;
}

static VcButton* vw_item(const char* label, TieVrConsoleAction action, int arg) {
	if (s_vw.item_count >= VW_MAX_ITEMS) {
		static VcButton spare;
		memset(&spare, 0, sizeof spare);
		return &spare;
	}
	VcButton* b = &s_vw.items[s_vw.item_count++];
	memset(b, 0, sizeof *b);
	b->label = label;
	b->action = action;
	b->arg = arg;
	b->enabled = 1;
	return b;
}

static VcButton* vw_key(const char* label, TieInputAction action) {
	return vw_item(label, TIE_VRC_INPUT, (int)action);
}

static int vw_clamp(int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; }

/* The current level's items with their live state, the first at 12
 * o'clock, then clockwise. */
static void vw_layout(void) {
	const TieVrConsoleState* st = &s_vc.state;
	VcButton* b;
	s_vw.item_count = 0;
	switch (s_vw.level) {
		case 0:
			for (int c = 0; c < VW_CATS; ++c) {
				b = vw_item(k_vw_cat_label[c], TIE_VRC_NONE, c);
				b->font_size = 30.0f;
			}
			break;
		case 1 + VW_CAT_SHIP:
			b = vw_key("MATCH\nSPEED", TIE_INPUT_ACTION_MATCH_SPEED);
			b->enabled = st->has_target;
			vw_key("CHANGE\nWEAPON", TIE_INPUT_ACTION_CYCLE_WEAPON_GROUP);
			vw_key("FIRING\nMODE", TIE_INPUT_ACTION_CYCLE_WEAPON_FIRING_MODE);
			b = vw_key("BEAM\nON / OFF", TIE_INPUT_ACTION_TOGGLE_BEAM);
			b->enabled = st->has_beam;
			b->lit = st->beam_on;
			b = vw_key("HYPER-\nSPACE", TIE_INPUT_ACTION_HYPERSPACE);
			b->enabled = st->has_hyperdrive;
			vw_key("MAP", TIE_INPUT_ACTION_INFO_MAP);
			vw_key("GOALS", TIE_INPUT_ACTION_INFO_GOALS);
			vw_key("FULL\nTHROTTLE", TIE_INPUT_ACTION_THROTTLE_FULL);
			vw_key("ZERO\nTHROTTLE", TIE_INPUT_ACTION_THROTTLE_ZERO);
			vw_key("END\nMISSION", TIE_INPUT_ACTION_END_MISSION);
			break;
		case 1 + VW_CAT_ENERGY:
			/* Recharge rates step up one level a pick (MAX wraps to OFF, as
			 * the keys do). */
			b = vw_key("LASERS TO\nSHIELDS", TIE_INPUT_ACTION_XFER_CANNON_TO_SHIELDS);
			b->enabled = st->has_shields;
			b = vw_item("LASER\nRECHARGE", TIE_VRC_LASER_LEVEL, (vw_clamp(st->laser_level, 4) + 1) % 5);
			b->enabled = st->has_lasers;
			b->sub = k_vw_levels[vw_clamp(st->laser_level, 4)];
			b = vw_item("SHIELD\nRECHARGE", TIE_VRC_SHIELD_LEVEL, (vw_clamp(st->shield_level, 4) + 1) % 5);
			b->enabled = st->has_shields;
			b->sub = st->has_shields ? k_vw_levels[vw_clamp(st->shield_level, 4)] : NULL;
			b = vw_item("BEAM\nRECHARGE", TIE_VRC_BEAM_LEVEL, (vw_clamp(st->beam_level, 4) + 1) % 5);
			b->enabled = st->beam_on;
			b->sub = st->beam_on ? k_vw_levels[vw_clamp(st->beam_level, 4)] : NULL;
			b = vw_item("SHIELD\nDIRECTION", TIE_VRC_SHIELD_MODE, (vw_clamp(st->shield_mode, 2) + 1) % 3);
			b->enabled = st->has_shields;
			b->sub = st->has_shields ? k_vw_modes[vw_clamp(st->shield_mode, 2)] : NULL;
			b = vw_key("SHIELDS TO\nLASERS", TIE_INPUT_ACTION_XFER_SHIELDS_TO_CANNON);
			b->enabled = st->has_shields;
			break;
		case 1 + VW_CAT_TARGETING:
			vw_key("NEAREST\nFIGHTER", TIE_INPUT_ACTION_TARGET_NEAREST_FIGHTER_OR_MINE);
			vw_key("NEXT\nTARGET", TIE_INPUT_ACTION_TARGET_NEXT);
			vw_key("MY\nATTACKER", TIE_INPUT_ACTION_TARGET_MY_ATTACKER);
			b = vw_key("ATTACKING\nMY TARGET", TIE_INPUT_ACTION_TARGET_ATTACKER_CHAIN);
			b->enabled = st->has_target;
			vw_key("NEWEST", TIE_INPUT_ACTION_TARGET_NEWEST_CRAFT);
			b = vw_key("NEXT\nCOMPONENT", TIE_INPUT_ACTION_TARGET_COMPONENT_NEXT);
			b->enabled = st->has_target;
			vw_key("IN SIGHTS", TIE_INPUT_ACTION_AUTO_TARGET);
			vw_key("PREVIOUS\nTARGET", TIE_INPUT_ACTION_TARGET_PREV);
			break;
		case 1 + VW_CAT_WINGMEN:
			b = vw_key("ATTACK\nMY TARGET", TIE_INPUT_ACTION_COMM_ASSIGN_TARGET);
			b->enabled = st->has_target;
			vw_key("COVER ME", TIE_INPUT_ACTION_COMM_COVER_ME);
			b = vw_key("EVADE", TIE_INPUT_ACTION_COMM_EVADE);
			b->enabled = st->has_target;
			b = vw_key("WAIT", TIE_INPUT_ACTION_COMM_STOP_AND_WAIT);
			b->enabled = st->has_target;
			b = vw_key("GO AHEAD", TIE_INPUT_ACTION_COMM_CONTINUE_MISSION);
			b->enabled = st->has_target;
			b = vw_key("GO HOME", TIE_INPUT_ACTION_COMM_HEAD_HOME);
			b->enabled = st->has_target;
			b = vw_key("REPORT", TIE_INPUT_ACTION_COMM_REPORT_ORDERS);
			b->enabled = st->has_target;
			vw_key("RESUPPLY", TIE_INPUT_ACTION_COMM_REQUEST_RESUPPLY);
			vw_key("REINFORCE", TIE_INPUT_ACTION_COMM_REINFORCEMENTS);
			b = vw_key("IGNORE\nMY TARGET", TIE_INPUT_ACTION_COMM_IGNORE_TARGET);
			b->enabled = st->has_target;
			break;
		default:
			s_vw.level = 0;
			break;
	}
	const int n = s_vw.item_count;
	const float ring = n <= 4 ? 222.0f : n <= 8 ? 266.0f : 278.0f;
	const float bw = n <= 4 ? 232.0f : n <= 8 ? 172.0f : 148.0f;
	const float bh = n <= 4 ? 124.0f : n <= 8 ? 90.0f : 80.0f;
	for (int i = 0; i < n; ++i) {
		const float a = (float)i * 2.0f * VC_PI / (float)n;
		VcButton* it = &s_vw.items[i];
		it->x = VW_W * 0.5f + ring * sinf(a) - bw * 0.5f;
		it->y = VW_H * 0.5f - ring * cosf(a) - bh * 0.5f;
		it->w = bw;
		it->h = bh;
		it->radius = 12.0f;
		it->base_fill = 0.18f;
		if (it->font_size <= 0.0f)
			it->font_size = n <= 8 ? 24.0f : 22.0f;
	}
	VcButton* c = &s_vw.center_button;
	memset(c, 0, sizeof *c);
	c->x = VW_W * 0.5f - VW_CENTER_R;
	c->y = VW_H * 0.5f - VW_CENTER_R;
	c->w = c->h = 2.0f * VW_CENTER_R;
	c->radius = VW_CENTER_R;
	c->base_fill = 0.18f;
	c->enabled = 1;
	c->font_size = 28.0f;
	if (s_vw.level == 0) {
		c->label = "CONFIRM";
		c->sub = "space";
		c->action = TIE_VRC_INPUT;
		c->arg = TIE_INPUT_ACTION_CONFIRM;
		c->lit = st->confirm_offered;
	} else {
		c->label = "BACK";
		c->sub = k_vw_cat_name[s_vw.level - 1];
	}
}

/* The slice the ray picks at (u, v) metres from the centre (+u right, +v
 * up), the centre, or -1. Keeps the current pick a little past its edge. */
static int vw_pick(float u, float v) {
	const float r = sqrtf(u * u + v * v);
	const float center_m = VW_CENTER_R / VW_PX_PER_M;
	if (r < (s_vw.hover == VW_CENTER ? center_m * 1.2f : center_m))
		return VW_CENTER;
	const int n = s_vw.item_count;
	if (r > VW_SELECT_MAX_M || n <= 0)
		return -1;
	const float step = 360.0f / (float)n;
	float a = atan2f(u, v) * 180.0f / VC_PI;
	if (a < 0.0f)
		a += 360.0f;
	int pick = (int)floorf((a + 0.5f * step) / step) % n;
	if (s_vw.hover >= 0 && s_vw.hover < n && pick != s_vw.hover) {
		float d = fmodf(fabsf(a - step * (float)s_vw.hover), 360.0f);
		if (d > 180.0f)
			d = 360.0f - d;
		if (d < 0.5f * step + VW_HYST_DEG)
			pick = s_vw.hover;
	}
	return pick;
}

/* The trigger on the current pick. */
static void vw_activate(void) {
	const int hand = s_vw.hand;
	if (s_vw.hover == VW_CENTER) {
		if (s_vw.center_lock)
			return; /* just came back here: point away first */
		if (s_vw.level == 0) {
			vc_push(TIE_VRC_INPUT, TIE_INPUT_ACTION_CONFIRM);
			s_vw.flash[VW_MAX_ITEMS] = VC_FLASH_S;
			Aeron_XrHapticPulse(hand, 0.6f, 30);
		} else {
			s_vw.level = 0;
			s_vw.anim = fminf(s_vw.anim, 0.6f);
			s_vw.center_lock = 1;
			Aeron_XrHapticPulse(hand, 0.4f, 25);
		}
		return;
	}
	if (s_vw.hover < 0 || s_vw.hover >= s_vw.item_count) {
		/* Pointing at no slice: the trigger is grip + left trigger as
		 * without the wheel (the wheel owns that trigger while open). */
		const TieInputAction shifted = TieVrButtons_Action(AERON_XR_BUTTON_LEFT_TRIGGER, true);
		if (shifted != TIE_INPUT_ACTION_NONE) {
			vc_push(TIE_VRC_INPUT, (int)shifted);
			Aeron_XrHapticPulse(hand, 0.15f, 15);
		}
		return;
	}
	const VcButton* b = &s_vw.items[s_vw.hover];
	if (s_vw.level == 0) {
		s_vw.level = 1 + b->arg;
		s_vw.anim = fminf(s_vw.anim, 0.6f);
		s_vw.hover = -1;
		Aeron_XrHapticPulse(hand, 0.4f, 25);
		return;
	}
	if (!b->enabled || b->action == TIE_VRC_NONE) {
		Aeron_XrHapticPulse(hand, 0.1f, 10);
		return;
	}
	vc_push(b->action, b->arg);
	s_vw.flash[s_vw.hover] = VC_FLASH_S;
	Aeron_XrHapticPulse(hand, 0.6f, 30);
}

/* Once a frame (after the console and the side panel had their turn with
 * the grips): opens, steers and closes. */
static void vw_update(const AeronXrView views[2]) {
	static const float minus_z[3] = { 0.0f, 0.0f, -1.0f };
	const int hand = Aeron_XrLeftHanded() ? 1 : 0;
	AeronXrHand xh;
	memset(&xh, 0, sizeof xh);
	const int tracked = Aeron_XrGetHand(hand, &xh) && xh.valid;
	s_vw.drawn = 1;
	if (s_vw.open && s_vw.hand != hand)
		vw_close();
	const int was_held = s_vw.grip_held;
	s_vw.grip_held = tracked && (was_held ? xh.grip > VC_GRIP_OFF : xh.grip >= VC_GRIP_ON);
	if (!s_vc.state.valid || !vw_enabled() || s_vc.font_state != 1 || !tracked) {
		vw_close();
		return;
	}
	const uint32_t serial = TieVrControls_ShiftUseSerial();
	if (!s_vw.open) {
		if (!s_vw.grip_held)
			return;
		if (!was_held) {
			/* A squeeze on a panel's handle or the flight stick is theirs. */
			s_vw.vetoed = s_vc.grip_capture[hand] || s_vc.grip_capture_prev[hand] || TieVrHands_GripCaptured(hand) ||
						  s_vc.lap.grab_hand == hand + 1 || s_vc.lap.hover_side[hand] ||
						  s_vc.card.place.grab_hand == hand + 1 || s_vc.card.place.hover_side[hand];
			s_vw.held_s = 0.0f;
			s_vw.serial = serial;
			return; /* squeezed during the last frame: start counting now */
		}
		/* The grip is TIE's shift layer: a grip + button chord in this
		 * squeeze, and it is not the wheel's. */
		if (serial != s_vw.serial)
			s_vw.vetoed = 1;
		s_vw.held_s += s_vc.wall_dt;
		if (s_vw.vetoed || s_vw.held_s < TIE_VR_WHEEL_DELAY_S)
			return;
		vw_set_open(1);
		s_vw.hand = hand;
		s_vw.level = 0;
		s_vw.hover = -1;
		s_vw.armed = xh.trigger < VW_TRIGGER_OFF; /* a held trigger must be let go first */
		s_vw.center_lock = 0;
		s_vw.anim = 0.0f;
		s_vw.fwd[0] = s_vw.fwd[1] = s_vw.fwd[2] = 0.0f;
		memset(s_vw.flash, 0, sizeof s_vw.flash);
		s_vw.hash_valid = 0;
		Aeron_XrHapticPulse(hand, 0.3f, 20);
		if (!s_vw.logged) {
			Aeron_LogInfo("tie.vrconsole", "quick wheel: %s grip, %.2f m ahead", hand ? "right" : "left",
						  (double)VW_AHEAD_M);
			s_vw.logged = 1;
		}
	} else if (!s_vw.grip_held || serial != s_vw.serial) {
		/* Let go, or a grip + button chord pressed while open (it acted). */
		vw_close();
		s_vw.vetoed = s_vw.grip_held;
		return;
	}
	s_vw.anim = s_vc.dt > 0.0f ? fminf(1.0f, s_vw.anim + s_vc.dt / VW_GROW_S) : 1.0f;
	for (int i = 0; i <= VW_MAX_ITEMS; ++i)
		if (s_vw.flash[i] > 0.0f)
			s_vw.flash[i] -= s_vc.dt;

	/* Facing the way the head looked when it opened (level). */
	{
		float f[3];
		vc_rotate(views[0].orientation, minus_z, f);
		const float len = sqrtf(f[0] * f[0] + f[2] * f[2]);
		const int fresh = s_vw.fwd[0] == 0.0f && s_vw.fwd[2] == 0.0f;
		if (fresh && len > 0.2f) {
			s_vw.fwd[0] = f[0] / len;
			s_vw.fwd[2] = f[2] / len;
		} else if (fresh) {
			s_vw.fwd[2] = -1.0f;
		}
		const float l = sqrtf(s_vw.fwd[0] * s_vw.fwd[0] + s_vw.fwd[2] * s_vw.fwd[2]);
		if (l > 1e-4f) {
			s_vw.fwd[0] /= l;
			s_vw.fwd[2] /= l;
		} else {
			s_vw.fwd[0] = 0.0f;
			s_vw.fwd[2] = -1.0f;
		}
		s_vw.fwd[1] = 0.0f;
	}
	const float az[3] = { -s_vw.fwd[0], 0.0f, -s_vw.fwd[2] }; /* towards the player */
	const float ax[3] = { az[2], 0.0f, -az[0] };
	const float ay[3] = { 0.0f, 1.0f, 0.0f };
	memcpy(s_vw.az, az, sizeof az);
	memcpy(s_vw.ax, ax, sizeof ax);
	memcpy(s_vw.ay, ay, sizeof ay);
	vc_basis_quat(ax, ay, az, s_vw.q);
	for (int k = 0; k < 3; ++k)
		s_vw.center[k] = xh.position[k] + s_vw.fwd[k] * VW_AHEAD_M;

	/* Where the controller points. */
	vw_layout();
	float d[3];
	vc_rotate(xh.orientation, minus_z, d);
	for (int k = 0; k < 3; ++k)
		s_vw.ray_start[k] = xh.position[k] + d[k] * VC_TIP_M;
	int hover = -1;
	s_vw.hit = 0;
	const float dn = vc_dot(d, az);
	if (dn < -0.15f) {
		const float rel[3] = { s_vw.center[0] - xh.position[0], s_vw.center[1] - xh.position[1],
							   s_vw.center[2] - xh.position[2] };
		const float t = vc_dot(rel, az) / dn;
		if (t > 0.0f && t < 1.5f) {
			float off[3];
			for (int k = 0; k < 3; ++k) {
				s_vw.ray_end[k] = xh.position[k] + d[k] * t;
				off[k] = s_vw.ray_end[k] - s_vw.center[k];
			}
			const float u = vc_dot(off, ax), v = vc_dot(off, ay);
			hover = vw_pick(u, v);
			/* The pointer stops at the wheel's rim. */
			const float rim = (VW_PLATE_R - 14.0f) / VW_PX_PER_M;
			const float r = sqrtf(u * u + v * v);
			const float k = r > rim ? rim / r : 1.0f;
			const float cu = u * k, cv = v * k;
			for (int c = 0; c < 3; ++c)
				s_vw.ray_end[c] = s_vw.center[c] + ax[c] * cu + ay[c] * cv;
			s_vw.hit = 1;
			s_vw.hit_px = (cu / VW_W_M + 0.5f) * VW_W;
			s_vw.hit_py = (0.5f - cv / VW_H_M) * VW_H;
		}
	}
	if (!s_vw.hit)
		for (int k = 0; k < 3; ++k)
			s_vw.ray_end[k] = s_vw.ray_start[k] + d[k] * 0.25f;
	if (hover != s_vw.hover) {
		if (hover != -1)
			Aeron_XrHapticPulse(hand, 0.25f, 25); /* a tick per slice */
		s_vw.hover = hover;
	}
	if (s_vw.hover != VW_CENTER)
		s_vw.center_lock = 0;

	/* The trigger picks; it must be let go before it picks again. */
	if (!s_vw.armed) {
		s_vw.armed = xh.trigger < VW_TRIGGER_OFF;
	} else if (xh.trigger >= VW_TRIGGER_ON) {
		s_vw.armed = 0;
		vw_activate();
	}
	vw_layout(); /* the level may have changed */
}

static void vw_draw(void) {
	const float cx = VW_W * 0.5f, cy = VW_H * 0.5f, R = VW_PLATE_R;
	const float transparent[4] = { 0, 0, 0, 0 };
	float fill[4], border[4], glow[4], line[4];
	vc_hud_colors();
	vc_pma(glow, VC_HUD, 0.25f);
	vc_rrect(cx - R - 6.0f, cy - R - 6.0f, 2.0f * R + 12.0f, 2.0f * R + 12.0f, R + 6.0f, 8.0f, 12.0f, transparent, glow);
	vc_pma(fill, s_vc.hud_dark[0] * 0.22f, s_vc.hud_dark[1] * 0.22f, s_vc.hud_dark[2] * 0.22f, 0.62f);
	vc_pma(border, VC_HUD, 0.7f);
	vc_rrect(cx - R, cy - R, 2.0f * R, 2.0f * R, R, 2.0f, 1.0f, fill, border);
	vc_pma(line, VC_HUD, 0.03f);
	for (float y = cy - R + 10.0f; y < cy + R - 10.0f; y += 6.0f) {
		const float dy = y + 1.0f - cy;
		const float half = sqrtf(fmaxf(0.0f, (R - 8.0f) * (R - 8.0f) - dy * dy));
		if (half > 4.0f)
			vc_fill(cx - half, y, 2.0f * half, 2.0f, line);
	}
	/* A faint track inside the rim, lit along the picked slice. */
	const float track = R - 16.0f;
	vc_pma(border, VC_HUD, 0.25f);
	vc_rrect(cx - track - 1.0f, cy - track - 1.0f, 2.0f * track + 2.0f, 2.0f * track + 2.0f, track + 1.0f, 2.0f, 1.0f,
			 transparent, border);
	const int n = s_vw.item_count;
	if (s_vw.hover >= 0 && s_vw.hover < n) {
		const float step = 2.0f * VC_PI / (float)n;
		const float a0 = ((float)s_vw.hover - 0.5f) * step + 0.02f, a1 = ((float)s_vw.hover + 0.5f) * step - 0.02f;
		const int segs = 24;
		vc_pma(line, VC_HUD, 0.9f);
		for (int i = 0; i < segs; ++i) {
			const float b0 = a0 + (a1 - a0) * (float)i / (float)segs;
			const float b1 = a0 + (a1 - a0) * (float)(i + 1) / (float)segs;
			vc_line(cx + track * sinf(b0), cy - track * cosf(b0), cx + track * sinf(b1), cy - track * cosf(b1), 9.0f,
					line);
		}
	}
	for (int i = 0; i < n; ++i)
		vc_draw_button_look(&s_vw.items[i], s_vw.hover == i, 0, s_vw.flash[i]);
	vc_draw_button_look(&s_vw.center_button, s_vw.hover == VW_CENTER && !s_vw.center_lock, 0,
						s_vw.flash[VW_MAX_ITEMS]);
}

/* Where the beam lands. */
static void vw_overlay(void) {
	vc_hud_colors();
	if (!s_vw.hit)
		return;
	float c[4], ring[4];
	vc_pma(c, 1.0f, 1.0f, 1.0f, 0.95f);
	vc_pma(ring, VC_HUD, 0.9f);
	vc_rrect(s_vw.hit_px - 10.0f, s_vw.hit_py - 10.0f, 20.0f, 20.0f, 10.0f, 3.0f, 1.5f, c, ring);
}

int TieVrConsole_WheelOpen(void) { return s_vw.open; }

int TieVrConsole_WheelFrame(AeronCommandBuffer* cmd, int width, int height, const AeronXrView views[2],
							AeronXrPanel out[2]) {
	if (!cmd || width <= 0 || height <= 0 || !views || !out)
		return 0;
	vw_update(views);
	if (!s_vw.open || !vc_ensure())
		return 0;
	if (!s_vw.list[0] && !(s_vw.list[0] = AeronDrawList_Create(0)))
		return 0;
	/* Grows in (and pops on a level change). */
	const float t = s_vw.anim;
	const float ease = t * t * (3.0f - 2.0f * t);
	const float scale = 0.7f + 0.3f * ease;
	memset(out, 0, sizeof *out);
	memcpy(out->position, s_vw.center, sizeof s_vw.center);
	memcpy(out->orientation, s_vw.q, sizeof s_vw.q);
	out->size[0] = VW_W_M * scale;
	out->size[1] = VW_H_M * scale;
	s_vc.cur_place = NULL;
	s_vc.cur_w_px = (float)width;
	s_vc.cur_h_px = (float)height;
	s_alpha_draw = 0.3f + 0.7f * ease;
	return vc_render_panel(cmd, AERON_XR_PANEL_CARD2, s_vw.list[0], vw_draw, vw_overlay, views, out, width, height,
						   out->size[0], out->size[1], &s_vw.hash, &s_vw.hash_valid, 0);
}

/* ---- Side panel: tabs ------------------------------------------------- */

enum { VP_TAB_MAP, VP_TAB_VOICE, VP_TAB_BUTTONS, VP_TABS };
static const char* const k_vp_tab_names[VP_TABS] = { "TACTICAL MAP", "VOICE COMMANDS", "BUTTON MAPPING" };
static const char* const k_vp_tab_keys[VP_TABS] = { "map", "voice", "buttons" };
static const float k_vp_ranges_km[] = { 1.0f, 3.0f, 10.0f };
#define VP_TAB_TOP 18.0f
#define VP_TAB_H 58.0f
#define VP_HIT_HELP (VP_TABS + 1)
#define VP_HELP_X(w) ((w) - 92.0f)
#define VP_HELP_Y 88.0f
#define VP_HELP_W 60.0f
#define VP_HELP_H 48.0f
static struct {
	int loaded;
	int tab;
	int zoom;
	int hover[2]; /* tab under each hand's cursor, -1 none; VP_TABS = the map */
	int poke_down[2];
	int trigger_down[2];
	int ray[2];
	float ray_start[2][3], ray_end[2][3];
	float cx[2], cy[2]; /* the ray's dot, panel pixels */
	int captured[2];
	int tab_dirty;
	int map_help;
} s_vp = { .zoom = 1, .hover = { -1, -1 } };

static void vp_load(void) {
	if (s_vp.loaded)
		return;
	s_vp.loaded = 1;
	const char* legend = SDL_getenv("XWA_XR_MAP_LEGEND"); /* 1: open at start (tests) */
	s_vp.map_help = legend && legend[0] == '1';
	const char* env = SDL_getenv("XWA_XR_PANEL_TAB");
	for (int t = 0; env && t < VP_TABS; ++t)
		if (strcmp(env, k_vp_tab_keys[t]) == 0)
			s_vp.tab = t;
}

int TieVrConsole_TakePanelTab(char* out, int size) {
	if (!s_vp.tab_dirty || !out || size <= 0)
		return 0;
	s_vp.tab_dirty = 0;
	snprintf(out, (size_t)size, "%s", k_vp_tab_keys[s_vp.tab]);
	return 1;
}

/* What lies at panel pixel (x, y): a tab, VP_TABS for the map area,
 * VP_HIT_HELP for the map's "?", -1. */
static int vp_hit(float x, float y, float w, float h) {
	if (x < 0.0f || y < 0.0f || x >= w || y >= h)
		return -1;
	if (s_vp.tab == VP_TAB_MAP && x >= VP_HELP_X(w) && x < VP_HELP_X(w) + VP_HELP_W && y >= VP_HELP_Y &&
		y < VP_HELP_Y + VP_HELP_H)
		return VP_HIT_HELP;
	if (y >= VP_TAB_TOP && y < VP_TAB_TOP + VP_TAB_H) {
		const float tw = (w - 48.0f) / (float)VP_TABS;
		const int t = (int)((x - 24.0f) / tw);
		return x >= 24.0f && t >= 0 && t < VP_TABS ? t : -1;
	}
	return s_vp.tab == VP_TAB_MAP && y > VP_TAB_TOP + VP_TAB_H + 8.0f ? VP_TABS : -1;
}

static void vp_press(int hand, int what) {
	if (what < 0)
		return;
	if (what == VP_HIT_HELP) {
		s_vp.map_help = !s_vp.map_help;
	} else if (what < VP_TABS) {
		if (what != s_vp.tab) {
			s_vp.tab = what;
			s_vp.tab_dirty = 1;
			SDL_setenv_unsafe("XWA_XR_PANEL_TAB", k_vp_tab_keys[what], 1);
		}
	} else if (s_vp.map_help) {
		s_vp.map_help = 0; /* a touch on the legend closes it */
	} else {
		s_vp.zoom = (s_vp.zoom + 1) % (int)(sizeof k_vp_ranges_km / sizeof k_vp_ranges_km[0]);
	}
	Aeron_XrHapticPulse(hand, 0.35f, 15);
}

static void vp_update_hands(const AeronXrPanel* pose, const float ax[3], const float ay[3], const float az[3],
							float w_m, float h_m, float w_px, float h_px) {
	for (int hand = 0; hand < 2; ++hand) {
		AeronXrHand xh;
		s_vp.hover[hand] = -1;
		s_vp.captured[hand] = 0;
		s_vp.ray[hand] = 0;
		if (!s_vc.card.awake || s_vc.card.place.grab_hand == hand + 1 || !Aeron_XrGetHand(hand, &xh) ||
			!xh.valid) {
			s_vp.poke_down[hand] = s_vp.trigger_down[hand] = 0;
			continue;
		}
		float fwd[3], tip[3];
		vc_hand_ray(&xh, fwd, tip);
		const float rel[3] = { tip[0] - pose->position[0], tip[1] - pose->position[1], tip[2] - pose->position[2] };
		const float depth = vc_dot(rel, az);
		const float px = (vc_dot(rel, ax) / w_m + 0.5f) * w_px;
		const float py = (0.5f - vc_dot(rel, ay) / h_m) * h_px;
		int what = -1, poke = 0;
		if (depth < VC_HOVER_M && depth > -VC_BLOCK_M) {
			what = vp_hit(px, py, w_px, h_px);
			poke = what >= 0 && depth < -VC_PRESS_M;
		}
		int ray = 0;
		if (what < 0) {
			float rx, ry, hit[3];
			if (vc_ray_hit(xh.position, fwd, pose, ax, ay, az, w_m, h_m, w_px, h_px, &rx, &ry, hit)) {
				ray = 1;
				what = vp_hit(rx, ry, w_px, h_px);
				s_vp.ray[hand] = 1;
				memcpy(s_vp.ray_start[hand], tip, sizeof tip);
				memcpy(s_vp.ray_end[hand], hit, sizeof hit);
				s_vp.cx[hand] = rx;
				s_vp.cy[hand] = ry;
			}
		}
		s_vp.hover[hand] = what;
		if (s_vp.poke_down[hand]) {
			s_vp.poke_down[hand] = what >= 0 && depth < -VC_RELEASE_M;
		} else if (poke) {
			s_vp.poke_down[hand] = 1;
			vp_press(hand, what);
		}
		if (s_vp.trigger_down[hand]) {
			s_vp.trigger_down[hand] = xh.trigger >= 0.35f;
		} else if (ray && xh.trigger >= 0.6f) {
			s_vp.trigger_down[hand] = 1;
			vp_press(hand, what);
		}
		s_vp.captured[hand] = ray || s_vp.trigger_down[hand];
	}
}

static void vp_draw_tabs(void) {
	const float W = s_vc.cur_w_px;
	const float tw = (W - 48.0f) / (float)VP_TABS;
	for (int t = 0; t < VP_TABS; ++t) {
		const float x = 24.0f + tw * (float)t + 4.0f, w = tw - 8.0f;
		const int active = t == s_vp.tab;
		const int hover = s_vp.hover[0] == t || s_vp.hover[1] == t;
		float fill[4], border[4];
		if (active) {
			vc_pma(fill, VC_HUD, 0.45f);
			vc_pma(border, VC_HUD, 1.0f);
		} else {
			vc_pma(fill, VC_HUD, hover ? 0.22f : 0.06f);
			vc_pma(border, VC_HUD, hover ? 0.9f : 0.45f);
		}
		vc_rrect(x, VP_TAB_TOP, w, VP_TAB_H, 14.0f, 2.0f, 1.0f, fill, border);
		float size = 24.0f;
		const float tw_px = vc_text_width(k_vp_tab_names[t], size);
		if (tw_px > w - 16.0f && tw_px > 0.0f)
			size *= (w - 16.0f) / tw_px;
		if (active)
			vc_text(x + w * 0.5f, VP_TAB_TOP + 29.0f - size * 0.55f, k_vp_tab_names[t], size, 1, 1.0f, 1.0f, 1.0f, 1.0f);
		else
			vc_text(x + w * 0.5f, VP_TAB_TOP + 29.0f - size * 0.55f, k_vp_tab_names[t], size, 1, VC_HUD_PALE, 0.9f);
	}
}

/* ---- Side panel: tactical map -------------------------------------------- */

/* TIE's IFF colours (the message bar's eventsidecolors[] in the palette:
 * 0x52, 0x4A, 0x46, 0x56), from the snapshot's palette when it has one. */
static void vp_iff_color(const TieSnapshot* snap, int side, float out[3]) {
	static const uint8_t k_index[4] = { 0x52, 0x4A, 0x46, 0x56 };
	static const float k_fallback[4][3] = {
		{ 0.35f, 1.00f, 0.35f }, { 1.00f, 0.30f, 0.25f }, { 0.40f, 0.60f, 1.00f }, { 1.00f, 0.95f, 0.30f },
	};
	const int i = side >= 0 && side < 4 ? side : 2;
	const uint32_t argb = snap ? snap->palette[k_index[i]] : 0;
	const float r = (float)((argb >> 16) & 255u) / 255.0f, g = (float)((argb >> 8) & 255u) / 255.0f,
				b = (float)(argb & 255u) / 255.0f;
	if (r + g + b > 0.3f) {
		/* Lift the palette colour to full brightness: readable on the holo. */
		const float peak = fmaxf(r, fmaxf(g, b));
		out[0] = r / peak;
		out[1] = g / peak;
		out[2] = b / peak;
	} else {
		memcpy(out, k_fallback[i], sizeof k_fallback[i]);
	}
}

/* A contact's symbol: 0 craft, 1 freighter / transport, 2 capital ship /
 * station, 3 mine, 4 small object (container, buoy, satellite, probe),
 * 5 missile, 6 torpedo / bomb. */
static void vp_symbol(int kind, float px, float py, const float cc[4]) {
	switch (kind) {
		case 0:
			vc_rrect(px - 6.0f, py - 6.0f, 12.0f, 12.0f, 6.0f, 0.0f, 1.0f, cc, NULL);
			break;
		case 1:
			vc_rrect(px - 9.0f, py - 7.0f, 18.0f, 14.0f, 3.0f, 0.0f, 1.0f, cc, NULL);
			break;
		case 2:
			vc_rrect(px - 16.0f, py - 8.0f, 32.0f, 16.0f, 3.0f, 0.0f, 1.0f, cc, NULL);
			break;
		case 3:
			vc_line(px - 6.0f, py - 6.0f, px + 6.0f, py + 6.0f, 2.5f, cc);
			vc_line(px - 6.0f, py + 6.0f, px + 6.0f, py - 6.0f, 2.5f, cc);
			break;
		case 4: {
			const float d = 8.0f;
			vc_line(px, py - d, px + d, py, 2.5f, cc);
			vc_line(px + d, py, px, py + d, 2.5f, cc);
			vc_line(px, py + d, px - d, py, 2.5f, cc);
			vc_line(px - d, py, px, py - d, 2.5f, cc);
			break;
		}
		default:
			vc_line(px, py - 7.0f, px - 6.0f, py + 5.0f, 2.5f, cc);
			vc_line(px, py - 7.0f, px + 6.0f, py + 5.0f, 2.5f, cc);
			vc_line(px - 6.0f, py + 5.0f, px + 6.0f, py + 5.0f, 2.5f, cc);
			break;
	}
}

/* A flight object's map symbol, or -1 when it is not shown (lasers,
 * debris, explosions). */
static int vp_kind(const TieFlightObjectState* f) {
	const int s = f->ship_idx;
	if (f->flags & (TIE_FOBJ_DESTROYED | TIE_FOBJ_INVISIBLE))
		return -1;
	switch (s) {
		case TIE_SPECIES_PROJECTILE_CONCUSSION_MISSILE:
		case TIE_SPECIES_PROJECTILE_ADVANCED_CONCUSSION_MISSILE:
		case TIE_SPECIES_PROJECTILE_MAGNETIC_PULSE:
		case TIE_SPECIES_PROJECTILE_MAGNETIC_PULSE_2:
		case TIE_SPECIES_PROJECTILE_MAGNETIC_PULSE_3:
			return 5;
		case TIE_SPECIES_PROJECTILE_PROTON_TORPEDO:
		case TIE_SPECIES_PROJECTILE_ADVANCED_PROTON_TORPEDO:
		case TIE_SPECIES_PROJECTILE_SPACE_BOMB:
		case TIE_SPECIES_PROJECTILE_HEAVY_ROCKET:
			return 6;
		default:
			break;
	}
	if ((s >= TIE_SPECIES_CONTAINER_A && s <= TIE_SPECIES_CONTAINER_D) ||
		(s >= TIE_SPECIES_CONTAINER_E && s <= TIE_SPECIES_CONTAINER_I) ||
		(s >= TIE_SPECIES_COMM_SATELLITE_1 && s <= TIE_SPECIES_COMM_SATELLITE_5) ||
		(s >= TIE_SPECIES_PROBE_1 && s <= TIE_SPECIES_NAV_BUOY_2))
		return 4;
	if (s >= TIE_SPECIES_MINE_1 && s <= TIE_SPECIES_MINE_5)
		return 3;
	switch (f->genus) {
		case TIE_GENUS_FIGHTER:
			return 0;
		case TIE_GENUS_TRANSPORT:
		case TIE_GENUS_UTILITY:
		case TIE_GENUS_FREIGHTER:
			return 1;
		case TIE_GENUS_STARSHIP:
		case TIE_GENUS_PLATFORM:
			return 2;
		case TIE_GENUS_MINE:
			return 3;
		default:
			return -1;
	}
}

/* Craft-to-world rotation (snapshot quaternion w, x, y, z): columns side,
 * forward, up. */
static void vp_craft_axes(const float q[4], double side[3], double fwd[3], double up[3]) {
	const double w = q[0], x = q[1], y = q[2], z = q[3];
	side[0] = 1.0 - 2.0 * (y * y + z * z);
	side[1] = 2.0 * (x * y + w * z);
	side[2] = 2.0 * (x * z - w * y);
	fwd[0] = 2.0 * (x * y - w * z);
	fwd[1] = 1.0 - 2.0 * (x * x + z * z);
	fwd[2] = 2.0 * (y * z + w * x);
	up[0] = 2.0 * (x * z + w * y);
	up[1] = 2.0 * (y * z - w * x);
	up[2] = 1.0 - 2.0 * (x * x + y * y);
}

/* "\xFE<colour>T/F\xFE<colour>: ALPHA 1" -> "T/F: ALPHA 1". */
static void vp_plain_name(const char* in, char* out, size_t size) {
	size_t n = 0;
	for (size_t i = 0; in && in[i] && n + 1 < size; ++i) {
		if ((unsigned char)in[i] == 0xFE) {
			if (in[i + 1])
				++i;
			continue;
		}
		if ((unsigned char)in[i] >= 32 && (unsigned char)in[i] < 127)
			out[n++] = in[i];
	}
	out[n] = '\0';
}

static void vp_ellipse(float cx, float cy, float rx, float ry, float width, const float col[4]) {
	const int steps = 72;
	float px = cx + rx, py = cy;
	for (int i = 1; i <= steps; ++i) {
		const float a = (float)i * 2.0f * VC_PI / (float)steps;
		const float x = cx + rx * cosf(a), y = cy + ry * sinf(a);
		vc_line(px, py, x, y, width, col);
		px = x;
		py = y;
	}
}

static void vp_map_legend(void);

static void vp_map_draw(void) {
	const float W = s_vc.cur_w_px, H = s_vc.cur_h_px;
	const TieSnapshot* snap = TieSnapshot_Current();
	const float cx = W * 0.5f, cy = 452.0f;
	const float rx = 420.0f, ry = 230.0f; /* the plane, seen from above and behind */
	const float lift = 0.75f;             /* stalk pixels per plane pixel of height */
	const float range_km = k_vp_ranges_km[s_vp.zoom];
	float c[4];
	char text[96];
	for (int ring = 1; ring <= 3 && !s_vp.map_help; ++ring) {
		vc_pma(c, VC_HUD, ring == 3 ? 0.55f : 0.28f);
		vp_ellipse(cx, cy, rx * (float)ring / 3.0f, ry * (float)ring / 3.0f, 2.0f, c);
	}
	if (!s_vp.map_help) {
		vc_pma(c, VC_HUD, 0.22f);
		vc_line(cx - rx, cy, cx + rx, cy, 1.5f, c);
		vc_line(cx, cy - ry, cx, cy + ry, 1.5f, c);
	}
	snprintf(text, sizeof text, "RANGE %.0f KM", (double)range_km);
	vc_text(44.0f, 96.0f, text, 20.0f, 0, VC_HUD, 0.95f);
	{
		const int hover = s_vp.hover[0] == VP_HIT_HELP || s_vp.hover[1] == VP_HIT_HELP;
		float fill[4], border[4];
		vc_pma(fill, VC_HUD, s_vp.map_help ? 0.45f : hover ? 0.22f : 0.08f);
		vc_pma(border, VC_HUD, s_vp.map_help || hover ? 1.0f : 0.55f);
		vc_rrect(VP_HELP_X(W), VP_HELP_Y, VP_HELP_W, VP_HELP_H, 12.0f, 2.0f, 1.0f, fill, border);
		vc_text(VP_HELP_X(W) + 0.5f * VP_HELP_W, VP_HELP_Y + 8.0f, "?", 30.0f, 1, 1.0f, 1.0f, 1.0f, 1.0f);
	}
	if (s_vp.map_help) {
		vp_map_legend();
		return;
	}
	{
		char ring_text[16];
		snprintf(ring_text, sizeof ring_text, "%.1f", (double)(range_km / 3.0f));
		vc_text(cx + rx / 3.0f + 6.0f, cy + 4.0f, ring_text, 14.0f, 0, VC_HUD_PALE, 0.8f);
	}
	/* The own craft: a chevron at the centre, nose up. */
	{
		float own[4];
		vc_pma(own, 1.0f, 1.0f, 1.0f, 1.0f);
		vc_line(cx, cy - 14.0f, cx - 10.0f, cy + 8.0f, 3.0f, own);
		vc_line(cx, cy - 14.0f, cx + 10.0f, cy + 8.0f, 3.0f, own);
	}
	if (!snap || !snap->flights)
		return;
	const TieFlightObjectState* self = NULL;
	for (uint16_t i = 0; i < snap->flight_count && !self; ++i)
		if ((int)snap->flights[i].slot == s_vc.state.player_slot)
			self = &snap->flights[i];
	/* The craft's frame (or the camera's when it is not found). */
	double origin[3], side[3], fwd[3], up[3];
	if (self) {
		for (int k = 0; k < 3; ++k)
			origin[k] = (double)self->world_pos[k];
		vp_craft_axes(self->ori, side, fwd, up);
	} else {
		/* Camera rows: eye x right, y down, z ahead. */
		double cs[3], cd[3], cf[3];
		const float* q = snap->camera.ori;
		const double w = q[0], x = q[1], y = q[2], z = q[3];
		cs[0] = 1.0 - 2.0 * (y * y + z * z), cs[1] = 2.0 * (x * y - w * z), cs[2] = 2.0 * (x * z + w * y);
		cd[0] = 2.0 * (x * y + w * z), cd[1] = 1.0 - 2.0 * (x * x + z * z), cd[2] = 2.0 * (y * z - w * x);
		cf[0] = 2.0 * (x * z - w * y), cf[1] = 2.0 * (y * z + w * x), cf[2] = 1.0 - 2.0 * (x * x + y * y);
		for (int k = 0; k < 3; ++k) {
			origin[k] = (double)snap->camera.world_pos[k];
			side[k] = cs[k];
			fwd[k] = cf[k];
			up[k] = -cd[k];
		}
	}
	/* 1600/65536 m per world unit. */
	const double units = (double)range_km * 1000.0 * 65536.0 / 1600.0;
	float target_xy[2] = { 0.0f, 0.0f };
	int target_shown = 0;
	double target_dist = -1.0;
	for (uint16_t i = 0; i < snap->flight_count; ++i) {
		const TieFlightObjectState* f = &snap->flights[i];
		if (f == self)
			continue;
		const int kind = vp_kind(f);
		if (kind < 0)
			continue;
		const double d[3] = { (double)f->world_pos[0] - origin[0], (double)f->world_pos[1] - origin[1],
							  (double)f->world_pos[2] - origin[2] };
		const int is_target = (int)f->slot == s_vc.state.target_slot;
		if (is_target)
			target_dist = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		double mx = (d[0] * side[0] + d[1] * side[1] + d[2] * side[2]) / units;
		double mz = (d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2]) / units;
		double mh = (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) / units;
		const double flat = sqrt(mx * mx + mz * mz);
		int outside = 0;
		if (flat > 1.0) {
			if (kind >= 5)
				continue;
			mx /= flat;
			mz /= flat;
			mh = 0.0;
			outside = 1;
		}
		mh = mh > 1.0 ? 1.0 : mh < -1.0 ? -1.0 : mh;
		const float px = cx + (float)mx * rx;
		const float base_y = cy - (float)mz * ry;
		const float py = base_y - (float)mh * ry * lift;
		float col[3];
		vp_iff_color(snap, f->side, col);
		if (kind >= 5) {
			col[0] = kind == 5 ? 1.0f : 0.35f;
			col[1] = kind == 5 ? 0.25f : 0.55f;
			col[2] = kind == 5 ? 0.2f : 1.0f;
		}
		float cc[4];
		vc_pma(cc, col[0], col[1], col[2], outside ? 0.45f : 1.0f);
		if (!outside && fabsf(py - base_y) > 2.0f) {
			float stalk[4];
			vc_pma(stalk, col[0], col[1], col[2], 0.45f);
			vc_line(px, base_y, px, py, 1.5f, stalk);
			vc_fill(px - 3.0f, base_y - 1.0f, 6.0f, 2.0f, stalk);
		}
		vp_symbol(kind, px, py, cc);
		if (is_target) {
			target_xy[0] = px;
			target_xy[1] = py;
			target_shown = 1;
		}
	}
	/* The current target: corner brackets, its name and range below. */
	if (target_shown) {
		const float x = target_xy[0], y = target_xy[1], r = 20.0f, l = 8.0f;
		float b[4];
		vc_pma(b, 1.0f, 1.0f, 1.0f, 1.0f);
		static const float corners[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
		for (int k = 0; k < 4; ++k) {
			const float sx = corners[k][0], sy = corners[k][1];
			vc_line(x + sx * r, y + sy * r, x + sx * (r - l), y + sy * r, 2.5f, b);
			vc_line(x + sx * r, y + sy * r, x + sx * r, y + sy * (r - l), 2.5f, b);
		}
	}
	if (s_vc.state.has_target) {
		char name[40];
		vp_plain_name(snap->hud.target_name, name, sizeof name);
		if (target_dist >= 0.0)
			snprintf(text, sizeof text, "TARGET  %s   %.2f KM", name[0] ? name : "-",
					 target_dist * 1600.0 / 65536.0 / 1000.0);
		else
			snprintf(text, sizeof text, "TARGET  %s", name[0] ? name : "-");
		vc_text(W * 0.5f, H - 72.0f, text, 20.0f, 1, 1.0f, 1.0f, 1.0f, 0.95f);
	}
}

/* The map's legend ("?"), over the map. */
static void vp_map_legend(void) {
	const float W = s_vc.cur_w_px;
	const float x = 60.0f, y = 146.0f, w = W - 120.0f, h = 568.0f;
	float fill[4], border[4], c[4];
	vc_pma(fill, 0.02f, 0.07f, 0.04f, 0.94f);
	vc_pma(border, VC_HUD, 0.8f);
	vc_rrect(x, y, w, h, 18.0f, 2.0f, 1.0f, fill, border);
	vc_text(x + 28.0f, y + 20.0f, "HOW TO READ THE MAP", 24.0f, 0, VC_HUD, 1.0f);
	static const char* const k_lines[] = {
		"You are the arrow in the middle; up is straight ahead.",
		"The oval is the plane of your wings; rings at 1/3, 2/3, full range.",
		"A line up or down: the contact is above or below that plane.",
		"Colours as the targeting computer. Brackets: your target.",
		"Faded at the edge: further away than the range.",
		"Touch the map to zoom (1 / 3 / 10 km).",
	};
	float ty = y + 66.0f;
	for (size_t i = 0; i < sizeof k_lines / sizeof k_lines[0]; ++i) {
		vc_text(x + 28.0f, ty, k_lines[i], 19.0f, 0, 0.88f, 0.97f, 1.0f, 1.0f);
		ty += 34.0f;
	}
	static const struct {
		int kind;
		const char* text;
	} k_syms[] = {
		{ 0, "fighter" }, { 1, "transport, freighter" }, { 2, "capital ship, station" }, { 3, "mine" },
		{ 4, "container, buoy, satellite" }, { 5, "missile" }, { 6, "torpedo, bomb" },
	};
	ty += 14.0f;
	for (size_t i = 0; i < sizeof k_syms / sizeof k_syms[0]; ++i) {
		const float sx = x + 50.0f + (float)(i % 2) * (w * 0.5f);
		const float sy = ty + (float)(i / 2) * 40.0f + 10.0f;
		if (k_syms[i].kind == 5)
			vc_pma(c, 1.0f, 0.25f, 0.2f, 1.0f);
		else if (k_syms[i].kind == 6)
			vc_pma(c, 0.35f, 0.55f, 1.0f, 1.0f);
		else
			vc_pma(c, 0.35f, 1.0f, 0.35f, 1.0f);
		vp_symbol(k_syms[i].kind, sx, sy, c);
		vc_text(sx + 34.0f, sy - 11.0f, k_syms[i].text, 19.0f, 0, 0.88f, 0.97f, 1.0f, 1.0f);
	}
	vc_text(x + w * 0.5f, y + h - 40.0f, "touch to close", 16.0f, 1, VC_HUD_PALE, 0.9f);
}

/* ---- Side panel: voice commands ------------------------------------------ */

static void vp_voice_draw(void) {
	const float W = s_vc.cur_w_px, H = s_vc.cur_h_px;
	const char* mode = SDL_getenv("XWA_XR_VOICE");
	const int off = mode && (mode[0] == '0' || mode[0] == 'o' || mode[0] == 'O');
	const char* how = off ? "Voice commands are off (Settings -> Virtual Reality -> Voice Commands)"
					  : mode && (mode[0] == 'h' || mode[0] == 'H') ? "Hold a thumb rest and speak, e.g.:"
																   : "Double-tap a thumb rest, then speak, e.g.:";
	vc_text(44.0f, 92.0f, how, 18.0f, 0, 0.88f, 0.97f, 1.0f, 0.95f);
	const int n = s_vc.voice_count ? s_vc.voice_count() : 0;
	if (n <= 0 || !s_vc.voice_phrase) {
		vc_text(W * 0.5f, H * 0.5f, "(no voice command list)", 20.0f, 1, VC_HUD_PALE, 0.9f);
		return;
	}
	/* Three columns of the example phrases, in the table's order. */
	const int per_col = (n + 2) / 3;
	const float step = per_col > 16 ? (H - 190.0f) / (float)per_col : 34.0f;
	const float size = step < 30.0f ? step * 0.62f : 19.0f;
	for (int i = 0; i < n; ++i) {
		const int col = i / per_col, row = i % per_col;
		const char* phrase = s_vc.voice_phrase(i, 0);
		if (!phrase)
			continue;
		char text[64];
		snprintf(text, sizeof text, "\"%s\"", phrase);
		float sz = size;
		const float wpx = vc_text_width(text, sz);
		if (wpx > 300.0f && wpx > 0.0f)
			sz *= 300.0f / wpx;
		vc_text(44.0f + 316.0f * (float)col, 132.0f + step * (float)row, text, sz, 0, 0.88f, 0.97f, 1.0f, 1.0f);
	}
	vc_text(W * 0.5f, H - 46.0f, "Your own words work too: \"wingmen, go after my target\", \"punch it\"", 16.0f, 1,
			VC_HUD_PALE, 0.9f);
}

/* ---- Side panel: button mapping ------------------------------------------
 * Both Touch controllers in outline, three-quarter view from above, each
 * control labelled from vr_buttons.c's table (plain and grip layer). */

static const float k_ctl_handle[][2] = {
	{ -88.0f, 66.0f },   { -104.0f, 128.0f }, { -116.0f, 198.0f }, { -120.0f, 254.0f }, { -110.0f, 293.0f },
	{ -86.0f, 309.0f },  { -60.0f, 298.0f },  { -44.0f, 256.0f },  { -24.0f, 190.0f },  { -2.0f, 128.0f },
	{ 18.0f, 88.0f },
};
static const float k_ctl_trigger[][2] = {
	{ 99.0f, 12.0f }, { 111.0f, 42.0f }, { 109.0f, 84.0f }, { 97.0f, 101.0f }, { 84.0f, 89.0f }, { 80.0f, 50.0f },
};
static const float k_ctl_grip[][2] = {
	{ -6.0f, 110.0f }, { 8.0f, 104.0f }, { 12.0f, 122.0f }, { 0.0f, 164.0f }, { -14.0f, 176.0f }, { -20.0f, 155.0f },
};
#define CTL_FACE_RX 104.0f
#define CTL_FACE_RY 68.0f
#define CTL_FACE_ROT_DEG -26.0f

enum { CTL_STICK, CTL_UPPER, CTL_LOWER, CTL_MENU, CTL_TRIGGER, CTL_GRIP, CTL_PARTS };
static const float k_ctl_part[CTL_PARTS][3] = {
	[CTL_STICK] = { -30.0f, -20.0f, 25.0f }, [CTL_UPPER] = { 42.0f, -12.0f, 14.0f },
	[CTL_LOWER] = { 16.0f, 27.0f, 14.0f },   [CTL_MENU] = { -58.0f, 24.0f, 8.0f },
	[CTL_TRIGGER] = { 97.0f, 92.0f, 0.0f },  [CTL_GRIP] = { -4.0f, 140.0f, 0.0f },
};
static const float k_ctl_row_y[CTL_PARTS] = { 96.0f, 168.0f, 232.0f, 296.0f, 452.0f, 380.0f };

static void vc_card_path(const float (*pts)[2], int n, int closed, float ox, float oy, float sx, float width,
						 const float col[4]) {
	const int steps = 6;
	float px = 0.0f, py = 0.0f;
	int have = 0;
	const int segs = closed ? n : n - 1;
	for (int i = 0; i < segs; ++i) {
		const int i0 = closed ? (i - 1 + n) % n : (i > 0 ? i - 1 : 0);
		const int i1 = i;
		const int i2 = closed ? (i + 1) % n : i + 1;
		const int i3 = closed ? (i + 2) % n : (i + 2 < n ? i + 2 : n - 1);
		for (int s = 0; s <= steps; ++s) {
			if (have && s == 0)
				continue;
			const float t = (float)s / (float)steps, t2 = t * t, t3 = t2 * t;
			float p[2];
			for (int k = 0; k < 2; ++k)
				p[k] = 0.5f * ((2.0f * pts[i1][k]) + (-pts[i0][k] + pts[i2][k]) * t +
							   (2.0f * pts[i0][k] - 5.0f * pts[i1][k] + 4.0f * pts[i2][k] - pts[i3][k]) * t2 +
							   (-pts[i0][k] + 3.0f * pts[i1][k] - 3.0f * pts[i2][k] + pts[i3][k]) * t3);
			const float x = ox + sx * p[0], y = oy + p[1];
			if (have)
				vc_line(px, py, x, y, width, col);
			px = x;
			py = y;
			have = 1;
		}
	}
}

static void vc_card_ellipse(float cx, float cy, float rx, float ry, float rot_deg, float a0, float a1, float width,
							const float col[4]) {
	const float rot = rot_deg * VC_PI / 180.0f;
	const float cr = cosf(rot), sr = sinf(rot);
	const int steps = (int)fmaxf(8.0f, fabsf(a1 - a0) / 6.0f);
	float px = 0.0f, py = 0.0f;
	for (int i = 0; i <= steps; ++i) {
		const float a = (a0 + (a1 - a0) * (float)i / (float)steps) * VC_PI / 180.0f;
		const float ex = rx * cosf(a), ey = ry * sinf(a);
		const float x = cx + ex * cr - ey * sr, y = cy + ex * sr + ey * cr;
		if (i > 0)
			vc_line(px, py, x, y, width, col);
		px = x;
		py = y;
	}
}

/* What a flight button does on the plain / grip layer ("" when nothing). */
static const char* vc_binding_label(AeronXrButton button, bool shifted) {
	size_t n = 0;
	const TieVrButtonBinding* t = TieVrButtons_Table(&n);
	for (size_t i = 0; i < n; ++i)
		if (t[i].button == button && t[i].shifted == shifted)
			return t[i].label ? t[i].label : "";
	return "";
}

/* The label lines of control `part` of the controller with the game's
 * `right_role` (left-handed play swaps the roles between the hands). */
static void vc_card_label_text(int part, int right_role, char* action, char* more, size_t size) {
	static const AeronXrButton k_role_button[2][CTL_PARTS] = {
		/* left role */ { AERON_XR_BUTTON_LEFT_STICK, AERON_XR_BUTTON_Y, AERON_XR_BUTTON_X, AERON_XR_BUTTON_COUNT,
						  AERON_XR_BUTTON_LEFT_TRIGGER, AERON_XR_BUTTON_COUNT },
		/* right role */ { AERON_XR_BUTTON_RIGHT_STICK, AERON_XR_BUTTON_B, AERON_XR_BUTTON_A, AERON_XR_BUTTON_COUNT,
						   AERON_XR_BUTTON_COUNT, AERON_XR_BUTTON_RIGHT_GRIP },
	};
	const int lh = Aeron_XrLeftHanded() != 0;
	action[0] = more[0] = '\0';
	const AeronXrButton button = k_role_button[right_role][part];
	const char* plain = button != AERON_XR_BUTTON_COUNT ? vc_binding_label(button, false) : "";
	const char* shifted = button != AERON_XR_BUTTON_COUNT ? vc_binding_label(button, true) : "";
	switch (part) {
		case CTL_STICK:
			snprintf(action, size, "%s", right_role ? "Yaw / pitch" : "Throttle (fwd/back) - roll");
			snprintf(more, size, "click: %s%s%s", plain, shifted[0] ? "   grip+: " : "", shifted);
			return;
		case CTL_UPPER:
		case CTL_LOWER:
			snprintf(action, size, "%s: %s", TieVrButtons_ButtonName(button, false, lh), plain);
			break;
		case CTL_MENU:
			snprintf(action, size, "%s", right_role ? "Meta: system menu" : "Settings menu");
			return;
		case CTL_TRIGGER:
			if (right_role) {
				snprintf(action, size, "Fire");
				snprintf(more, size, "both: confirm   grip + both: hyperspace");
				return;
			}
			snprintf(action, size, "%s", plain);
			break;
		default: /* grip */
			if (!right_role) {
				snprintf(action, size, "Hold: grip layer (grip+)");
				snprintf(more, size, "hold still: radial menu; on a panel's end: move it");
				return;
			}
			snprintf(action, size, "%s", plain);
			break;
	}
	if (action[0])
		action[0] = (char)toupper((unsigned char)action[0]);
	if (shifted[0])
		snprintf(more, size, "grip+: %s", shifted);
}

/* One controller: outline, its controls and their labels. `sx` -1 mirrors
 * it into the right controller; labels go to the outer side. */
static void vc_card_controller(float cx, float cy, float sx, int right_role, float text_x, float mid_x) {
	float line[4], part[4], lead[4], trig[4], grip[4];
	vc_pma(line, 0.78f, 0.97f, 0.85f, 0.95f);
	vc_pma(part, VC_HUD, 1.0f);
	vc_pma(lead, VC_HUD, 0.5f);
	vc_pma(trig, VC_AMBER, 1.0f);
	vc_pma(grip, 0.66f, 0.56f, 1.0f, 1.0f);
	const float face_rot = CTL_FACE_ROT_DEG * sx;
	vc_card_path(k_ctl_handle, (int)(sizeof k_ctl_handle / sizeof k_ctl_handle[0]), 0, cx, cy, sx, 3.0f, line);
	vc_card_ellipse(cx + 5.0f * sx, cy + 15.0f, CTL_FACE_RX, CTL_FACE_RY, face_rot, sx > 0.0f ? -12.0f : 12.0f,
					sx > 0.0f ? 192.0f : 168.0f, 3.0f, line);
	vc_card_ellipse(cx, cy, CTL_FACE_RX, CTL_FACE_RY, face_rot, 0.0f, 360.0f, 3.0f, line);
	vc_card_path(k_ctl_trigger, (int)(sizeof k_ctl_trigger / sizeof k_ctl_trigger[0]), 1, cx, cy, sx, 2.5f, trig);
	vc_card_path(k_ctl_grip, (int)(sizeof k_ctl_grip / sizeof k_ctl_grip[0]), 1, cx, cy, sx, 2.5f, grip);
	{
		const float* s = k_ctl_part[CTL_STICK];
		vc_card_ellipse(cx + sx * s[0], cy + s[1], s[2], s[2] * 0.82f, 0.0f, 0.0f, 360.0f, 2.5f, part);
		vc_card_ellipse(cx + sx * s[0], cy + s[1] - 3.0f, s[2] * 0.62f, s[2] * 0.52f, 0.0f, 0.0f, 360.0f, 2.5f, part);
	}
	for (int p = CTL_UPPER; p <= CTL_MENU; ++p) {
		const float* b = k_ctl_part[p];
		vc_card_ellipse(cx + sx * b[0], cy + b[1], b[2], b[2] * 0.78f, 0.0f, 0.0f, 360.0f, 2.5f, part);
	}
	for (int p = 0; p < CTL_PARTS; ++p) {
		const float* pp = k_ctl_part[p];
		const float row_y = k_ctl_row_y[p];
		const float ex = cx + sx * pp[0], ey = cy + pp[1];
		const int under = p == CTL_TRIGGER;
		const float tx = under ? mid_x - sx * 14.0f : text_x;
		const float ly = under ? row_y - 8.0f : row_y + 14.0f;
		const float lx = under ? ex + sx * 6.0f : text_x + (sx > 0.0f ? 10.0f : -10.0f);
		const float r = p <= CTL_MENU ? pp[2] : 0.0f;
		float dx = lx - ex, dy = ly - ey;
		const float dl = sqrtf(dx * dx + dy * dy);
		dx /= dl > 0.0f ? dl : 1.0f;
		dy /= dl > 0.0f ? dl : 1.0f;
		const float* c = p == CTL_TRIGGER ? trig : p == CTL_GRIP ? grip : lead;
		vc_line(ex + dx * (r + 3.0f), ey + dy * (r + 3.0f), lx, ly, 1.5f, lead);
		vc_rrect(lx - 3.0f, ly - 3.0f, 6.0f, 6.0f, 3.0f, 0.0f, 0.0f, c, c);
		const int align = sx > 0.0f ? 2 : 0;
		const float tc[3] = { p == CTL_TRIGGER ? 1.0f : p == CTL_GRIP ? 0.80f : 0.85f,
							  p == CTL_TRIGGER ? 0.71f : p == CTL_GRIP ? 0.72f : 1.0f,
							  p == CTL_TRIGGER ? 0.28f : p == CTL_GRIP ? 1.0f : 0.9f };
		char action[96], more[96];
		vc_card_label_text(p, right_role, action, more, sizeof action);
		float size = 20.0f, msize = 15.0f;
		const float room = under ? 330.0f : 200.0f;
		const float aw = vc_text_width(action, size), mw = vc_text_width(more, msize);
		if (aw > room && aw > 0.0f)
			size *= room / aw;
		if (mw > room && mw > 0.0f)
			msize *= room / mw;
		vc_text(tx, row_y, action, size, align, tc[0], tc[1], tc[2], 1.0f);
		if (more[0])
			vc_text(tx, row_y + 24.0f, more, msize, align, VC_HUD_PALE, 0.9f);
	}
}

static void vp_buttons_draw(void) {
	const float W = s_vc.cur_w_px, H = s_vc.cur_h_px;
	const int lh = Aeron_XrLeftHanded() != 0;
	const float cy = 222.0f;
	/* The physical left controller plays the right role when left-handed. */
	vc_card_controller(W * 0.5f - 148.0f, cy, 1.0f, lh ? 1 : 0, 236.0f, W * 0.5f);
	vc_card_controller(W * 0.5f + 148.0f, cy, -1.0f, lh ? 0 : 1, W - 236.0f, W * 0.5f);
	vc_text(W * 0.5f - 148.0f - 80.0f, cy + 318.0f, "LEFT", 22.0f, 1, VC_HUD, 0.9f);
	vc_text(W * 0.5f + 148.0f + 80.0f, cy + 318.0f, "RIGHT", 22.0f, 1, VC_HUD, 0.9f);
	vc_text(W * 0.5f, H - 132.0f,
			lh ? "Radial menu: hold the RIGHT grip still, point at a command, pull the right trigger"
			   : "Radial menu: hold the LEFT grip still, point at a command, pull the left trigger",
			17.0f, 1, 0.88f, 0.97f, 1.0f, 0.95f);
	vc_text(W * 0.5f, H - 104.0f, "Lap console (look down): touch a button, or point and pull the trigger", 17.0f, 1,
			VC_HUD_PALE, 0.9f);
}

/* ---- Side panel: frame ----------------------------------------------------- */

static int vp_ray_beam(int hand, float start[3], float end[3]) {
	if (!s_vc.card.drawn || !s_vp.ray[hand] || s_vp.poke_down[hand])
		return 0;
	memcpy(start, s_vp.ray_start[hand], sizeof s_vp.ray_start[hand]);
	memcpy(end, s_vp.ray_end[hand], sizeof s_vp.ray_end[hand]);
	return 1;
}

static void vp_draw(void) {
	vc_hud_colors();
	vc_plate(&s_vc.card.place);
	vp_draw_tabs();
	switch (s_vp.tab) {
		case VP_TAB_VOICE:
			vp_voice_draw();
			break;
		case VP_TAB_BUTTONS:
			vp_buttons_draw();
			break;
		default:
			vp_map_draw();
			break;
	}
}

/* The ray's dot on the side panel. */
static void vp_overlay(void) {
	vc_hud_colors();
	for (int h = 0; h < 2; ++h) {
		if (!s_vp.ray[h])
			continue;
		float c[4], ring[4];
		vc_pma(c, 1.0f, 1.0f, 1.0f, 0.95f);
		vc_pma(ring, VC_HUD, 0.9f);
		vc_rrect(s_vp.cx[h] - 9.0f, s_vp.cy[h] - 9.0f, 18.0f, 18.0f, 9.0f, 3.0f, 1.5f, c, ring);
	}
}

int TieVrConsole_PanelFrame(AeronCommandBuffer* cmd, int width, int height, const AeronXrView views[2],
							AeronXrPanel out[2]) {
	if (s_vw.open || !cmd || width <= 0 || height <= 0 || !views || !out || !s_vc.state.valid || !vp_enabled() ||
		s_vc.font_state == 0 || !vc_ensure())
		return 0;
	VcCard* cd = &s_vc.card;
	vp_load();
	if (!cd->list[0] && !(cd->list[0] = AeronDrawList_Create(0)))
		return 0;
	/* Every mission starts with the panel back beside the player. */
	if (s_vc.state.mission_serial != cd->mission) {
		vc_grab_end(&cd->place);
		cd->place.custom = 0;
		cd->mission = s_vc.state.mission_serial;
	}
	cd->drawn = 1;
	const float yaw = vc_env("XWA_XR_CARD_YAW", VC_CARD_YAW_DEG);
	if (!cd->logged) {
		Aeron_LogInfo("tie.vrconsole", "side panel: %dx%d layer, %.0f deg %s", width, height, (double)fabsf(yaw),
					  yaw < 0.0f ? "left" : "right");
		cd->logged = 1;
	}
	memset(out, 0, sizeof *out);
	float ax[3], ay[3], az[3];
	for (int pass = 0; pass < 2; ++pass) {
		if (!vc_place_pose(&cd->place, VC_CARD_W_M, VC_CARD_H_M, out, ax, ay, az))
			vc_facing_pose(yaw, VC_CARD_PITCH_DEG, VC_CARD_DISTANCE_M, VC_CARD_W_M, VC_CARD_H_M, out, ax, ay, az);
		if (pass == 0)
			vc_update_grab(&cd->place, out, ax, ay, az, VC_CARD_W_M, VC_CARD_H_M);
	}
	vc_update_wake(out, views, &cd->place, VC_CARD_WAKE_DEG, VC_CARD_SLEEP_DEG, &cd->awake, &cd->alpha);
	vp_update_hands(out, ax, ay, az, VC_CARD_W_M, VC_CARD_H_M, (float)width, (float)height);

	s_vc.cur_place = &cd->place;
	s_vc.cur_w_px = (float)width;
	s_vc.cur_h_px = (float)height;
	s_alpha_draw = cd->alpha;
	/* What the panel's drawing reads. The map redraws 12 times a second. */
	uint64_t key = 1469598103934665603ull;
	{
		const float alpha_q = floorf(cd->alpha * 255.0f + 0.5f);
		const int map_tick = s_vp.tab == VP_TAB_MAP ? (int)(s_vc.clock * 12.0f) : 0;
		const int words[] = { (int)alpha_q,
							  cd->place.grab_side,
							  cd->place.hover_side[0],
							  cd->place.hover_side[1],
							  Aeron_XrLeftHanded(),
							  s_vc.font_state,
							  width,
							  height,
							  s_vp.tab,
							  s_vp.zoom,
							  s_vp.hover[0],
							  s_vp.hover[1],
							  map_tick,
							  s_vp.map_help,
							  s_vc.voice_count ? s_vc.voice_count() : 0 };
		for (size_t i = 0; i < sizeof words / sizeof words[0]; ++i)
			key = vc_mix(key, (uint32_t)words[i]);
		key = vc_mix_s(key, SDL_getenv("XWA_XR_VOICE"));
		key = vc_mix_s(key, SDL_getenv("TIE_XR_CONSOLE_COLOR"));
		key |= 1u;
	}
	if (!cd->list[1] && !(cd->list[1] = AeronDrawList_Create(0)))
		return 0;
	return vc_render_panel(cmd, AERON_XR_PANEL_CARD, cd->list[1], vp_draw, vp_overlay, views, out, width, height,
						   VC_CARD_W_M, VC_CARD_H_M, &cd->hash, &cd->hash_valid, key);
}

/* ---- End of frame ------------------------------------------------------ */

void TieVrConsole_EndFrame(void) {
	if (!s_vw.drawn)
		vw_close(); /* not in flight this frame (paused, a menu) */
	if (!s_vc.drawn) {
		for (int h = 0; h < 2; ++h) {
			if (s_vc.hands[h].poke_down || s_vc.hands[h].trigger_down)
				vc_release(h);
			s_vc.hands[h].captured = 0;
			s_vc.hands[h].poke_down = 0;
			s_vc.hands[h].trigger_down = 0;
			s_vc.hands[h].cursor = 0;
			s_vc.lap.hover_side[h] = 0;
		}
		vc_grab_end(&s_vc.lap);
		s_vc.awake = 0;
	}
	if (!s_vc.card.drawn) {
		for (int h = 0; h < 2; ++h) {
			s_vc.card.place.hover_side[h] = 0;
			s_vp.captured[h] = s_vp.poke_down[h] = s_vp.trigger_down[h] = s_vp.ray[h] = 0;
		}
		vc_grab_end(&s_vc.card.place);
		s_vc.card.awake = 0;
	}
	bool owned[2];
	for (int h = 0; h < 2; ++h) {
		/* The trigger of a hand pointing at a panel, or the open wheel's,
		 * is not passed on to the game. */
		const int trigger = s_vc.hands[h].captured || s_vp.captured[h] || (s_vw.open && h == s_vw.hand);
		Aeron_XrCaptureTrigger(h, trigger);
		/* Grips at a panel's handle (vr_hands applies them with the flight
		 * stick's in its EndFrame, which runs next). */
		if (s_vc.grip_capture[h])
			TieVrHands_CaptureGrip(h);
		owned[h] = TieVrHands_GripCaptured(h) != 0;
		s_vc.grip_capture_prev[h] = owned[h];
		s_vc.grip_capture[h] = 0;
	}
	TieVrControls_SetConsoleState(s_vw.open != 0, owned);
}

void TieVrConsole_Shutdown(void) {
	if (s_vc.list)
		AeronDrawList_Destroy(s_vc.list);
	if (s_vc.list_r)
		AeronDrawList_Destroy(s_vc.list_r);
	for (int e = 0; e < 2; ++e) {
		if (s_vc.card.list[e])
			AeronDrawList_Destroy(s_vc.card.list[e]);
		if (s_vw.list[e])
			AeronDrawList_Destroy(s_vw.list[e]);
	}
	memset(&s_vw, 0, sizeof s_vw);
	for (int i = 0; i < AERON_XR_PANEL_COUNT; ++i) {
		if (s_base[i].list)
			AeronDrawList_Destroy(s_base[i].list);
		if (s_base[i].rt)
			Aeron_DestroyRenderTarget(s_base[i].rt);
	}
	memset(s_base, 0, sizeof s_base);
	if (s_vc.font_state == 1)
		AeronFontAtlas_Release(&s_vc.font);
	int (*count)(void) = s_vc.voice_count;
	const char* (*label)(int) = s_vc.voice_label;
	const char* (*phrase)(int, int) = s_vc.voice_phrase;
	memset(&s_vc, 0, sizeof s_vc);
	TieVrConsole_SetVoiceList(count, label, phrase);
}

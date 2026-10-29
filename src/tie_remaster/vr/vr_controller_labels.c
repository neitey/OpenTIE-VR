/* VR controller labels; see vr_controller_labels.h. */

#include "tie_remaster/vr/vr_controller_labels.h"
#include <SDL3/SDL_stdinc.h>

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "aeron/scene/draw_list2d.h"
#include "aeron/scene/font_atlas.h"
#include "aeron/scene/text2d.h"
#include "aeron/vfs.h"
#include "tie_remaster/vr/vr_controllers.h"
#include "tie_remaster/vr/vr_gloves.h"
#include "tie_runtime/input/vr_buttons.h"

#include <SDL3/SDL_timer.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* With the controller models shown (Hands: Controllers), looking at a
 * controller with its buttons turned towards you shows what each control
 * does: a label per control beside it, a leader line to a dot on the
 * control. Each controller has a sheet of its own (a quad facing the eyes
 * through the middle of its button face); both share the labels' layer,
 * the left one in its left half. Thumbstick directions are left out. */
#define LB_SHEET_H_M 0.30f
/* Looked at: near the middle of the view, raised towards the face (a
 * hand resting in the lap is farther), buttons turned towards the eyes. */
#define LB_ENTER_DEG 20.0f
#define LB_EXIT_DEG 28.0f
#define LB_ENTER_DIST_M 0.58f
#define LB_EXIT_DIST_M 0.72f
#define LB_ENTER_FACING 0.50f
#define LB_EXIT_FACING 0.25f
#define LB_DWELL_S 0.30f
#define LB_FADE_IN_S 0.15f
#define LB_FADE_OUT_S 0.25f

#define LB_CONTROL_PX 15.0f
#define LB_ACTION_PX 24.0f
#define LB_MORE_PX 15.0f

/* The labels' colours: the holo cyan of OpenXWA-VR's panels, a darker one
 * for the plates and a pale one for the second lines. */
static const float k_lb_hud[3] = { 0.498f, 0.910f, 1.0f };
#define LB_HUD k_lb_hud[0], k_lb_hud[1], k_lb_hud[2]
#define LB_HUD_DARK k_lb_hud[0] * 0.75f, k_lb_hud[1] * 0.75f, k_lb_hud[2] * 0.75f
#define LB_HUD_PALE 0.55f * k_lb_hud[0] + 0.414f, 0.55f * k_lb_hud[1] + 0.414f, 0.55f * k_lb_hud[2] + 0.414f

typedef struct LbEntry {
	int ctl;
	const char* control; /* "TRIGGER", "B", ... */
	char action[48];
	char more[64]; /* a second, smaller line, or empty */
	int pressed;
	float ax, ay; /* the control on the sheet, px */
	int side;     /* +1 right of the controller, -1 left */
	float x, y, w, h;
} LbEntry;

typedef struct LbSheet {
	int visible;
	AeronXrPanel pose;
	float c[3], ax[3], ay[3], az[3], eye[3];
	float w_m, h_m, w_px, h_px;
	LbEntry e[TIE_VR_CTL_COUNT];
	int n;
} LbSheet;

static struct {
	float alpha[2], dwell[2];
	int on[2];
	LbSheet sheet[2];
	AeronDrawList2D* list[2];
	AeronDrawList2D* cur_list;
	float cur_alpha;
	AeronFontAtlas font;
	int font_state; /* 0 not tried, 1 loaded, -1 failed */
	uint64_t last_ns;
	float dt;
	int logged;
	/* The layer's size (known after its first image) and what it shows:
	 * redrawn only when that changes. */
	int tw, th;
	uint64_t hash;
	int hash_valid;
} s_lb;

/* ---- Small math and drawing -------------------------------------------- */

static void lb_rotate(const float q[4], const float v[3], float out[3]) {
	const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
	const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
	const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
	out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
	out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
	out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

static float lb_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void lb_norm(float v[3]) {
	const float l = sqrtf(lb_dot(v, v));
	if (l > 1e-9f) {
		v[0] /= l;
		v[1] /= l;
		v[2] /= l;
	}
}

/* Rotation with columns x, y, z -> quaternion. */
static void lb_basis_quat(const float x[3], const float y[3], const float z[3], float q[4]) {
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

static void lb_pma(float out[4], float r, float g, float b, float a) {
	a *= s_lb.cur_alpha;
	out[0] = r * a;
	out[1] = g * a;
	out[2] = b * a;
	out[3] = a;
}

static void lb_rrect(float x, float y, float w, float h, float radius, float border_px, float soft,
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
	if (border) {
		memcpy(r.border, border, sizeof r.border);
	}
	AeronDrawList_AddRRect(s_lb.cur_list, &r);
}

static void lb_text(float x, float y, const char* text, float size_px, float r, float g, float b, float a) {
	if (s_lb.font_state != 1 || !text || !text[0]) {
		return;
	}
	AeronTextStyle style;
	memset(&style, 0, sizeof style);
	style.font = &s_lb.font;
	style.scale = size_px / (float)s_lb.font.cell_h;
	style.tint[0] = r;
	style.tint[1] = g;
	style.tint[2] = b;
	style.tint[3] = a * s_lb.cur_alpha;
	style.filter = AERON_BLIT2D_FILTER_LINEAR;
	AeronText_Draw(s_lb.cur_list, &style, x, y, AERON_TEXT_LEFT, text, -1, NULL);
}

static float lb_text_width(const char* text, float size) {
	if (s_lb.font_state != 1 || !text || !text[0]) {
		return 0.0f;
	}
	AeronTextStyle probe;
	memset(&probe, 0, sizeof probe);
	probe.font = &s_lb.font;
	probe.scale = size / (float)s_lb.font.cell_h;
	return AeronText_MeasureWidth(&probe, text, -1);
}

/* ---- What the controls do ---------------------------------------------- */

static int lb_enabled(void) {
	const char* env = SDL_getenv("XWA_XR_CONTROLLER_LABELS");
	return TieVrHands_Mode() == TIE_VR_HANDS_CONTROLLERS && !(env && env[0] == '0');
}

/* "next target" -> "Next target". */
static void lb_set_action(LbEntry* e, const char* text) {
	snprintf(e->action, sizeof e->action, "%s", text ? text : "");
	e->action[0] = (char)toupper((unsigned char)e->action[0]);
}

static const char* lb_binding_label(AeronXrButton button, bool shifted) {
	size_t count = 0;
	const TieVrButtonBinding* table = TieVrButtons_Table(&count);
	for (size_t i = 0; i < count; ++i) {
		if (table[i].button == button && table[i].shifted == shifted && table[i].label) {
			return table[i].label;
		}
	}
	return NULL;
}

/* The plain action of `button`, the shifted one as the second line. */
static int lb_from_binding(LbEntry* e, AeronXrButton button) {
	const char* plain = lb_binding_label(button, false);
	const char* shifted = lb_binding_label(button, true);
	if (!plain && !shifted) {
		return 0;
	}
	lb_set_action(e, plain ? plain : "-");
	if (shifted) {
		snprintf(e->more, sizeof e->more, "with grip: %s", shifted);
	}
	return 1;
}

/* The labels of physical controller `hand` (0 left, 1 right) now. */
static int lb_entries(int hand, const AeronXrHand* xh, LbEntry* out) {
	const int right_role = Aeron_XrLeftHanded() ? !hand : hand;
	int n = 0;
	for (int ctl = 0; ctl < TIE_VR_CTL_COUNT; ++ctl) {
		LbEntry* e = &out[n];
		memset(e, 0, sizeof *e);
		e->ctl = ctl;
		int ok = 1;
		switch (ctl) {
			case TIE_VR_CTL_TRIGGER:
				e->control = "TRIGGER";
				if (right_role) {
					lb_set_action(e, TieVrButtons_ControlLabel(TIE_VR_CONTROL_RIGHT_TRIGGER));
					snprintf(e->more, sizeof e->more, "both triggers: %s",
							 TieVrButtons_ControlLabel(TIE_VR_CONTROL_CHORD));
				} else {
					ok = lb_from_binding(e, AERON_XR_BUTTON_LEFT_TRIGGER);
				}
				e->pressed = xh->trigger > 0.5f;
				break;
			case TIE_VR_CTL_GRIP:
				e->control = "GRIP";
				if (right_role) {
					ok = lb_from_binding(e, AERON_XR_BUTTON_RIGHT_GRIP);
				} else {
					lb_set_action(e, "Hold: shift");
					snprintf(e->more, sizeof e->more, "+ both triggers: %s",
							 TieVrButtons_ControlLabel(TIE_VR_CONTROL_SHIFT_CHORD));
				}
				e->pressed = xh->grip > 0.5f;
				break;
			case TIE_VR_CTL_UPPER:
				e->control = hand ? "B" : "Y";
				ok = lb_from_binding(e, right_role ? AERON_XR_BUTTON_B : AERON_XR_BUTTON_Y);
				e->pressed = (xh->buttons & AERON_XR_HAND_BUTTON_UPPER) != 0;
				break;
			case TIE_VR_CTL_LOWER:
				e->control = hand ? "A" : "X";
				ok = lb_from_binding(e, right_role ? AERON_XR_BUTTON_A : AERON_XR_BUTTON_X);
				e->pressed = (xh->buttons & AERON_XR_HAND_BUTTON_LOWER) != 0;
				break;
			case TIE_VR_CTL_STICK:
				e->control = "STICK CLICK";
				ok = lb_from_binding(e, right_role ? AERON_XR_BUTTON_RIGHT_STICK : AERON_XR_BUTTON_LEFT_STICK);
				e->pressed = (xh->buttons & AERON_XR_HAND_BUTTON_STICK) != 0;
				break;
			case TIE_VR_CTL_MENU:
				/* The left menu button; the right one is the headset's. */
				e->control = "MENU";
				ok = hand == 0;
				lb_set_action(e, TieVrButtons_ControlLabel(TIE_VR_CONTROL_MENU));
				e->pressed = (xh->buttons & AERON_XR_HAND_BUTTON_MENU) != 0;
				break;
			default: {
				/* The thumb rest starts voice commands (where it has one). */
				const char* voice = SDL_getenv("XWA_XR_VOICE");
				e->control = "THUMB REST";
				ok = Aeron_XrThumbrestTouch(hand) >= 0 &&
					 !(voice && (voice[0] == '0' || voice[0] == 'o' || voice[0] == 'O'));
				lb_set_action(e, voice && (voice[0] == 'h' || voice[0] == 'H') ? "Hold: voice command"
																			   : "Double-tap: voice command");
				e->pressed = (xh->buttons & AERON_XR_HAND_BUTTON_THUMBREST) != 0;
				break;
			}
		}
		if (ok && e->action[0]) {
			++n;
		}
	}
	return n;
}

/* ---- Placing and drawing ------------------------------------------------ */

/* Whether the player looks at controller `hand` (its buttons towards
 * them), held for a moment; eases the labels' alpha. */
static void lb_update_gaze(int hand, const TieVrControllerView* v, const AeronXrHand* xh, const float eye[3],
						   const float fwd[3]) {
	float to[3] = { v->face_center[0] - eye[0], v->face_center[1] - eye[1], v->face_center[2] - eye[2] };
	const float dist = sqrtf(lb_dot(to, to));
	int look = 0;
	const int on = s_lb.on[hand];
	if (dist > 0.05f && dist < (on ? LB_EXIT_DIST_M : LB_ENTER_DIST_M)) {
		for (int k = 0; k < 3; ++k) {
			to[k] /= dist;
		}
		const float angle = acosf(fmaxf(-1.0f, fminf(1.0f, lb_dot(fwd, to)))) * 180.0f / 3.14159265f;
		const float facing = -lb_dot(v->face_normal, to);
		look = angle < (on ? LB_EXIT_DEG : LB_ENTER_DEG) && facing > (on ? LB_EXIT_FACING : LB_ENTER_FACING);
	}
	/* Not while the hand is busy: firing, gripping (the stick, a shift). */
	const int busy = xh->trigger > 0.35f || xh->grip > 0.6f;
	if (look && !busy) {
		s_lb.dwell[hand] += s_lb.dt;
		if (s_lb.dwell[hand] >= LB_DWELL_S) {
			s_lb.on[hand] = 1;
		}
	} else {
		s_lb.dwell[hand] = 0.0f;
		s_lb.on[hand] = 0;
	}
	const float goal = s_lb.on[hand] ? 1.0f : 0.0f;
	const float step = s_lb.dt / (goal > s_lb.alpha[hand] ? LB_FADE_IN_S : LB_FADE_OUT_S);
	s_lb.alpha[hand] =
		goal > s_lb.alpha[hand] ? fminf(goal, s_lb.alpha[hand] + step) : fmaxf(goal, s_lb.alpha[hand] - step);
}

/* A LOCAL point seen from the eye, on the sheet's plane: sheet px. */
static void lb_project(const LbSheet* s, const float p[3], float* px, float* py) {
	const float d[3] = { p[0] - s->eye[0], p[1] - s->eye[1], p[2] - s->eye[2] };
	const float ce[3] = { s->c[0] - s->eye[0], s->c[1] - s->eye[1], s->c[2] - s->eye[2] };
	const float dn = lb_dot(d, s->az);
	const float t = fabsf(dn) > 1e-5f ? lb_dot(ce, s->az) / dn : 1.0f;
	const float h[3] = { s->eye[0] + d[0] * t - s->c[0], s->eye[1] + d[1] * t - s->c[1],
						 s->eye[2] + d[2] * t - s->c[2] };
	*px = (lb_dot(h, s->ax) / s->w_m + 0.5f) * s->w_px;
	*py = (0.5f - lb_dot(h, s->ay) / s->h_m) * s->h_px;
}

/* Places the sheet and its labels for controller `hand`. */
static void lb_layout(LbSheet* s, int hand, const TieVrControllerView* v, const float eye[3], float w_px,
					  float h_px) {
	memcpy(s->c, v->face_center, sizeof s->c);
	memcpy(s->eye, eye, sizeof s->eye);
	s->h_m = LB_SHEET_H_M;
	s->w_m = LB_SHEET_H_M * w_px / h_px;
	s->w_px = w_px;
	s->h_px = h_px;
	/* Facing the eye, kept level. */
	float z[3] = { eye[0] - s->c[0], eye[1] - s->c[1], eye[2] - s->c[2] };
	lb_norm(z);
	float x[3] = { z[2], 0.0f, -z[0] };
	if (lb_dot(x, x) < 1e-6f) {
		x[0] = 1.0f;
		x[2] = 0.0f;
	}
	lb_norm(x);
	const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
	memcpy(s->ax, x, sizeof x);
	memcpy(s->ay, y, sizeof y);
	memcpy(s->az, z, sizeof z);
	memset(&s->pose, 0, sizeof s->pose);
	memcpy(s->pose.position, s->c, sizeof s->c);
	lb_basis_quat(x, y, z, s->pose.orientation);
	s->pose.size[0] = s->w_m;
	s->pose.size[1] = s->h_m;
	/* The controller's footprint: labels go beside it. */
	float x0 = 1e9f, x1 = -1e9f;
	for (int i = 0; i < v->outline_count; ++i) {
		float px, py;
		lb_project(s, v->outline[i], &px, &py);
		x0 = fminf(x0, px);
		x1 = fmaxf(x1, px);
	}
	for (int i = 0; i < s->n; ++i) {
		float px, py;
		lb_project(s, v->anchor[s->e[i].ctl], &px, &py);
		/* Whole pixels: a still controller draws the same sheet again (and
		 * the layer is not redrawn). */
		px = floorf(px + 0.5f);
		py = floorf(py + 0.5f);
		s->e[i].ax = px;
		s->e[i].ay = py;
		x0 = fminf(x0, px);
		x1 = fmaxf(x1, px);
	}
	/* Outer side: away from the other hand; stick and menu inside. */
	const int outer = hand ? 1 : -1;
	for (int i = 0; i < s->n; ++i) {
		LbEntry* e = &s->e[i];
		e->side = e->ctl == TIE_VR_CTL_STICK || e->ctl == TIE_VR_CTL_MENU ? -outer : outer;
		const float tw = fmaxf(fmaxf(lb_text_width(e->action, LB_ACTION_PX), lb_text_width(e->control, LB_CONTROL_PX)),
							   lb_text_width(e->more, LB_MORE_PX));
		e->w = tw + 32.0f;
		e->h = e->more[0] ? 78.0f : 60.0f;
		const float margin = 6.0f;
		if (e->side > 0) {
			e->x = fminf(x1 + 46.0f, w_px - margin - e->w);
		} else {
			e->x = fmaxf(x0 - 46.0f - e->w, margin);
		}
	}
	/* Each column top to bottom in the order of its controls, no overlaps. */
	for (int side = -1; side <= 1; side += 2) {
		int order[TIE_VR_CTL_COUNT], m = 0;
		for (int i = 0; i < s->n; ++i) {
			if (s->e[i].side == side) {
				int k = m++;
				while (k > 0 && s->e[order[k - 1]].ay > s->e[i].ay) {
					order[k] = order[k - 1];
					--k;
				}
				order[k] = i;
			}
		}
		float bottom = 6.0f - 8.0f;
		for (int k = 0; k < m; ++k) {
			LbEntry* e = &s->e[order[k]];
			e->y = fmaxf(e->ay - e->h * 0.5f, bottom + 8.0f);
			bottom = e->y + e->h;
		}
		if (m > 0 && bottom > h_px - 6.0f) {
			const float shift = bottom - (h_px - 6.0f);
			for (int k = 0; k < m; ++k) {
				s->e[order[k]].y -= shift;
			}
		}
		for (int k = 0; k < m; ++k) {
			LbEntry* e = &s->e[order[k]];
			e->y = fmaxf(e->y, 6.0f);
		}
	}
}

static void lb_draw_sheet(const LbSheet* s) {
	const float transparent[4] = { 0, 0, 0, 0 };
	/* Leader lines and dots first, the labels over them. */
	for (int i = 0; i < s->n; ++i) {
		const LbEntry* e = &s->e[i];
		float line[4], dot[4], rim[4];
		const float ex = e->side > 0 ? e->x : e->x + e->w, ey = e->y + e->h * 0.5f;
		const float elbow = ex - (float)e->side * 26.0f;
		lb_pma(line, LB_HUD, 0.85f);
		AeronDrawList_AddLine(s_lb.cur_list, e->ax, e->ay, elbow, ey, 2.5f, line, AERON_BLIT2D_BLEND_PMA, NULL);
		AeronDrawList_AddLine(s_lb.cur_list, elbow, ey, ex, ey, 2.5f, line, AERON_BLIT2D_BLEND_PMA, NULL);
		lb_pma(dot, LB_HUD, 1.0f);
		lb_pma(rim, 1.0f, 1.0f, 1.0f, 0.95f);
		lb_rrect(e->ax - 7.0f, e->ay - 7.0f, 14.0f, 14.0f, 7.0f, 2.0f, 1.0f, dot, rim);
	}
	for (int i = 0; i < s->n; ++i) {
		const LbEntry* e = &s->e[i];
		float fill[4], border[4], glow[4];
		lb_pma(glow, LB_HUD, e->pressed ? 0.6f : 0.3f);
		lb_rrect(e->x - 5.0f, e->y - 5.0f, e->w + 10.0f, e->h + 10.0f, 16.0f, 5.0f, 9.0f, transparent, glow);
		lb_pma(fill, LB_HUD_DARK, e->pressed ? 0.55f : 0.32f);
		if (e->pressed) {
			lb_pma(border, 1.0f, 1.0f, 1.0f, 1.0f);
		} else {
			lb_pma(border, LB_HUD, 0.8f);
		}
		lb_rrect(e->x, e->y, e->w, e->h, 12.0f, 2.0f, 1.0f, fill, border);
		lb_text(e->x + 16.0f, e->y + 6.0f, e->control, LB_CONTROL_PX, LB_HUD, 0.95f);
		lb_text(e->x + 16.0f, e->y + 26.0f, e->action, LB_ACTION_PX, 0.90f, 0.98f, 0.93f, 1.0f);
		if (e->more[0]) {
			lb_text(e->x + 16.0f, e->y + 56.0f, e->more, LB_MORE_PX, LB_HUD_PALE, 0.85f);
		}
	}
}

/* ---- Frame -------------------------------------------------------------- */

void TieVrLabels_BeginFrame(void) {
	const uint64_t now = SDL_GetTicksNS();
	const float dt = s_lb.last_ns ? (float)((double)(now - s_lb.last_ns) * 1e-9) : 0.0f;
	s_lb.last_ns = now;
	/* A slow frame still advances the dwell and the fade (a step at most). */
	s_lb.dt = dt > 0.1f ? 0.1f : dt;
	/* Load the font here, between frames, rather than while the flight
	 * renderer is recording its command buffer. */
	if (s_lb.font_state == 0 && lb_enabled() && Aeron_XrIsPresenting()) {
		AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
		s_lb.font_state = -1;
		if (cmd) {
			if (AeronFontAtlas_LoadVfs(&s_lb.font, cmd, Aeron_GetVfs(), AERON_VFS_ROOT_RESOURCE, "assets/fonts/menu",
									   16u * 1024u * 1024u)) {
				if (Aeron_SubmitCommandBuffer(cmd)) {
					s_lb.font_state = 1;
				} else {
					AeronFontAtlas_Release(&s_lb.font);
				}
			} else {
				Aeron_CancelCommandBuffer(cmd);
			}
		}
		if (s_lb.font_state != 1) {
			Aeron_LogWarn("tie.vr", "controller labels: font unavailable, labels are not drawn");
		}
	}
}

void TieVrLabels_Idle(void) {
	s_lb.alpha[0] = s_lb.alpha[1] = 0.0f;
	s_lb.on[0] = s_lb.on[1] = 0;
	s_lb.dwell[0] = s_lb.dwell[1] = 0.0f;
}

int TieVrLabels_Frame(AeronCommandBuffer* cmd, const AeronXrView views[2], AeronXrPanel out[2]) {
	if (!cmd || !views || !out || !lb_enabled() || s_lb.font_state != 1) {
		TieVrLabels_Idle();
		return 0;
	}
	static const float minus_z[3] = { 0.0f, 0.0f, -1.0f };
	float eye[3], fwd[3];
	for (int k = 0; k < 3; ++k) {
		eye[k] = 0.5f * (views[0].position[k] + views[1].position[k]);
	}
	lb_rotate(views[0].orientation, minus_z, fwd);
	TieVrControllerView view[2];
	AeronXrHand xh[2];
	int any = 0;
	for (int h = 0; h < 2; ++h) {
		LbSheet* s = &s_lb.sheet[h];
		s->visible = 0;
		if (!TieVrControllers_View(h, &view[h]) || !Aeron_XrGetHand(h, &xh[h])) {
			s_lb.alpha[h] = 0.0f;
			s_lb.on[h] = 0;
			s_lb.dwell[h] = 0.0f;
			continue;
		}
		lb_update_gaze(h, &view[h], &xh[h], eye, fwd);
		if (s_lb.alpha[h] > 0.01f) {
			s->visible = 1;
			any = 1;
		}
	}
	if (!any) {
		s_lb.hash_valid = 0;
		return 0;
	}
	/* Taking the layer's target acquires its next image: only to draw. */
	AeronRenderTarget* target = NULL;
	if (s_lb.tw <= 0) {
		target = Aeron_XrStereoPanelTarget(AERON_XR_PANEL_LABELS, &s_lb.tw, &s_lb.th);
		s_lb.hash_valid = 0;
		if (!target || s_lb.tw < 4 || s_lb.th < 4) {
			s_lb.tw = 0;
			return 0;
		}
	}
	const int tw = s_lb.tw, th = s_lb.th;
	const int sw = tw / 2, sh = th;
	for (int e = 0; e < 2; ++e) {
		if (!s_lb.list[e] && !(s_lb.list[e] = AeronDrawList_Create(0))) {
			return 0;
		}
	}
	for (int h = 0; h < 2; ++h) {
		LbSheet* s = &s_lb.sheet[h];
		AeronDrawList_Begin(s_lb.list[h], NULL, sw, sh, h == 0 ? AERON_DRAWLIST2D_CLEAR : AERON_DRAWLIST2D_LOAD,
							NULL);
		AeronDrawList_SetOrigin(s_lb.list[h], h * sw, 0);
		if (!s->visible) {
			continue;
		}
		s->n = lb_entries(h, &xh[h], s->e);
		lb_layout(s, h, &view[h], eye, (float)sw, (float)sh);
		s_lb.cur_list = s_lb.list[h];
		s_lb.cur_alpha = s_lb.alpha[h];
		lb_draw_sheet(s);
	}
	const uint64_t hash = AeronDrawList_Hash(s_lb.list[0]) * 1099511628211ull ^ AeronDrawList_Hash(s_lb.list[1]);
	if (!s_lb.hash_valid || hash != s_lb.hash || target) {
		if (!target && !(target = Aeron_XrStereoPanelTarget(AERON_XR_PANEL_LABELS, NULL, NULL))) {
			return 0;
		}
		for (int e = 0; e < 2; ++e) {
			AeronDrawList_SetTarget(s_lb.list[e], target);
			if (!AeronDrawList_Prepare(s_lb.list[e], cmd)) {
				return 0;
			}
		}
		AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
			.color_target = target,
			.clear_color = 1,
			.command_buffer = cmd,
			.debug_label = "VR controller labels",
		});
		if (!pass) {
			return 0;
		}
		for (int e = 0; e < 2; ++e) {
			AeronDrawList_RenderIntoPass(s_lb.list[e], cmd, pass, target);
		}
		Aeron_EndRenderPass(pass);
		s_lb.hash = hash;
		s_lb.hash_valid = 1;
	}
	int n = 0;
	for (int h = 0; h < 2; ++h) {
		if (!s_lb.sheet[h].visible) {
			continue;
		}
		out[n] = s_lb.sheet[h].pose;
		out[n].rect[0] = h * sw;
		out[n].rect[1] = 0;
		out[n].rect[2] = sw;
		out[n].rect[3] = sh;
		out[n].eye = 0;
		++n;
	}
	if (!s_lb.logged) {
		Aeron_LogInfo("tie.vr", "controller labels: %dx%d per controller, %.2f m tall", sw, sh, (double)LB_SHEET_H_M);
		s_lb.logged = 1;
	}
	return n;
}

void TieVrLabels_Shutdown(void) {
	for (int e = 0; e < 2; ++e) {
		if (s_lb.list[e]) {
			AeronDrawList_Destroy(s_lb.list[e]);
		}
	}
	if (s_lb.font_state == 1) {
		AeronFontAtlas_Release(&s_lb.font);
	}
	memset(&s_lb, 0, sizeof s_lb);
}

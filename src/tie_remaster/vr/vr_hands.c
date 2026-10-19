/* The player's hands in stereo flight; see vr_hands.h. */

#include "tie_remaster/vr/vr_hands.h"

#include <math.h>
#include <string.h>

#include "aeron/aeron.h"
#include "tie_remaster/flight/render_math.h"
#include "tie_remaster/vr/vr_controller_labels.h"
#include "tie_remaster/vr/vr_flight_stick.h"
#include "tie_remaster/vr/vr_gloves.h"
#include "tie_remaster/vr/vr_hand_overlay.h"

enum { TIE_VR_HANDS_MAX_LAYERS = 4 };

static struct {
	int submitted; /* hands submitted this host frame */
	int over;      /* the eye overlay is worth showing this frame */
	int grip_capture[2];
	AeronXrPanel labels[2];
	int label_count;
	/* Last frame's HUD quads in LOCAL space (the overlay's layers). */
	AeronXrPanel layers[TIE_VR_HANDS_MAX_LAYERS];
	int layer_count;
} s_hands;

void TieVrHands_BeginFrame(void) {
	s_hands.submitted = 0;
	s_hands.over = 0;
	s_hands.label_count = 0;
	TieVrLabels_BeginFrame();
}

void TieVrHands_Submit(AeronScene3D* scene, AeronCommandBuffer* cmd, const AeronSceneCamera* craft_cam, float units,
					   const AeronXrStereoFrame* frame) {
	if (!scene || !cmd || !craft_cam || !frame || units <= 0.0f)
		return;
	float crows[9];
	TieRenderMath_QuaternionToMat3(craft_cam->ori, crows); /* rows: world -> craft eye */
	/* The flight stick first: its grab decides what the grips do. */
	TieVrStick_Update();
	TieVrGloves_Submit(scene, cmd, crows, craft_cam->pos, units);
	TieVrStick_Submit(scene, cmd, crows, craft_cam->pos, units);
	/* The hands' alpha for the overlay over the HUD layers. */
	TieVrHandOverlay_SetLayers(s_hands.layers, s_hands.layer_count);
	s_hands.over = TieVrHandOverlay_Prepare(cmd, frame->views, frame->width, frame->height);
	s_hands.submitted = 1;
}

void TieVrHands_SubmitMenu(AeronScene3D* scene, AeronCommandBuffer* cmd, const AeronSceneCamera* craft_cam,
						   float units, const AeronXrStereoFrame* frame, const AeronXrPanel* screen) {
	if (!scene || !cmd || !craft_cam || !frame || units <= 0.0f)
		return;
	float crows[9];
	TieRenderMath_QuaternionToMat3(craft_cam->ori, crows);
	TieVrGloves_Submit(scene, cmd, crows, craft_cam->pos, units);
	if (screen)
		TieVrHandOverlay_SetLayers(screen, 1);
	s_hands.over = TieVrHandOverlay_Prepare(cmd, frame->views, frame->width, frame->height);
	s_hands.submitted = 1;
}

void TieVrHands_CommitMenu(void) {
	if (s_hands.submitted && s_hands.over) {
		int rects[2][4];
		TieVrHandOverlay_Rects(rects);
		Aeron_XrStereoCommitEyeOverlayRect((const int(*)[4])rects);
	}
}

AeronDrawList2D* TieVrHands_EyeAlpha(int eye) { return s_hands.over ? TieVrHandOverlay_List(eye) : NULL; }

void TieVrHands_FinishRightEye(AeronCommandBuffer* cmd) {
	if (s_hands.over)
		TieVrHandOverlay_Finish(cmd);
}

void TieVrHands_RenderLayers(AeronCommandBuffer* cmd, const AeronXrStereoFrame* frame) {
	s_hands.label_count = 0;
	if (!s_hands.submitted || !frame) {
		TieVrLabels_Idle();
		return;
	}
	s_hands.label_count = TieVrLabels_Frame(cmd, frame->views, s_hands.labels);
}

/* A committed HUD quad as an overlay layer in LOCAL space. */
static void TieVrHands_LayerFromQuad(const AeronXrHudQuad* q, const AeronXrStereoFrame* frame) {
	if (!q || !q->visible || q->size[0] <= 0.0f || q->size[1] <= 0.0f ||
		s_hands.layer_count >= TIE_VR_HANDS_MAX_LAYERS)
		return;
	AeronXrPanel* p = &s_hands.layers[s_hands.layer_count++];
	memset(p, 0, sizeof *p);
	p->size[0] = q->size[0];
	p->size[1] = q->size[1];
	if (!q->head_locked) {
		memcpy(p->position, q->position, sizeof p->position);
		p->orientation[3] = 1.0f;
		return;
	}
	/* Head-locked: relative to the head (the eyes' midpoint), turning with it
	 * (its follow smoothing is left out: it trails by a fraction of a second). */
	const float* hq = frame->views[0].orientation;
	const float* v = q->position;
	const float tx = 2.0f * (hq[1] * v[2] - hq[2] * v[1]);
	const float ty = 2.0f * (hq[2] * v[0] - hq[0] * v[2]);
	const float tz = 2.0f * (hq[0] * v[1] - hq[1] * v[0]);
	const float r[3] = { v[0] + hq[3] * tx + (hq[1] * tz - hq[2] * ty), v[1] + hq[3] * ty + (hq[2] * tx - hq[0] * tz),
						 v[2] + hq[3] * tz + (hq[0] * ty - hq[1] * tx) };
	for (int k = 0; k < 3; ++k)
		p->position[k] = 0.5f * (frame->views[0].position[k] + frame->views[1].position[k]) + r[k];
	memcpy(p->orientation, hq, sizeof p->orientation);
}

void TieVrHands_CommitLayers(const AeronXrHudQuad* hud, const AeronXrHudQuad* hud_world,
							 const AeronXrStereoFrame* frame) {
	if (!frame)
		return;
	if (s_hands.submitted && s_hands.over) {
		int rects[2][4];
		TieVrHandOverlay_Rects(rects);
		Aeron_XrStereoCommitEyeOverlayRect((const int(*)[4])rects);
	}
	if (s_hands.label_count > 0)
		Aeron_XrStereoCommitPanelLayer(AERON_XR_PANEL_LABELS, s_hands.labels, s_hands.label_count);
	s_hands.layer_count = 0;
	TieVrHands_LayerFromQuad(hud, frame);
	TieVrHands_LayerFromQuad(hud_world, frame);
}

void TieVrHands_CaptureGrip(int hand) {
	if (hand >= 0 && hand < 2)
		s_hands.grip_capture[hand] = 1;
}

void TieVrHands_EndFrame(void) {
	if (!s_hands.submitted) {
		TieVrGloves_Idle();
		TieVrLabels_Idle();
		s_hands.over = 0;
	}
	TieVrStick_EndFrame();
	for (int h = 0; h < 2; ++h) {
		Aeron_XrCaptureGrip(h, s_hands.grip_capture[h]);
		s_hands.grip_capture[h] = 0;
	}
	s_hands.submitted = 0;
}

int TieVrHands_TakeStickPlacement(char* out, int size) { return TieVrStick_TakePlacement(out, size); }

void TieVrHands_Shutdown(void) {
	TieVrHandOverlay_Shutdown();
	TieVrLabels_Shutdown();
	TieVrGloves_Shutdown();
	TieVrStick_Shutdown();
	memset(&s_hands, 0, sizeof s_hands);
}

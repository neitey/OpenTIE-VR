#ifndef TIE_REMASTER_FLIGHT_XR_STEREO_H
#define TIE_REMASTER_FLIGHT_XR_STEREO_H

/* OpenXR stereo flight helpers (ported from OpenXWA-VR's flight.c).
 *
 * The flight renderer keeps one AeronScene: the left eye renders the frame's
 * submissions, AeronScene_SetCamera swaps in the right eye and the scene
 * renders again. Eye cameras are the craft camera (snapshot.camera, which
 * also carries TIE's look-around angles) composed with the tracked head
 * pose; the asymmetric eye FOVs become projection offsets. origin_world stays
 * the craft camera's, so every world-relative submission is shared.
 *
 * Settings (environment, read live; names shared with OpenXWA-VR):
 *   XWA_XR_STEREO=0          flight stays flat on the virtual screen
 *   XWA_XR_WORLD_SCALE       metres -> scene units multiplier [1.0]
 *   XWA_XR_DEPTH_BOOST       eye separation multiplier for the world [1.0]
 *   XWA_XR_HUD_LAYOUT        helmet (default) | fixed
 *   XWA_XR_HUD_DISTANCE      helmet HUD distance in metres [3.0]
 *   XWA_XR_HUD_SCALE         helmet HUD size factor [0.65]
 *   XWA_XR_HUD_SMOOTH        helmet HUD follow time constant, s [0.18]
 *   XWA_XR_HUD_WORLD_DISTANCE target-box quad distance, metres [500]
 *   XWA_XR_SSAO=1            keep SSAO in stereo (off by default)
 *   XWA_XR_SHADOWS=0         no directional shadows in stereo
 *   XWA_XR_BLOOM=0           no bloom in stereo */

#include <stdbool.h>

#include "aeron/xr.h"
#include "tie_remaster/flight/render_math.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum TieFlightXrHudLayout {
	TIE_FLIGHT_XR_HUD_HELMET = 0, /* head-locked sheet + far world quad for the target box */
	TIE_FLIGHT_XR_HUD_FIXED = 1,  /* one sheet fixed in the craft frame (everything on it) */
} TieFlightXrHudLayout;

/* Positive float from the environment, else `fallback`. */
float TieFlightXr_Env(const char* name, float fallback);
/* XWA_XR_STEREO=0 turns stereo flight off. */
bool TieFlightXr_StereoEnabled(void);
TieFlightXrHudLayout TieFlightXr_HudLayout(void);
float TieFlightXr_DepthBoost(void);
float TieFlightXr_UnitsPerMeter(void);

/* Scene camera of one eye: `base` (the craft camera) composed with the view
 * pose; FOV and projection offsets from the view's asymmetric angles. */
void TieFlightXr_EyeCamera(const AeronSceneCamera* base, const AeronXrView* view, AeronSceneCamera* out);

/* Per-eye flight cameras (depth boost applied) and a camera spanning both
 * eyes (between them, the union of their FOVs) for culling, shadow fitting
 * and camera-facing sprites. `base` is the flat camera built for the eye
 * size; its origin and fit rectangle carry over. near_shift[e] (optional):
 * how far the depth boost moved eye e (scene units); near geometry such as a
 * cockpit moves by it to keep its real size. */
void TieFlightXr_BuildEyes(const TieFlightCamera* base, const AeronXrStereoFrame* frame, TieFlightCamera eyes[2],
						   TieFlightCamera* both, float near_shift[2][3]);

/* HUD quads. head = 1: the helmet sheet (head-locked); head = 0: the quad fixed
 * in LOCAL space that matches `hud_cam`'s frustum (what was drawn at a pixel
 * of the HUD target lines up with the world from the seated head position),
 * far away when `world_far` (helmet layout's target box), else at the HUD
 * distance (fixed layout). */
void TieFlightXr_HudQuad(const TieFlightCamera* hud_cam, int head, int world_far, AeronXrHudQuad* out);

/* The area of a HUD target the 4:3 cockpit/HUD is laid out in: the whole
 * target when it is at least 4:3, else a centred full-width 4:3 band (the
 * flat layout would crop the sides, where the sensors are). area = x, y, w, h. */
void TieFlightXr_HudArea(int target_w, int target_h, int area[4]);
/* HudQuad for a HUD laid out in `area` of a target_w x target_h target. */
void TieFlightXr_HudQuadForArea(const struct TieCameraState* camera, int target_w, int target_h, const int area[4],
								int head, int world_far, AeronXrHudQuad* out);

#ifdef __cplusplus
}
#endif

#endif

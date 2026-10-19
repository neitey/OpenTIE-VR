#ifndef TIE_REMASTER_VR_HANDS_H
#define TIE_REMASTER_VR_HANDS_H

/*
 * The player's hands in stereo flight: the glue between the flight
 * renderer and the modules ported from OpenXWA-VR (all in this folder):
 *
 *   vr_controllers   controller models (the runtime's own, else a generic one)
 *   vr_gloves        CC0 pilot gloves (XWA_XR_HANDS = controllers|gloves|off)
 *   vr_flight_stick  a grabbable centre stick (XWA_XR_FLIGHT_STICK)
 *   vr_hand_overlay  hands in front of the HUD layers (eye overlay alpha)
 *   vr_controller_labels  what each button does, next to the controllers
 *
 * Per host frame:
 *   TieVrHands_BeginFrame()                 before anything records
 *   TieVrHands_Submit(...)                  flight prepare (stereo), scene open
 *   TieVrHands_EyeAlpha(e)                  drawn into eye e's output
 *   TieVrHands_FinishRightEye(cmd)          between the eyes
 *   TieVrHands_RenderLayers(cmd, frame)     after the eyes and the HUD
 *   TieVrHands_CommitLayers(hud, world, f)  after the HUD commit
 *   TieVrHands_EndFrame()                   after rendering
 * A frame without Submit (menus, flat flight) shows no hands and lets go of
 * the stick.
 */

#include "aeron/render.h"
#include "aeron/scene/draw_list2d.h"
#include "aeron/scene/scene3d.h"
#include "aeron/xr.h"

#ifdef __cplusplus
extern "C" {
#endif

void TieVrHands_BeginFrame(void);

/* Stereo flight prepare, no pass open on `cmd`: flight stick update, the
 * hands and the stick as scene meshes, the hands' overlay masks.
 * `craft_cam` is the seated head's camera (the craft camera the eye cameras
 * are built from; LOCAL space hangs off it), `units` scene units per metre
 * (world scale applied). */
void TieVrHands_Submit(AeronScene3D* scene, AeronCommandBuffer* cmd, const AeronSceneCamera* craft_cam, float units,
					   const AeronXrStereoFrame* frame);

/* The menu scene (flight/xr_env.c), scene open, no pass on `cmd`: the hands
 * (no flight stick) and their overlay masks against `screen` (the virtual
 * screen's quad in LOCAL space; NULL = in front of everything). After the
 * eyes, TieVrHands_CommitMenu commits the overlay. */
void TieVrHands_SubmitMenu(AeronScene3D* scene, AeronCommandBuffer* cmd, const AeronSceneCamera* craft_cam,
						   float units, const AeronXrStereoFrame* frame, const AeronXrPanel* screen);
void TieVrHands_CommitMenu(void);

/* The alpha of eye `eye` for the overlay (draw it into the eye image after
 * its colour, in the same pass); NULL when there is none this frame. */
AeronDrawList2D* TieVrHands_EyeAlpha(int eye);
/* After the left eye's output, before the right eye renders (no pass open). */
void TieVrHands_FinishRightEye(AeronCommandBuffer* cmd);

/* After the eyes and HUD, no pass open: draws the controller labels' layer. */
void TieVrHands_RenderLayers(AeronCommandBuffer* cmd, const AeronXrStereoFrame* frame);
/* After the HUD commit: the hands' eye overlay and the labels. `hud` (head-
 * locked or not) and `hud_world` are the quads just committed (either may be
 * NULL); the next frame's overlay masks the hands against them. */
void TieVrHands_CommitLayers(const AeronXrHudQuad* hud, const AeronXrHudQuad* hud_world,
							 const AeronXrStereoFrame* frame);

/* After CommitLayers: another committed quad (a VR panel: lap console,
 * side panel, quick wheel) the hands are shown in front of next frame. */
void TieVrHands_AddPanelLayer(const AeronXrPanel* quad);

/* A VR panel owns hand 0/1's grip this frame (the game does not see it). */
void TieVrHands_CaptureGrip(int hand);
/* Whether something (the flight stick, a panel) took hand 0/1's grip so far
 * this frame. */
int TieVrHands_GripCaptured(int hand);

void TieVrHands_EndFrame(void);

/* Where the player moved the flight stick ("x,y,z", for XWA_XR_STICK_POS),
 * once after a move; 0 when nothing new. */
int TieVrHands_TakeStickPlacement(char* out, int size);

void TieVrHands_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

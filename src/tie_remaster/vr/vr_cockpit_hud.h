#ifndef TIE_REMASTER_VR_COCKPIT_HUD_H
#define TIE_REMASTER_VR_COCKPIT_HUD_H

/*
 * VR cockpit HUD (port of OpenXWA-VR's vr_cockpit_hud): TIE Fighter's
 * instruments as separate panels floating around the canopy, the forward
 * sensor up left, the rear sensor up right, the targeting computer (CMD)
 * below the centre, the power bars beside the sensors, the shield/hull and
 * speed/throttle/clock groups lower left and right, the threat readouts above
 * the CMD and the message bar between the window and the CMD. Each panel is an
 * OpenXR quad layer (Aeron_XrStereoCommitPanels) in LOCAL space, the seated
 * head at the origin. The gunsight and target box stay on the far world quad.
 *
 * Content: the cockpit renderer's instrument atlas (TieCockpitRenderer_
 * SetXrPanels): each panel's rectangle of it is copied into the aux HUD
 * target (hud_aux_target; without one, into the rows of hud_target above the
 * message bar). The message panel is the message bar's text in hud_target.
 * Rectangles are the latched (grow-only) union of the panel's instruments.
 *
 * The grip picks a panel up when the hand is on it (within a few cm of its
 * plane) and puts it anywhere (6DoF); the layout is saved per cockpit as
 * XWA_XR_HUD_POSE_<COCKPIT> = "id=x,y,z,qx,qy,qz,qw;..." (LOCAL metres; ids
 * pod_l pod_r msg threat topbar cmd power_l power_r gauges_l gauges_r readouts_l readouts_r;
 * cockpit = the
 * XWA cockpit model, e.g. TIECOCKPIT, or 2D in the 2D cockpit mode). Unset or
 * empty = the built-in layout of that cockpit.
 *
 * Per stereo flight frame, panels mode:
 *   TieVrCockpitHud_RenderContent  after the cockpit's HUD targets, no pass open
 *   TieVrCockpitHud_Build          after the lap console etc., before the commit
 */

#include "aeron/render.h"
#include "aeron/xr.h"

#ifdef __cplusplus
extern "C" {
#endif

struct TieCockpitRenderer;
struct TieSnapshot;

#define TIE_VR_COCKPIT_HUD_MAX_PANELS 12

/* Copies the panels' atlas rectangles into the aux HUD target (or
 * hud_target's free rows). `hud_area` = x, y, w, h of the HUD target the
 * cockpit renderer laid the screen HUD out in. Returns 0 when there is
 * nothing to show (no atlas yet). */
int TieVrCockpitHud_RenderContent(AeronCommandBuffer* cmd, const AeronXrStereoFrame* xr,
								  struct TieCockpitRenderer* cockpit, const struct TieSnapshot* snap,
								  const int hud_area[4]);

/* This frame's panels with their placements (grab and move applied); 0 when
 * RenderContent had nothing. `cockpit_name`: the layout key's cockpit. */
int TieVrCockpitHud_Build(const AeronXrStereoFrame* xr, const char* cockpit_name, AeronXrPanel* out, int max_out);

/* Panels mode off this frame (external view, menus): lets go of a held
 * panel (saving its spot). */
void TieVrCockpitHud_Idle(void);

/* After a move: the setting to save (once). */
int TieVrCockpitHud_TakeLayout(char* key, int key_size, char* value, int value_size);

void TieVrCockpitHud_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

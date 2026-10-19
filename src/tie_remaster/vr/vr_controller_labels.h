#ifndef TIE_REMASTER_VR_CONTROLLER_LABELS_H
#define TIE_REMASTER_VR_CONTROLLER_LABELS_H

/*
 * Labels beside the controller models (VR Options > Hands: Controllers;
 * XWA_XR_CONTROLLER_LABELS=0 turns them off): while the player looks at a
 * controller with its buttons turned towards them, what each control does
 * in flight, a label per control with a leader line to a dot on it. The
 * texts come from the VR binding table (tie_runtime/input/vr_buttons.h), so
 * they follow a changed binding; the shifted (grip held) action is the
 * label's second line.
 *
 * Ported from the controller-labels part of OpenXWA-VR's
 * xwa_remaster/vr_lap_console.c (XwaVrConsole_LabelsFrame); the lap
 * console itself is not part of OpenTIE-VR.
 */

#include "aeron/render.h"
#include "aeron/xr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Between frames (no command buffer recording): loads the labels' font
 * once a headset presents. */
void TieVrLabels_BeginFrame(void);

/* After the eyes and the HUD were drawn, with no pass open on `cmd`: draws
 * the labels' layer (AERON_XR_PANEL_LABELS) when it changed and fills `out`
 * with a quad per controller whose labels show (LOCAL space); returns the
 * count, for Aeron_XrStereoCommitPanelLayer. `views` are this frame's. */
int TieVrLabels_Frame(AeronCommandBuffer* cmd, const AeronXrView views[2], AeronXrPanel out[2]);

/* Not in stereo flight this frame: labels fade out from scratch next time. */
void TieVrLabels_Idle(void);

void TieVrLabels_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

#ifndef TIE_REMASTER_FLIGHT_XR_MENU_BEAM_H
#define TIE_REMASTER_FLIGHT_XR_MENU_BEAM_H

/* VR menus: a laser beam from the right controller to where its aim ray
 * meets the virtual screen, so the player sees where the pointer is. The
 * beam is a thin quad of the stereo frame's beam layer, drawn over the
 * screen (ported from OpenXWA-VR's vr_menu_beam.c).
 *
 *   XWA_XR_BEAM_OPACITY   0.05..1 [0.5]  the screen shows through
 *   XWA_XR_CURSOR_DEPTH   metres the beam stops short of the screen [0.02] */

#include <stdbool.h>

#include "aeron/render.h"
#include "aeron/xr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Draws the beam into `target` (the stereo frame's beam target) and fills
 * `out` with its quad. True when the pointer is on the screen; then commit
 * `out` with Aeron_XrStereoCommitBeams. */
bool TieFlightXrBeam_Frame(AeronCommandBuffer* cmd, AeronRenderTarget* target, int width, int height,
						   const AeronXrView views[2], AeronXrPanel* out);

void TieFlightXrBeam_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

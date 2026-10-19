#ifndef TIE_REMASTER_VR_HAND_OVERLAY_H
#define TIE_REMASTER_VR_HAND_OVERLAY_H

/*
 * Hands in front of the menu screen and the flight HUD. The virtual
 * screen and its frame, and in flight the HUD panels, the helmet HUD and
 * the world HUD (reticle), are compositor layers over the 3D view, so they
 * covered the gloves / controllers reaching in front of them. The eye
 * images are shown a second time over those layers, only in the hands'
 * box (Aeron_XrStereoCommitEyeOverlayRect), with an alpha that is 1 only
 * on the hands: their outline seen from each eye, built here.
 */

#include "aeron/render.h"
#include "aeron/scene/draw_list2d.h"
#include "aeron/xr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* In flight the layers to come out in front of are the HUD's (cockpit
 * panels, the helmet HUD, the world HUD with the reticle), the lap console
 * and the cards, given here before Prepare, in LOCAL space (position,
 * orientation, size of each quad). The hands and the flight stick stay
 * behind a panel they reach through. Without this call the menu screen
 * and its frame are the layers. Used by one Prepare. */
void TieVrHandOverlay_SetLayers(const AeronXrPanel* quads, int count);

/* After the gloves (or controllers) and the stick were submitted for this
 * frame, with no pass open on `cmd`: builds each eye's alpha mask (eye
 * images w x h; the right eye's is finished by Finish). Returns 1 when a
 * hand or the stick is in view of either eye over a layer (then the
 * overlay is worth showing). */
int TieVrHandOverlay_Prepare(AeronCommandBuffer* cmd, const AeronXrView views[2], int w, int h);

/* Before the right eye is drawn, with no pass open on `cmd`: takes in its
 * mask (filled on a worker thread while the left eye was drawn). Nothing
 * to do when Prepare returned 0. */
void TieVrHandOverlay_Finish(AeronCommandBuffer* cmd);

/* Eye `eye`'s alpha list, to draw into the eye image after its colour
 * (it writes only the alpha, inside Rect); NULL when there is none. */
AeronDrawList2D* TieVrHandOverlay_List(int eye);

/* The part of each eye image the overlay needs ({ x, y, w, h }: the hands'
 * box), for Aeron_XrStereoCommitEyeOverlayRect. */
void TieVrHandOverlay_Rects(int rect[2][4]);

void TieVrHandOverlay_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

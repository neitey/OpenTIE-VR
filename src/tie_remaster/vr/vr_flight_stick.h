#ifndef TIE_REMASTER_VR_FLIGHT_STICK_H
#define TIE_REMASTER_VR_FLIGHT_STICK_H

/*
 * VR flight stick: a centre stick in the cockpit, right of the seat
 * between the knees. Squeeze the right grip with the hand on its grip to
 * take it; moving the hand then tilts it like a real stick (forward = nose
 * down, sideways = yaw; twisting = roll with XWA_XR_STICK_TWIST=1), and it
 * springs back upright when let go. The left hand moves the whole stick
 * (grip on it, move, let go); the spot is saved (XWA_XR_STICK_POS).
 *
 * The deflection goes to Aeron as a virtual stick (Aeron_XrSetVirtualStick),
 * so it drives the same axes as the steering thumbstick (the right one; the
 * left with Swap Thumbsticks, XWA_XR_SWAP_STICKS), which keeps working.
 * XWA_XR_FLIGHT_STICK=0 turns it off.
 */

#include "aeron/render.h"
#include "aeron/scene/scene3d.h"
#include "tie_remaster/vr/vr_gloves.h"

#ifdef __cplusplus
extern "C" {
#endif

int TieVrStick_Enabled(void);

/* Grab, deflection and placement for this frame (stereo cockpit flight
 * only). Call before the lap console and the HUD panels update. */
void TieVrStick_Update(void);

/* Submits the stick's meshes with the cockpit (same space as the gloves:
 * `crows` world->eye rows, `cam_pos`, `units` scene units per metre). `cmd`
 * must have no open pass (meshes are created on first use). */
void TieVrStick_Submit(AeronScene3D* scene, AeronCommandBuffer* cmd, const float crows[9], const float cam_pos[3],
					   float units);

/* The stick's shape as capsules in LOCAL space (hands near it, the
 * panels' grab zones). Returns the count (0 when not shown). */
int TieVrStick_Capsules(TieVrGloveCapsule* out, int max_out);

/* The stick as shown this frame, one mesh in LOCAL space (the hands'
 * overlay over the HUD and panels). Returns 0 when not shown. */
int TieVrStick_Mesh(const float** local_xyz, const float** local_normals, int* vertex_count,
					const uint16_t** indices, int* index_count);

/* Once per host frame after rendering: releases the stick when it was not
 * updated (menus, external views). */
void TieVrStick_EndFrame(void);

/* Where the player moved the stick ("x,y,z"), once after a move. */
int TieVrStick_TakePlacement(char* out, int size);

void TieVrStick_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

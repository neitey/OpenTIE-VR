#ifndef TIE_REMASTER_VR_GLOVES_H
#define TIE_REMASTER_VR_GLOVES_H

/*
 * VR pilot gloves: two dark grey flight gloves that follow the Touch
 * controllers, posed from the trigger, grip and the controllers' finger
 * touch sensors (index off the trigger = pointing). The right glove wears
 * a small Imperial-style rank plaque on the back of the hand above the
 * thumb.
 *
 * The gloves are the CC0 Godot XR Tools glove meshes (vr_glove_mesh.h),
 * skinned on the CPU each frame and drawn as scene mesh instances with the
 * cockpit, so they are lit and seen in stereo like it.
 */

#include "aeron/render.h"
#include "aeron/scene/scene3d.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Updates the hand poses and submits the glove instances. `crows` are the
 * ship camera's world->eye rows, `cam_pos` its position and `units` the
 * scene units per metre of the player's space (world scale applied).
 * `cmd` must have no open pass (meshes are created on first use). */
void TieVrGloves_Submit(AeronScene3D* scene, AeronCommandBuffer* cmd, const float crows[9], const float cam_pos[3],
						float units);

/* Hides the gloves this frame (not in a stereo cockpit view). */
void TieVrGloves_Idle(void);

/* What the player's hands are (VR Options > Hands, XWA_XR_HANDS): the
 * pilot gloves, the controllers (vr_controllers.h; the functions here then
 * draw and describe them instead), or nothing. */
enum { TIE_VR_HANDS_OFF = 0, TIE_VR_HANDS_GLOVES = 1, TIE_VR_HANDS_CONTROLLERS = 2 };
int TieVrHands_Mode(void);

/* The pose of the last submitted frame, in OpenXR LOCAL space (metres). */
typedef struct TieVrGloveCapsule {
	float a[3], b[3];
	float radius;
} TieVrGloveCapsule;

/* The skinned glove of hand 0 (left) / 1 (right) this frame: vertices and
 * normals in LOCAL space (xyz) and its triangle list. Returns 0 when not
 * shown. */
int TieVrGloves_Mesh(int hand, const float** local_xyz, const float** local_normals, int* vertex_count,
					 const uint16_t** indices, int* index_count);

/* Index fingertip of hand 0 (left) / 1 (right). Returns 0 when the hand is
 * not shown. `pointing` is set when the index finger is off the trigger. */
int TieVrGloves_IndexTip(int hand, float out[3], int* pointing);

/* The hand's shape as capsules (palm, fingers, cuff). Returns the count. */
int TieVrGloves_Capsules(int hand, TieVrGloveCapsule* out, int max_out);

void TieVrGloves_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

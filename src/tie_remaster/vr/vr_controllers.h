#ifndef TIE_REMASTER_VR_CONTROLLERS_H
#define TIE_REMASTER_VR_CONTROLLERS_H

/*
 * VR controller models (VR Options > Hands: Controllers), shown in place of
 * the pilot gloves. Where the headset's runtime has its own model of the
 * controller (Quest: XR_FB_render_model; SteamVR / Pico 4 Ultra:
 * XR_EXT_interaction_render_model, with moving buttons) that one is drawn;
 * otherwise a generic controller of our own. They are scene meshes like
 * the gloves (lit with the cockpit, seen in stereo).
 *
 * The labels beside them (vr_controller_labels.c) point at the controls this
 * module locates on the model (TieVrControllers_View).
 */

#include "aeron/render.h"
#include "aeron/scene/scene3d.h"
#include "tie_remaster/vr/vr_gloves.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The controls a label can point at. */
enum {
	TIE_VR_CTL_TRIGGER,
	TIE_VR_CTL_GRIP,
	TIE_VR_CTL_UPPER, /* B / Y */
	TIE_VR_CTL_LOWER, /* A / X */
	TIE_VR_CTL_STICK,
	TIE_VR_CTL_MENU,
	TIE_VR_CTL_THUMBREST,
	TIE_VR_CTL_COUNT
};

/* Where a shown controller's parts are this frame, OpenXR LOCAL space. */
typedef struct TieVrControllerView {
	float anchor[TIE_VR_CTL_COUNT][3]; /* on each control */
	float face_center[3];              /* the middle of the button face */
	float face_normal[3];              /* the way the button face looks */
	float outline[8][3];               /* points around the body */
	int outline_count;
	int runtime_model; /* the headset's own model is shown */
} TieVrControllerView;

void TieVrControllers_Submit(AeronScene3D* scene, AeronCommandBuffer* cmd, const float crows[9],
							 const float cam_pos[3], float units);
void TieVrControllers_Idle(void);
/* Hand 0 (left) / 1 (right). Returns 0 when it is not shown. */
int TieVrControllers_View(int hand, TieVrControllerView* out);
/* For the panels' grab zones and the flight stick (as the gloves' ones):
 * the body as capsules (the last one is the handle, where the palm is),
 * and a light mesh in LOCAL space (the hands' overlay masks). */
int TieVrControllers_Capsules(int hand, TieVrGloveCapsule* out, int max_out);
int TieVrControllers_Mesh(int hand, const float** local_xyz, const float** local_normals, int* vertex_count,
						  const uint16_t** indices, int* index_count);
void TieVrControllers_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

#ifndef TIE_REMASTER_FLIGHT_XWA_COCKPIT_H
#define TIE_REMASTER_FLIGHT_XWA_COCKPIT_H

/* 3D cockpits for OpenTIE-VR.
 *
 * The player's TIE craft gets one of X-Wing Alliance's cockpit models
 * (TIECOCKPIT.OPT, TIEBOMBERCOCKPIT.OPT, COMBATSIMCOCKPIT.OPT) read from the
 * folder in env TIE_XWA_COCKPITS when the player copied them over, else
 * OpenTIE-VR's own cockpit shell (OPENTIE_FIGHTER: an original generated
 * model shipped with the app as resources/cockpit/opentie_cockpit_fighter.glb,
 * made by tools/cockpit_shell/make_cockpit.py). The model is an ordinary
 * scene mesh instance anchored to the player craft with its seat eye point at
 * the flight camera, so every per-eye camera sees it in stereo. TIE's
 * instruments are drawn by TieCockpitRenderer into an instrument atlas and
 * shown on the model as depth-tested panels (scene billboards with free
 * corners).
 *
 * Switches:
 *   TIE_XR_COCKPIT=2d                 keep TIE's 2D cockpit art
 *   TIE_XR_COCKPIT=xwa                XWA's cockpits, the own shell when
 *                                     they are not there; the default when
 *                                     TIE_XWA_COCKPITS is set (the VR app
 *                                     always sets it), else 2d
 *   TIE_XR_COCKPIT=own                the own shell for every craft
 *   TIE_XWA_COCKPITS=<dir>            folder holding the cockpit OPTs
 *   TIE_XR_COCKPIT_FILE=<NAME>        one XWA cockpit for every craft (testing)
 *   TIE_OWN_COCKPIT_GLB=<path>        the own shell from this file instead of
 *                                     the app resources (testing)
 *   TIE_XR_COCKPIT_DEBUG_PITCH/_YAW=deg  turn the cockpit about the eye as if
 *                                     looking down/aside (flat-view testing)
 *   TIE_XR_COCKPIT_EYE_<NAME>=x,y,z   eye point in the cockpit model, OPT
 *                                     metres (x right, y back, z up)
 *   TIE_XR_COCKPIT_SCALE_<NAME>=k     model scale about the eye
 *   TIE_XR_COCKPIT_EMISSIVE=v         instance base_color_emissive_strength
 *                                     (-0.98 caps the cockpit under bloom)
 *   TIE_XR_PANEL_<NAME>=id=x,y,z,pitch,yaw,roll,height;...
 *                                     panel placement override (OPT metres,
 *                                     degrees); ids sensor_fwd, sensor_rear,
 *                                     cmd, gauges_l, gauges_r, off to hide
 * <NAME> is the cockpit file name without extension, e.g. TIECOCKPIT, or
 * OPENTIE_FIGHTER for the own shell. */

#include <stdbool.h>

struct AeronCommandBuffer;
struct AeronScene3D;
struct TieSnapshot;
struct TieFlightAssetSource;
struct TieCockpitRenderer;

#ifdef __cplusplus
extern "C" {
#endif

/* Load (or switch) the cockpit model for the player's craft. Call outside a
 * render pass once per frame; it does work only when the craft changes. */
void TieXwaCockpit_Prepare(struct AeronCommandBuffer* cmd, const struct TieFlightAssetSource* assets,
						   const struct TieSnapshot* snap);

/* True when this snapshot is drawn with the 3D cockpit: model loaded, 3D
 * cockpit selected, cockpit (not external) view and a view in which TIE
 * shows its panel. TieCockpitRenderer then skips its 2D art. */
bool TieXwaCockpit_Drawn(const struct TieSnapshot* snap);

/* A cockpit view in which TIE shows its panel (any cockpit mode): the VR
 * cockpit HUD's instrument panels are shown then. */
bool TieXwaCockpit_PanelView(const struct TieSnapshot* snap);

/* The loaded cockpit model's name (e.g. "TIECOCKPIT", "OPENTIE_FIGHTER"),
 * "" when none. */
const char* TieXwaCockpit_ModelName(void);

/* Add the cockpit instance and the instrument panels to a begun scene. The
 * scene's origin must be snapshot.camera.world_pos. */
void TieXwaCockpit_Submit(struct AeronScene3D* scene, const struct TieSnapshot* snap,
						  const struct TieCockpitRenderer* instruments);

/* Free the model (mission end, profile change, shutdown). */
void TieXwaCockpit_Release(void);

#ifdef __cplusplus
}
#endif

#endif

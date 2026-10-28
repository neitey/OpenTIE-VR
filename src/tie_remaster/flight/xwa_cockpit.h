#ifndef TIE_REMASTER_FLIGHT_XWA_COCKPIT_H
#define TIE_REMASTER_FLIGHT_XWA_COCKPIT_H

/* 3D cockpits for OpenTIE-VR.
 *
 * The player's TIE craft gets one of three kinds of cockpit model:
 * - X-Wing Alliance's (TIECOCKPIT.OPT, TIEBOMBERCOCKPIT.OPT,
 *   COMBATSIMCOCKPIT.OPT) read from the folder in env TIE_XWA_COCKPITS, when
 *   the player's PC launcher copied them over;
 * - X-Wing Revival's TIE cockpits (Raptor007 and the X-Wing Alliance
 *   Upgrade team): REVIVAL_TIEFIGHTER
 *   (also TIE Advanced and Defender), REVIVAL_TIEINTERCEPTOR,
 *   REVIVAL_TIEBOMBER, REVIVAL_GUNBOAT (Assault Gunboat, Missile Boat), app
 *   resources cockpit/revival/<craft>.glb made at package time by
 *   tools/revival_cockpits/gen_revival_cockpits.py (not in the repository);
 * - OpenTIE-VR's own cockpit shell (OPENTIE_FIGHTER: an original generated
 *   model shipped as resources/cockpit/opentie_cockpit_fighter.glb, made by
 *   tools/cockpit_shell/make_cockpit.py).
 * When the chosen kind cannot load for a craft the next one is used, in the
 * order XWA -> Revival -> own -> TIE's 2D cockpit art. The model is an
 * ordinary scene mesh instance anchored to the player craft with its seat
 * eye point at the flight camera, so every per-eye camera sees it in stereo.
 * TIE's instruments are drawn by TieCockpitRenderer into an instrument atlas
 * and shown on the model as depth-tested panels (scene billboards with free
 * corners).
 *
 * Switches:
 *   TIE_XR_COCKPIT=xwa                XWA's cockpits
 *   TIE_XR_COCKPIT=revival            X-Wing Revival's cockpits
 *   TIE_XR_COCKPIT=own                the own shell for every craft
 *   TIE_XR_COCKPIT=2d                 keep TIE's 2D cockpit art
 *   TIE_XR_COCKPIT unset (or auto)    the default rule: XWA's cockpits when
 *                                     they were copied over, else Revival's
 *                                     (the xwa chain); needs TIE_XWA_COCKPITS
 *                                     set (the VR app always sets it), else 2d
 *   TIE_XWA_COCKPITS=<dir>            folder holding the cockpit OPTs
 *   TIE_XR_COCKPIT_FILE=<NAME>        one cockpit for every craft (testing):
 *                                     an XWA name, REVIVAL_<CRAFT> or
 *                                     OPENTIE_FIGHTER
 *   TIE_REVIVAL_COCKPITS=<dir>        the Revival .glb files from this folder
 *                                     instead of the app resources (testing)
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
 * <NAME> is the cockpit's table name: the XWA file name without extension,
 * e.g. TIECOCKPIT, REVIVAL_<CRAFT>, or OPENTIE_FIGHTER for the own shell. */

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

/* The loaded cockpit model's name (e.g. "TIECOCKPIT", "REVIVAL_TIEFIGHTER",
 * "OPENTIE_FIGHTER"), "" when none. */
const char* TieXwaCockpit_ModelName(void);

/* Add the cockpit instance and the instrument panels to a begun scene. The
 * scene's origin must be snapshot.camera.world_pos. */
void TieXwaCockpit_Submit(struct AeronScene3D* scene, const struct TieSnapshot* snap,
						  const struct TieCockpitRenderer* instruments);

/* True when at least one of X-Wing Alliance's cockpit files is in the
 * TIE_XWA_COCKPITS folder (the settings page's default choice). */
bool TieXwaCockpit_XwaFilesPresent(void);

/* Free the model (mission end, profile change, shutdown). */
void TieXwaCockpit_Release(void);

#ifdef __cplusplus
}
#endif

#endif

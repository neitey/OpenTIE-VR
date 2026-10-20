#ifndef TIE_REMASTER_FLIGHT_XR_ENV_H
#define TIE_REMASTER_FLIGHT_XR_ENV_H

/* OpenXR menu environment (after OpenXWA-VR's vr_env.c): while the headset
 * shows the menus, briefings, films or the loading screen on the virtual
 * screen, the eyes see an Imperial fleet in deep space around it: stars, a
 * nebula, a planet and a sun (all generated), an Imperial Star Destroyer and
 * TIE fighters flying past, built from the player's own flight models
 * (the IVFILES OPT files, through the selected flight asset source). The scene renders
 * in stereo every host frame through the flight renderer's AeronScene, then
 * Aeron_XrStereoCommitBackdrop lets the engine compose the virtual screen in
 * front of it. The right controller's pointer beam is drawn over the screen.
 *
 * Settings (environment, read live; names shared with OpenXWA-VR):
 *   XWA_XR_ENVIRONMENT      tiefighter (default, also unset) the TIE Fighter box art |
 *                           defender the Defender of the Empire box art (a TIE
 *                           Bomber, TIE Advanced, an A-wing blown apart before a TIE
 *                           pilot's helmet, vr/xvt_tie_pilot.rgba) |
 *                           balance the Balance of Power box art (B-wings, an A-wing
 *                           and TIE Advanced round a Super Star Destroyer before a
 *                           brown world) |
 *                           fleet (or 1) the Imperial fleet | 0 (or stars) stars only |
 *                           off nothing
 *   XWA_XR_TIE_PILOT        the Defender of the Empire helmet: a path, 0 = none
 *                           [assets/vr/xvt_tie_pilot.rgba, packaged; never in the repo]
 *   XWA_XR_PILOT_BRIGHTNESS / _PITCH   that helmet [1.0, 16 deg]
 *   XWA_XR_PLANET_BRIGHTNESS the Balance of Power world [1.0]
 *   XWA_XR_ENV_FADE_SECONDS the TIE Fighter scene's fade in [1.5]
 *   XWA_XR_ENV_BACK         the viewer's distance behind the TIE Fighter scene's
 *                           centre [1.1 m]
 *   XWA_XR_TIEART_SPEED     the TIE Fighter scene's craft speed factor [1.0]
 *   XWA_XR_VADER            the helmet image: a path, 0 = none
 *                           [assets/vr/vader.rgba, packaged; never in the repo]
 *   XWA_XR_VADER_BRIGHTNESS / _PITCH / _FADE_SECONDS   the helmet [1.0, 31 deg, 2.5 s]
 *   XWA_XR_ENDOR_SUN        the TIE Fighter scene's key light [1.6]
 *   XWA_XR_SCENE_SFX_VOLUME the TIE Fighter scene's sounds, on top of the
 *                           in-flight effects volume [1.0; 0 = silent]
 *   XWA_XR_LOGO             the logo above the screen and on the startup title:
 *                           an image path, 0 = none
 *                           [assets/vr/logo.rgba, packaged; never in the repo]
 *   XWA_XR_LOGO_SCALE       the logo's size [1.0]
 *   XWA_XR_TITLE            0: no startup show (the screen stays up while the
 *                           TIE Fighter scene's fight first plays, then no title)
 *                           [1]
 *   XWA_XR_TITLE_FADE_SECONDS the title's fade in [1.2]
 *   TIE_XR_ENV_TIME         tests: the scene held at this many seconds
 *   XWA_XR_ENV_BRIGHTNESS   displayed brightness of the scene [0.8]
 *   XWA_XR_ENV_FILM_DIM     brightness factor during films/loading [0.5]
 *   XWA_XR_FRAME            the frame round the screen: an image path, 0 = none
 *                           [assets/vr/frame.rgba, packaged; never in the repo]
 *   XWA_XR_FRAME_DEPTH      the frame's distance in front of the picture [0.02 m]
 *   XWA_XR_FRAME_OVERSCAN   > 1 shows the picture smaller in the frame [1.0] */

#include <stdbool.h>
#include <stdint.h>

#include "aeron/render.h"

#ifdef __cplusplus
extern "C" {
#endif

struct TieFlightRenderer;
struct TieFlightAssetSource;

typedef enum TieFlightXrEnvMode {
	TIE_FLIGHT_XR_ENV_OFF = 0,
	TIE_FLIGHT_XR_ENV_STARS = 1,
	TIE_FLIGHT_XR_ENV_FLEET = 2,
	TIE_FLIGHT_XR_ENV_TIEFIGHTER = 3,
	TIE_FLIGHT_XR_ENV_DEFENDER = 4,
	TIE_FLIGHT_XR_ENV_BALANCE = 5,
} TieFlightXrEnvMode;

TieFlightXrEnvMode TieFlightXrEnv_Mode(void);

/* Renders the environment into this frame's eyes and commits it as the
 * backdrop of the virtual screen (plus the pointer beam unless `dimmed`).
 * `ships`: build and show the craft (false keeps the sky only, e.g. on the
 * in-flight map, without loading models mid-mission). `dimmed` eases the
 * scene down (films, loading). Returns false when nothing was committed
 * (setting off, no XR frame, failure). */
bool TieFlightXrEnv_Render(struct TieFlightRenderer* renderer, const struct TieFlightAssetSource* assets,
						   uint64_t time_us, bool ships, bool dimmed);

/* Frees the craft meshes (entering a mission); textures stay. */
void TieFlightXrEnv_ReleaseShips(void);
void TieFlightXrEnv_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

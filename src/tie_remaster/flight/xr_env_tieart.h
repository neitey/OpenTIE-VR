#ifndef TIE_REMASTER_FLIGHT_XR_ENV_TIEART_H
#define TIE_REMASTER_FLIGHT_XR_ENV_TIEART_H

/* "TIE Fighter": the TIE Fighter box art around the virtual screen, played
 * (ported from OpenXWA-VR's vr_env.c, ENV_TIEART). Internal to the menu
 * environment (xr_env.c), which owns the frame loop, the stars, the eyes and
 * the screen's frame; this module owns the scene: its craft (the player's own
 * TIE Fighter flight models), the seven-second fight that ends on the cover's
 * moment, its laser fire and explosions (TIE Fighter's own XACT sprites),
 * Vader's helmet over purple clouds (vr/vader.rgba, packaged, never in the
 * repository), the scene's lighting and sounds (TIE Fighter's own flight
 * sound effects), and the startup show: the first time the menus come up the
 * screen stays away while the fight plays (A skips to its end), then comes
 * in. Units: the scene's world units (AERON_OPT_UNITS_PER_METER per metre),
 * TIE world axes (x right, y ahead, z up). */

#include <stdbool.h>
#include <stdint.h>

#include "aeron/render.h"

struct AeronScene3D;
struct TieFlightAssetSource;

typedef struct TieXrArtLighting {
	float key_dir[3], key_color[3];
	float fill_dir[3], fill_color[3];
	float ambient_sides[3], ambient_top[3], ambient_bottom[3];
} TieXrArtLighting;

/* Loads one model, sprite set, image or the sounds per call; true once
 * everything was tried (loaded or not). `ships` false: the sky only. */
bool TieXrArt_Prepare(AeronCommandBuffer* cmd, const struct TieFlightAssetSource* assets, bool ships);
/* Advances the fight (from the first frame the scene is up): dt in seconds,
 * `head_q` the head's orientation (LOCAL, x y z w) for the sounds' pan.
 * TIE_XR_ENV_TIME=<s> instead plays it silently up to that moment, then
 * holds it there (tests). */
void TieXrArt_Advance(float dt, const float head_q[4]);
/* Queues the craft (unless `ships` is false) and the sprites; `fade` 0..1 is
 * the scene's fade in. */
void TieXrArt_Submit(struct AeronScene3D* scene, bool ships, float fade);
/* Where the viewer sits (world units): XWA_XR_ENV_BACK behind the scene's
 * centre. */
void TieXrArt_ViewPos(float out[3]);
void TieXrArt_Lighting(TieXrArtLighting* out);
/* The startup show: 1 = screen shown .. 0 = away (A still reaches us). */
float TieXrArt_ScreenReveal(void);
/* Another scene came up first this run: no startup show (it belongs to the
 * first menus only, as in OpenXWA-VR). */
void TieXrArt_SkipStartupShow(void);
/* The fight starts over (entering a mission, another scene). */
void TieXrArt_Restart(void);
/* True when there was something to release (craft, a fight under way). */
bool TieXrArt_ReleaseShips(void);
void TieXrArt_Shutdown(void);

#endif

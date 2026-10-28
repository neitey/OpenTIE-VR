#ifndef TIE_REMASTER_FLIGHT_XR_ENV_TIEART_H
#define TIE_REMASTER_FLIGHT_XR_ENV_TIEART_H

/* "TIE Fighter", "Defender of the Empire" and "Balance of Power": the box
 * arts around the virtual screen, played. The TIE Fighter one is described
 * here; the other two use the same machinery with their own craft, backdrop
 * (the TIE pilot's helmet, vr/xvt_tie_pilot.rgba; a generated brown world
 * behind the Super Star Destroyer) and light (OpenXWA-VR's ENV_DOEART,
 * ENV_BOPART). "X-Wing" and "X-Wing vs. TIE Fighter" are OpenXWA-VR's stills
 * of those box arts (ENV_BOXART, ENV_XVTART) with TIE Fighter's own craft:
 * the purple nebula and a fighter bursting; the two pilots behind the
 * explosion and its pillar of light (vr/xvt_tie_pilot.rgba and
 * vr/xvt_rebel_pilot.rgba, packaged, never in the repository).
 *
 * "TIE Fighter": the TIE Fighter box art around the virtual screen, played
 * (ported from OpenXWA-VR's vr_env.c, ENV_TIEART). Internal to the menu
 * environment (xr_env.c), which owns the frame loop, the stars, the eyes and
 * the screen's frame; this module owns the scene: its craft (the player's own
 * TIE Fighter flight models), the seven-second fight that ends on the cover's
 * moment, its laser fire and explosions (TIE Fighter's own XACT sprites),
 * Vader's helmet over purple clouds (vr/vader.rgba, packaged, never in the
 * repository), the scene's lighting and sounds (TIE Fighter's own flight
 * sound effects), the logo above the screen (vr/logo.rgba) and the startup
 * show: the first time the menus come up the screen stays away while the
 * fight plays (A skips to its end), then the title (the logo, "Press (A) to
 * Continue") until A brings the screen in. Units: the scene's world units (AERON_OPT_UNITS_PER_METER per
 * metre), TIE world axes (x right, y ahead, z up). */

#include <stdbool.h>
#include <stdint.h>

#include "aeron/render.h"

struct AeronScene3D;
struct TieFlightAssetSource;

/* The virtual screen this frame (xr_env.c): metres, head-relative. The 4:3
 * picture pic_w x pic_h, its centre `lift` above eye level, `dist` ahead;
 * the frame (when there is one) frame_dist ahead, the picture scaled by
 * frame_scale into it, its bezel frame_top above the picture's top (in the
 * picture's plane); `units` scene units per metre (world scale). */
typedef struct TieXrArtScreen {
	float dist, lift, pic_w, pic_h;
	float frame_dist, frame_scale, frame_top;
	float frame_bottom; /* the bezel's lower edge below the picture's (its plane) */
	float units;
} TieXrArtScreen;

typedef struct TieXrArtLighting {
	float key_dir[3], key_color[3];
	float fill_dir[3], fill_color[3];
	float ambient_sides[3], ambient_top[3], ambient_bottom[3];
} TieXrArtLighting;

typedef enum TieXrArtScene {
	TIE_XR_ART_TIEFIGHTER = 0,
	TIE_XR_ART_DEFENDER,
	TIE_XR_ART_BALANCE,
	/* Stills (OpenXWA-VR's ENV_BOXART, ENV_XVTART): no fight; the startup
	 * show's title comes at once. */
	TIE_XR_ART_XWING, /* the X-Wing box art: X-wings and TIE fighters over a purple nebula */
	TIE_XR_ART_XVT,   /* the X-Wing vs. TIE Fighter box art: its two pilots behind an explosion */
} TieXrArtScene;

/* The menus' chrome in every scene (not only the box arts): the logo and
 * the button hints' texture, one step per call. */
void TieXrArt_PrepareChrome(AeronCommandBuffer* cmd);
/* The button hints under the frame ("X Toggle 2D screen", "Y Change VR
 * environment") at opacity `hints`, and the logo large where the screen is
 * while it is put away at `logo` (0 none); `view_pos` the viewer (world
 * units). Needs TieXrArt_SetScreen. */
void TieXrArt_SubmitChrome(struct AeronScene3D* scene, const float view_pos[3], float hints, float logo);
/* The screen's reveal by X (1 shown .. 0 away): the logo above the frame
 * goes with it. */
void TieXrArt_SetDismissReveal(float reveal);
/* The startup show's title is still to come or up (X and Y wait). */
bool TieXrArt_TitleActive(void);

/* The box art shown (the fight starts over when it changes). */
void TieXrArt_SetScene(TieXrArtScene scene);
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
/* The screen this frame, for the logo above it and the startup title. */
void TieXrArt_SetScreen(const TieXrArtScreen* screen);
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

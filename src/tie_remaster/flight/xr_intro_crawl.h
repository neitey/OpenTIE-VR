#ifndef TIE_REMASTER_FLIGHT_XR_INTRO_CRAWL_H
#define TIE_REMASTER_FLIGHT_XR_INTRO_CRAWL_H

/* VR: TIE Fighter's opening title crawl (shell scene 8, TITLE.LFD) as a 3D
 * scene round the player, after OpenXWA-VR's vr_intro_crawl.c (without its
 * Hoth shot). The game runs its title scene as always (its clock, music and
 * buttons); while it does, the virtual screen is put away and in its place:
 *   - "A long time ago in a galaxy far, far away...." (the game's own "along"
 *     picture, TITLE.LFD, in its own colours) where the screen would be;
 *   - the stars and the STAR WARS logo receding from the viewer, sized frame
 *     by frame as the game scales its logo;
 *   - the crawl's text (the game's own, TITLE.LFD's "title" paragraph, as
 *     the snapshot carries it), centred lines on a huge tilted plane in
 *     OpenXWA-VR's film-measured perspective, crawling away at the game's
 *     pace (a line every 28 frames) and fading out with the game's fade.
 * When the game moves on (to the intro film, shell scene 10) the screen comes
 * back with the film on it. While it plays the controllers reach nothing but
 * A (X when left-handed) and the left menu button, which skip it to the
 * intro film (the game's Space on its title). The battle intro crawls ("todtxtN")
 * stay on the screen.
 *
 * Needs, as APK assets (never in the repository): vr/starwars_logo.rgba
 * (XWAL; only its alpha is used) and vr/LiberationSans-BoldItalic.ttf (SIL OFL
 * 1.1, rasterized here with stb_truetype). Without either, or with
 * XWA_XR_INTRO_CRAWL=0 (VR settings: "Intro: 3D Crawl"), the crawl plays on
 * the screen as before.
 *
 * Tuning: XWA_XR_CRAWL_HEIGHT (metres between the eye and the crawl's plane,
 * 12), XWA_XR_CRAWL_LOGO_DISTANCE (metres to the logo when it appears, 5),
 * XWA_XR_CRAWL_TEXTURE (the text texture's width, 4096),
 * XWA_XR_CRAWL_TEXT_ENTRY_DEG (where the first line comes into view: degrees
 * below the logo's line of sight, 35); TIE_XR_CRAWL_FRAME (tests: hold the
 * scene at that frame of the game's title clock). */

#include <stdbool.h>
#include <stdint.h>

#include "aeron/render.h"

#ifdef __cplusplus
extern "C" {
#endif

struct AeronScene3D;
struct TieSnapshot;
struct TieFlightAssetSource;

/* Once a host frame, before the VR menu scene renders: what the game shows.
 * `tie98_frames`: the TIE98 frontend's title clock runs 4 frames later. */
void TieXrIntroCrawl_SetSnapshot(const struct TieSnapshot* snap, bool tie98_frames);

/* The opening title crawl is up (shell scene 8) and wanted in 3D (it may
 * still play on the screen without its assets). */
bool TieXrIntroCrawl_TitleUp(void);

/* Every VR menu-scene frame: loads the logo, the font and the game's "along"
 * picture when the title starts, rasterizes the text once the game has it,
 * frees them after. */
void TieXrIntroCrawl_Update(AeronCommandBuffer* cmd, const struct TieFlightAssetSource* assets);

/* Nonzero while the 3D scene plays (the virtual screen put away). */
bool TieXrIntroCrawl_Active(void);

/* While it plays (the controllers blocked): A (X when left-handed) or the
 * left menu button skips it, as Space does on the title: on to the intro
 * film. Nothing else does. */
void TieXrIntroCrawl_PollSkip(void);

/* The menus' stars are shown (from the logo's cut on). */
bool TieXrIntroCrawl_StarsOn(void);

/* Queues the picture, the logo and the text; `view_pos` the seated viewer
 * (world units), `units` world units per metre. World axes as the VR menu
 * scene's: x right, y ahead, z up. */
void TieXrIntroCrawl_Submit(struct AeronScene3D* scene, const float view_pos[3], float units);

void TieXrIntroCrawl_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

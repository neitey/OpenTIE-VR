#ifndef TIE_REMASTER_FLIGHT_COCKPIT_RENDERER_H
#define TIE_REMASTER_FLIGHT_COCKPIT_RENDERER_H

/* Owns the cockpit bitmap, HUD atlas, damage overlay, and their draw state. */

#include <stdbool.h>
#include <stdint.h>

struct AeronCommandBuffer;
struct AeronRenderPass;
struct AeronRenderTarget;
struct AeronTexture;
struct TieSnapshot;
struct TieScene2dTextRenderer;
struct TieFlightRenderer;
struct StringsDat;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TieCockpitRenderer TieCockpitRenderer;
struct TieFlightAssetSource;

/* Initialise. `cmd` receives startup texture uploads.
 * `assets` is the selected flight asset provider. `rt_format` is the color format of the flight RT we
 * render onto (drives pipeline cache lookup). `text_renderer` is borrowed
 * — used to render DIGIT_FIELD / TEXT_FIELD / STATIC_LABEL glyphs;
 * pass NULL to skip text-based widgets entirely. */
TieCockpitRenderer* TieCockpitRenderer_Init(struct AeronCommandBuffer* cmd,
											const struct TieFlightAssetSource* assets,
											uint32_t /*AeronTextureFormat*/ rt_format,
											struct TieScene2dTextRenderer* text_renderer);
void TieCockpitRenderer_Shutdown(TieCockpitRenderer* cg);

/* Bind a TieFlightRenderer handle to use for the 3DCRT picture-in-picture
 * pre-pass. NULL unbinds — the 3DCRT widget then renders only the
 * background colour from the cockpit atlas, with no target mesh. */
void TieCockpitRenderer_SetFlightRenderer(TieCockpitRenderer* cg, struct TieFlightRenderer* fg);

/* Request a from-disk reload of all cockpit assets (layout YAML, base
 * canopy bitmap, HUD parts atlas, damage overlay, CRT mask) for artist
 * iteration. Deferred: the cache clear happens at the start of the next
 * TieCockpitRenderer_Prepare(); the active cockpit re-loads that frame, other
 * cached variants lazily on next use. Safe to call from the debug UI. */
void TieCockpitRenderer_RequestReload(void);

/* Prepare performs uploads and PIP rendering before the flight pass opens.
 * RenderInPass emits cockpit content when present plus global flight HUD.
 * Non-flight and replay snapshots are no-ops. */
bool TieCockpitRenderer_Prepare(TieCockpitRenderer* cg, struct AeronCommandBuffer* cmd,
								const struct TieSnapshot* snap);

void TieCockpitRenderer_RenderInPass(TieCockpitRenderer* cg, struct AeronCommandBuffer* cmd,
									 struct AeronRenderPass* pass, struct AeronRenderTarget* target,
									 const struct TieSnapshot* snap);

/* 3D cockpit (OpenTIE-VR): while TieXwaCockpit_Drawn(snap), Prepare draws the
 * cockpit panel (TIE's instruments over its own panel art) into an offscreen
 * instrument atlas instead of the flight RT, and the flight RT gets only the
 * screen-space HUD (target box, gunsight, message bar). The atlas regions
 * below are shown as panels on the 3D cockpit model. */
typedef enum TieCockpitPanelId {
	TIE_COCKPIT_PANEL_SENSOR_FWD = 0,
	TIE_COCKPIT_PANEL_SENSOR_REAR,
	TIE_COCKPIT_PANEL_CMD,
	TIE_COCKPIT_PANEL_GAUGES_L,
	TIE_COCKPIT_PANEL_GAUGES_R,
	TIE_COCKPIT_PANEL_COUNT
} TieCockpitPanelId;

typedef struct TieCockpitPanelSource {
	bool valid;
	float uv[4];  /* u0, v0, u1, v1 in the atlas texture */
	float aspect; /* displayed width / height */
} TieCockpitPanelSource;

/* The atlas texture (NULL when no atlas was drawn this frame) and the panel
 * rectangles in it. */
struct AeronTexture* TieCockpitRenderer_InstrumentAtlas(const TieCockpitRenderer* cg,
														TieCockpitPanelSource out[TIE_COCKPIT_PANEL_COUNT]);

#ifdef __cplusplus
}
#endif

#endif

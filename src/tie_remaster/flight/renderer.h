#ifndef TIE_REMASTER_FLIGHT_RENDERER_H
#define TIE_REMASTER_FLIGHT_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include "aeron/render.h"
#include "aeron/scene/present.h"
struct AeronCommandBuffer;
struct AeronRenderPass;
struct TieSnapshot;
struct TieFlightRenderConfig;
struct TieFlightPbrConfig;
struct TieFlightPointLightParams;
struct TieFlightAssetSource;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TieFlightRenderer TieFlightRenderer;

/* Creates the flight render targets. Returns NULL after requesting a fatal
 * renderer error on failure. `remaster_dir` supplies ambient-light data;
 * flight presentation assets come from `assets`. */
TieFlightRenderer* TieFlightRenderer_Init(int rt_w, int rt_h, AeronTextureFormat rt_format,
										  AeronTextureFormat target_format, const char* remaster_dir,
										  const struct TieFlightAssetSource* assets,
										  const struct TieFlightRenderConfig* render,
										  const struct TieFlightPbrConfig* pbr,
										  const struct TieFlightPointLightParams* point_lights);

void TieFlightRenderer_Shutdown(TieFlightRenderer* g);

/* Discard temporal renderer history when flight rendering is suspended. */
void TieFlightRenderer_InvalidateHistory(TieFlightRenderer* g);

/* Release mission-sized meshes, sprites, and skybox data while retaining the
 * renderer, render targets, pipelines, and cockpit bindings. */
void TieFlightRenderer_ReleaseMissionAssets(TieFlightRenderer* g);

/* Rebuild only the output-size-dependent scene and bloom resources.
 * Persistent pipelines, assets, mesh caches, and the fixed-size PIP stay live. */
bool TieFlightRenderer_EnsureOutputSize(TieFlightRenderer* g, int rt_w, int rt_h);
bool TieFlightRenderer_ApplyQuality(TieFlightRenderer* g, int ssao_quality, bool shadows_enabled,
									int shadow_atlas_size, int fsr_mode, float fsr_sharpness,
									int motion_blur_quality, float motion_blur_shutter, int msaa_samples);
void TieFlightRenderer_SetStarfieldStyle(TieFlightRenderer* g, int style);

/* Borrows the HDR scene render target. */
AeronRenderTarget* TieFlightRenderer_SceneRt(const TieFlightRenderer* g);

/* Tonemaps the HDR scene and bloom into the active render pass. The pass must
 * match `target_format`. `present_tint` is an optional post-tonemap RGB
 * multiplier and premultiplied-alpha coverage. */
void TieFlightRenderer_Present(TieFlightRenderer* g, AeronCommandBuffer* cmd, AeronRenderPass* pass,
							   const float present_tint[4]);
void TieFlightRenderer_PresentSwapchain(TieFlightRenderer* g, AeronCommandBuffer* cmd, AeronRenderPass* pass);

/* Reconcile mission-scoped sprites, models, projectiles, and the optional
 * skybox against the snapshot's mission generation. Safe to call from the
 * loading scene; repeated calls for the prepared generation are no-ops. */
bool TieFlightRenderer_PrepareMissionAssets(TieFlightRenderer* g, AeronCommandBuffer* cmd,
											const struct TieSnapshot* snapshot);

/* Prepares interpolated flight state and GPU resources. `cmd` must not have
 * an active pass. Returns false for missing or non-flight snapshots. */
bool TieFlightRenderer_PrepareFrame(TieFlightRenderer* g, AeronCommandBuffer* cmd,
									const struct TieSnapshot* prev, const struct TieSnapshot* curr,
									int32_t delta_us, uint64_t host_time_us, bool paused);

bool TieFlightRenderer_RenderFrame(TieFlightRenderer* g, AeronCommandBuffer* cmd);

/* OpenXR stereo. SetXrFrame(frame) before PrepareFrame makes that frame
 * stereo (NULL = flat): PrepareFrame builds the eye cameras from the frame's
 * views (the renderer's RTs must have the eye size, see EnsureOutputSize),
 * and RenderStereo then renders both eyes and tonemaps them into the
 * frame's eye targets, in place of RenderFrame. The cockpit/HUD hook is
 * skipped in stereo; the caller draws it into the XR HUD target. */
struct AeronXrStereoFrame;
void TieFlightRenderer_SetXrFrame(TieFlightRenderer* g, const struct AeronXrStereoFrame* frame);
bool TieFlightRenderer_RenderStereo(TieFlightRenderer* g, AeronCommandBuffer* cmd);
/* True while the frame being prepared is stereo. */
bool TieFlightRenderer_XrActive(const TieFlightRenderer* g);
/* The stereo frame is the backdrop of a menu on the virtual screen (the
 * settings overlay opened in flight): the hands as in the menus (no flight
 * stick, shown in front of the screen) and the eyes dimmed by
 * TIE_XR_PAUSE_DIM. Set before PrepareFrame; false for ordinary flight. */
void TieFlightRenderer_SetXrMenuBackdrop(TieFlightRenderer* g, bool menu);
/* Brackets (AeronScene_Mark) submissions made during PrepareFrame that sit
 * near the eyes, e.g. a 3D cockpit: with XWA_XR_DEPTH_BOOST > 1 they move
 * with each eye's boost shift so they keep their real size. */
struct AeronSceneMark;
void TieFlightRenderer_XrSetNearRange(TieFlightRenderer* g, const struct AeronSceneMark* begin,
									  const struct AeronSceneMark* end);

/* Current picture-in-picture texture for the cockpit 3D CRT. The frame
 * preparation path uses the target as an integer world origin and rebuilds
 * its local camera from `pip_back_step` plus `pip_cam_ori`. */
AeronTexture* TieFlightRenderer_PipTexture(const TieFlightRenderer* g);

/* Binds the cockpit and flight-HUD compositor to the scene pass. */
struct TieCockpitRenderer;
void TieFlightRenderer_SetCockpit(TieFlightRenderer* g, struct TieCockpitRenderer* cockpit);

/* Returns the process-wide live renderer, or NULL. */
TieFlightRenderer* TieFlightRenderer_Current(void);

/* Applies bloom outside any active pass. The message bar is excluded from
 * bloom generation and composition. Invalid inputs and non-flight scenes are
 * no-ops. */
bool TieFlightRenderer_ApplyBloom(TieFlightRenderer* g, AeronCommandBuffer* cmd,
								  const struct TieSnapshot* snap);

/* Borrows bloom mip 0, or returns NULL when bloom is unavailable. */
AeronRenderTarget* TieFlightRenderer_BloomRt(const TieFlightRenderer* g);

/* Present uniforms. Call after ApplyBloom for the current message-bar bound. */
typedef struct TieFlightRendererBloomParams {
	float intensity; /* multiplier applied to the bloom contribution */
	float texel_x;   /* 1.0 / flight_rt_width  (kernel offset in UV) */
	float texel_y;   /* 1.0 / flight_rt_height (kernel offset in UV) */
	float bar_y_uv;  /* UV.y at the message-bar top (1.0 = no bar)   */
} TieFlightRendererBloomParams;

void TieFlightRenderer_BloomParams(const TieFlightRenderer* g, TieFlightRendererBloomParams* out);

/* Flight color-RT pixel dimensions. Used by the cockpit overlay to size
 * its chrome-cache RT at the same full render resolution. */
void TieFlightRenderer_GetRtDims(const TieFlightRenderer* g, int* out_w, int* out_h);

#ifdef __cplusplus
}
#endif

#endif

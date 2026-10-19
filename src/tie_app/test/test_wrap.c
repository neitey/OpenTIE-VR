/* Link-time wraps for the headless test harness (test_harness.h).
 *
 * Built with TIE_TEST_WRAP (Linux desktop): the tie executable is linked with
 * -Wl,--wrap=<symbol> for the SDL GPU swapchain calls Aeron_Present makes and
 * for the Aeron OpenXR stereo API. While no TIE_TEST_* switch is on, every
 * wrapper forwards straight to the real function.
 *
 *  - Window capture: Aeron_Present composes into a texture of ours instead of
 *    the swapchain image; we download it, blit it on to the swapchain image
 *    and write a PNG.
 *  - Fake XR: Aeron_XrIsPresenting() = 1 and Aeron_XrStereoBegin() hands out
 *    eye / HUD / panel render targets of ours with scripted head poses and
 *    Quest-like asymmetric FOVs; the commits are recorded and written out as
 *    PNGs plus a text description of the layers. The engine's own XR state
 *    stays off, so Aeron_Present keeps composing the desktop window as usual
 *    (the "desktop mirror").
 */
#include "tie_app/test/test_harness.h"

#if defined(TIE_TEST_WRAP)

#include "aeron/log.h"
#include "aeron/render.h"
#include "aeron/xr.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int TieTestWrap_Available(void) { return 1; }

/* ======================================================================
 * Window capture
 * ====================================================================== */

static SDL_GPUDevice* s_device;
static SDL_Window* s_window;
static char s_capture_path[700];
static int s_capture_requested;
static SDL_GPUTexture* s_cap_tex;
static Uint32 s_cap_w, s_cap_h;
static SDL_GPUTextureFormat s_cap_fmt;
static SDL_GPUTransferBuffer* s_cap_tb;
static Uint32 s_cap_tb_size;
static SDL_GPUCommandBuffer* s_cap_cb;
static SDL_GPUTexture* s_cap_swap;

bool __real_SDL_ClaimWindowForGPUDevice(SDL_GPUDevice* device, SDL_Window* window);
bool __wrap_SDL_ClaimWindowForGPUDevice(SDL_GPUDevice* device, SDL_Window* window) {
	const bool ok = __real_SDL_ClaimWindowForGPUDevice(device, window);
	if (ok) {
		s_device = device;
		s_window = window;
	}
	return ok;
}

void TieTestWrap_RequestWindowCapture(const char* path) {
	SDL_strlcpy(s_capture_path, path, sizeof s_capture_path);
	s_capture_requested = 1;
}

static int TieTestWrap_EnsureCaptureTexture(Uint32 w, Uint32 h, SDL_GPUTextureFormat fmt) {
	if (s_cap_tex && s_cap_w == w && s_cap_h == h && s_cap_fmt == fmt)
		return 1;
	if (s_cap_tex)
		SDL_ReleaseGPUTexture(s_device, s_cap_tex);
	SDL_GPUTextureCreateInfo info;
	SDL_zero(info);
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = fmt;
	info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
	info.width = w;
	info.height = h;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	s_cap_tex = SDL_CreateGPUTexture(s_device, &info);
	s_cap_w = w;
	s_cap_h = h;
	s_cap_fmt = fmt;
	const Uint32 size = SDL_CalculateGPUTextureFormatSize(fmt, w, h, 1);
	if (s_cap_tb && s_cap_tb_size < size) {
		SDL_ReleaseGPUTransferBuffer(s_device, s_cap_tb);
		s_cap_tb = NULL;
	}
	if (!s_cap_tb) {
		SDL_GPUTransferBufferCreateInfo tb;
		SDL_zero(tb);
		tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		tb.size = size;
		s_cap_tb = SDL_CreateGPUTransferBuffer(s_device, &tb);
		s_cap_tb_size = size;
	}
	return s_cap_tex && s_cap_tb;
}

bool __real_SDL_WaitAndAcquireGPUSwapchainTexture(SDL_GPUCommandBuffer* cb, SDL_Window* window,
												  SDL_GPUTexture** texture, Uint32* w, Uint32* h);
bool __wrap_SDL_WaitAndAcquireGPUSwapchainTexture(SDL_GPUCommandBuffer* cb, SDL_Window* window,
												  SDL_GPUTexture** texture, Uint32* w, Uint32* h) {
	const bool ok = __real_SDL_WaitAndAcquireGPUSwapchainTexture(cb, window, texture, w, h);
	if (!ok || !s_capture_requested || !texture || !*texture || !s_device || !w || !h)
		return ok;
	s_capture_requested = 0;
	const SDL_GPUTextureFormat fmt = SDL_GetGPUSwapchainTextureFormat(s_device, window);
	if (!TieTestWrap_EnsureCaptureTexture(*w, *h, fmt)) {
		Aeron_LogWarn("tie.test", "window capture texture failed: %s", SDL_GetError());
		return ok;
	}
	s_cap_swap = *texture;
	s_cap_cb = cb;
	*texture = s_cap_tex;
	return ok;
}

static float TieTestWrap_HalfToFloat(uint16_t h) {
	const uint32_t sign = (uint32_t)(h >> 15) << 31;
	const int exp = (h >> 10) & 0x1f;
	const uint32_t mant = h & 0x3ffu;
	uint32_t bits;
	if (exp == 0) {
		const float f = (float)mant / 1024.0f / 16384.0f;
		return sign ? -f : f;
	}
	if (exp == 31)
		bits = sign | 0x7f800000u | (mant << 13);
	else
		bits = sign | ((uint32_t)(exp - 15 + 127) << 23) | (mant << 13);
	float f;
	memcpy(&f, &bits, sizeof f);
	return f;
}

static uint8_t TieTestWrap_LinearToSrgb8(float v) {
	v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
	v = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
	return (uint8_t)(v * 255.0f + 0.5f);
}

/* Converts w*h pixels of `fmt` into tightly packed RGBA8. */
static int TieTestWrap_ToRgba8(const uint8_t* src, SDL_GPUTextureFormat fmt, uint8_t* dst, Uint32 count,
							   int opaque) {
	for (Uint32 i = 0; i < count; ++i) {
		uint8_t* d = dst + i * 4u;
		switch (fmt) {
			case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
			case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB:
				d[0] = src[i * 4u + 2];
				d[1] = src[i * 4u + 1];
				d[2] = src[i * 4u + 0];
				d[3] = src[i * 4u + 3];
				break;
			case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
			case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:
				memcpy(d, src + i * 4u, 4);
				break;
			case SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM: {
				uint32_t p;
				memcpy(&p, src + i * 4u, 4);
				d[0] = (uint8_t)(((p >> 0) & 0x3ffu) >> 2);
				d[1] = (uint8_t)(((p >> 10) & 0x3ffu) >> 2);
				d[2] = (uint8_t)(((p >> 20) & 0x3ffu) >> 2);
				d[3] = (uint8_t)(((p >> 30) & 3u) * 85u);
				break;
			}
			case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT: {
				uint16_t c[4];
				memcpy(c, src + i * 8u, 8);
				for (int k = 0; k < 3; ++k)
					d[k] = TieTestWrap_LinearToSrgb8(TieTestWrap_HalfToFloat(c[k]));
				const float a = TieTestWrap_HalfToFloat(c[3]);
				d[3] = (uint8_t)((a < 0 ? 0 : a > 1 ? 1 : a) * 255.0f + 0.5f);
				break;
			}
			default:
				return 0;
		}
		if (opaque)
			d[3] = 255;
	}
	return 1;
}

static int TieTestWrap_SavePng(const char* path, const uint8_t* rgba, int w, int h) {
	SDL_Surface* surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, (void*)rgba, w * 4);
	if (!surface)
		return 0;
	const bool ok = SDL_SavePNG(surface, path);
	SDL_DestroySurface(surface);
	if (!ok)
		Aeron_LogWarn("tie.test", "could not write %s: %s", path, SDL_GetError());
	return ok ? 1 : 0;
}

bool __real_SDL_SubmitGPUCommandBuffer(SDL_GPUCommandBuffer* cb);
bool __wrap_SDL_SubmitGPUCommandBuffer(SDL_GPUCommandBuffer* cb) {
	if (!cb || cb != s_cap_cb)
		return __real_SDL_SubmitGPUCommandBuffer(cb);
	s_cap_cb = NULL;

	SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cb);
	if (copy) {
		SDL_GPUTextureRegion region;
		SDL_zero(region);
		region.texture = s_cap_tex;
		region.w = s_cap_w;
		region.h = s_cap_h;
		region.d = 1;
		SDL_GPUTextureTransferInfo transfer;
		SDL_zero(transfer);
		transfer.transfer_buffer = s_cap_tb;
		SDL_DownloadFromGPUTexture(copy, &region, &transfer);
		SDL_EndGPUCopyPass(copy);
	}
	/* Still show the frame in the window. */
	SDL_GPUBlitInfo blit;
	SDL_zero(blit);
	blit.source.texture = s_cap_tex;
	blit.source.w = s_cap_w;
	blit.source.h = s_cap_h;
	blit.destination.texture = s_cap_swap;
	blit.destination.w = s_cap_w;
	blit.destination.h = s_cap_h;
	blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
	blit.filter = SDL_GPU_FILTER_NEAREST;
	SDL_BlitGPUTexture(cb, &blit);

	SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
	if (!fence)
		return false;
	SDL_WaitForGPUFences(s_device, true, &fence, 1);
	SDL_ReleaseGPUFence(s_device, fence);
	if (copy) {
		const uint8_t* mapped = SDL_MapGPUTransferBuffer(s_device, s_cap_tb, false);
		if (mapped) {
			uint8_t* rgba = SDL_malloc((size_t)s_cap_w * s_cap_h * 4u);
			if (rgba && TieTestWrap_ToRgba8(mapped, s_cap_fmt, rgba, s_cap_w * s_cap_h, 1))
				TieTestWrap_SavePng(s_capture_path, rgba, (int)s_cap_w, (int)s_cap_h);
			else
				Aeron_LogWarn("tie.test", "window capture: unsupported swapchain format %d", (int)s_cap_fmt);
			SDL_free(rgba);
			SDL_UnmapGPUTransferBuffer(s_device, s_cap_tb);
		}
	}
	return true;
}

/* ======================================================================
 * Fake XR headset
 * ====================================================================== */

typedef struct TieTestXrCommit {
	int committed;     /* a Commit/CommitHuds/CommitPanels/Backdrop this frame */
	int backdrop;
	int has_hud, has_world;
	AeronXrHudQuad hud, world;
	int panel_count;
	AeronXrPanel panels[AERON_XR_MAX_PANELS];
	int console, card;
	AeronXrPanel console_quad, card_quad;
	int layer_count[AERON_XR_PANEL_COUNT];
	AeronXrPanel layer_quads[AERON_XR_PANEL_COUNT][2];
	int overlay;
	int overlay_rect[2][4];
	int beam_count;
	AeronXrPanel beams[2];
	int began;
	AeronXrView views[2];
} TieTestXrCommit;

static struct {
	int on;
	int eye_w, eye_h;
	float fov[4]; /* left eye: left, right, up, down (radians) */
	float ypr[3];
	float pos[3];
	AeronRenderTarget* eye[2];
	AeronRenderTarget* hud[3]; /* hud, world, aux */
	AeronRenderTarget* beam;
	AeronRenderTarget* panel[AERON_XR_PANEL_COUNT];
	int panel_w[AERON_XR_PANEL_COUNT], panel_h[AERON_XR_PANEL_COUNT];
	int panel_drawn[AERON_XR_PANEL_COUNT];
	TieTestXrCommit frame;
	uint64_t committed_frames;
	int warned;
	/* scripted controller input */
	int pointer, menu_keys, flight_buttons; /* last modes the game set (-1 unknown) */
	int button_queue[16];
	int button_count;
	int menu_latch[4]; /* A, B, X, Y */
	float throttle;
	float trigger[2], grip[2];
	int hand_pose_set[2];
	int capture_trigger[2], capture_grip[2];
	float hand_pose[2][5];
} s_xr = { .pointer = -1, .menu_keys = -1, .flight_buttons = -1 };

void TieTestXr_Enable(int eye_width, int eye_height) {
	s_xr.on = 1;
	s_xr.eye_w = eye_width >= 64 ? eye_width : 512;
	s_xr.eye_h = eye_height >= 64 ? eye_height : 512;
	/* Quest 3-like asymmetric left-eye FOV (the right eye mirrors it). */
	s_xr.fov[0] = -0.942f;
	s_xr.fov[1] = 0.698f;
	s_xr.fov[2] = 0.768f;
	s_xr.fov[3] = -0.890f;
	const char* fov = SDL_getenv("TIE_TEST_XR_FOV");
	if (fov)
		(void)SDL_sscanf(fov, "%f,%f,%f,%f", &s_xr.fov[0], &s_xr.fov[1], &s_xr.fov[2], &s_xr.fov[3]);
}

int TieTestXr_Enabled(void) { return s_xr.on; }

uint64_t TieTestXr_CommittedFrames(void) { return s_xr.committed_frames; }

void TieTestXr_SetHead(const float ypr[3], const float pos[3]) {
	memcpy(s_xr.ypr, ypr, sizeof s_xr.ypr);
	memcpy(s_xr.pos, pos, sizeof s_xr.pos);
}

static void TieTestXr_QuatMul(const float a[4], const float b[4], float out[4]) {
	const float r[4] = {
		a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
		a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
		a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
		a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
	};
	memcpy(out, r, sizeof r);
}

/* Head orientation: yaw about +Y (positive = look left), then pitch about +X
 * (positive = look up), then roll about -Z... i.e. q = qy * qx * qz. */
static void TieTestXr_HeadQuat(float q[4]) {
	const float d2r = 3.14159265f / 180.0f;
	const float y = s_xr.ypr[0] * d2r * 0.5f, p = s_xr.ypr[1] * d2r * 0.5f, r = s_xr.ypr[2] * d2r * 0.5f;
	const float qy[4] = { 0, sinf(y), 0, cosf(y) };
	const float qx[4] = { sinf(p), 0, 0, cosf(p) };
	const float qz[4] = { 0, 0, sinf(r), cosf(r) };
	float t[4];
	TieTestXr_QuatMul(qy, qx, t);
	TieTestXr_QuatMul(t, qz, q);
}

static void TieTestXr_Rotate(const float q[4], const float v[3], float out[3]) {
	const float u[3] = { q[0], q[1], q[2] };
	const float s = q[3];
	const float uv = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
	const float uu = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
	const float c[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
	for (int i = 0; i < 3; ++i)
		out[i] = 2.0f * uv * u[i] + (s * s - uu) * v[i] + 2.0f * s * c[i];
}

static AeronRenderTarget* TieTestXr_Target(int w, int h, const char* name) {
	AeronRenderTarget* rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width = w,
		.height = h,
		.format = AERON_TEXTURE_FORMAT_RGBA8_SRGB,
		.debug_name = name,
	});
	if (!rt)
		Aeron_LogError("tie.test", "fake XR: cannot create %s %dx%d", name, w, h);
	return rt;
}

int __real_Aeron_XrIsPresenting(void);
int __wrap_Aeron_XrIsPresenting(void) { return s_xr.on ? 1 : __real_Aeron_XrIsPresenting(); }

int __real_Aeron_XrIsUnfocused(void);
int __wrap_Aeron_XrIsUnfocused(void) { return s_xr.on ? 0 : __real_Aeron_XrIsUnfocused(); }

int __real_Aeron_XrStereoBegin(AeronXrStereoFrame* out);
int __wrap_Aeron_XrStereoBegin(AeronXrStereoFrame* out) {
	if (!s_xr.on)
		return __real_Aeron_XrStereoBegin(out);
	if (!out)
		return 0;
	if (!s_xr.eye[0]) {
		s_xr.eye[0] = TieTestXr_Target(s_xr.eye_w, s_xr.eye_h, "test.xr.eye_left");
		s_xr.eye[1] = TieTestXr_Target(s_xr.eye_w, s_xr.eye_h, "test.xr.eye_right");
		s_xr.hud[0] = TieTestXr_Target(s_xr.eye_w, s_xr.eye_h, "test.xr.hud");
		s_xr.hud[1] = TieTestXr_Target(s_xr.eye_w, s_xr.eye_h, "test.xr.hud_world");
		s_xr.hud[2] = TieTestXr_Target(s_xr.eye_w, s_xr.eye_h, "test.xr.hud_aux");
		s_xr.beam = TieTestXr_Target(128, 512, "test.xr.beam");
	}
	if (!s_xr.eye[0] || !s_xr.eye[1] || !s_xr.hud[0] || !s_xr.hud[1])
		return 0;
	memset(out, 0, sizeof *out);
	float head[4];
	TieTestXr_HeadQuat(head);
	for (int e = 0; e < 2; ++e) {
		const float offset[3] = { e ? 0.032f : -0.032f, 0.0f, 0.0f };
		float world_offset[3];
		TieTestXr_Rotate(head, offset, world_offset);
		memcpy(out->views[e].orientation, head, sizeof head);
		for (int i = 0; i < 3; ++i)
			out->views[e].position[i] = s_xr.pos[i] + world_offset[i];
		out->views[e].fov[0] = e ? -s_xr.fov[1] : s_xr.fov[0];
		out->views[e].fov[1] = e ? -s_xr.fov[0] : s_xr.fov[1];
		out->views[e].fov[2] = s_xr.fov[2];
		out->views[e].fov[3] = s_xr.fov[3];
		out->eye_targets[e] = s_xr.eye[e];
	}
	out->width = s_xr.eye_w;
	out->height = s_xr.eye_h;
	out->format = AERON_TEXTURE_FORMAT_RGBA8_SRGB;
	out->hud_target = s_xr.hud[0];
	out->hud_world_target = s_xr.hud[1];
	out->hud_aux_target = s_xr.hud[2];
	out->hud_width = s_xr.eye_w;
	out->hud_height = s_xr.eye_h;
	out->console_width = 1536;
	out->console_height = 512;
	out->card_width = 1024;
	out->card_height = 768;
	out->beam_target = s_xr.beam;
	out->beam_width = 128;
	out->beam_height = 512;
	s_xr.frame.began = 1;
	memcpy(s_xr.frame.views, out->views, sizeof s_xr.frame.views);
	return 1;
}

AeronRenderTarget* __real_Aeron_XrStereoPanelTarget(int which, int* width, int* height);
AeronRenderTarget* __wrap_Aeron_XrStereoPanelTarget(int which, int* width, int* height) {
	if (!s_xr.on)
		return __real_Aeron_XrStereoPanelTarget(which, width, height);
	if (which < 0 || which >= AERON_XR_PANEL_COUNT)
		return NULL;
	if (!s_xr.panel[which]) {
		const int w = which == AERON_XR_PANEL_CONSOLE ? 1536
					  : which == AERON_XR_PANEL_FRAME ? AERON_XR_FRAME_WIDTH
					  : which == AERON_XR_PANEL_LABELS ? 2048
													   : 1024;
		const int h = which == AERON_XR_PANEL_CONSOLE ? 512 : which == AERON_XR_PANEL_FRAME ? AERON_XR_FRAME_HEIGHT : 768;
		static const char* const names[AERON_XR_PANEL_COUNT] = { "test.xr.console", "test.xr.card",
																 "test.xr.card2", "test.xr.frame",
																 "test.xr.labels" };
		s_xr.panel[which] = TieTestXr_Target(w, h, names[which]);
		s_xr.panel_w[which] = w;
		s_xr.panel_h[which] = h;
	}
	s_xr.panel_drawn[which] = 1;
	if (width)
		*width = s_xr.panel_w[which];
	if (height)
		*height = s_xr.panel_h[which];
	return s_xr.panel[which];
}

static void TieTestXr_MarkCommitted(void) {
	if (!s_xr.frame.committed)
		++s_xr.committed_frames;
	s_xr.frame.committed = 1;
}

void __real_Aeron_XrStereoCommit(const AeronXrHudQuad* hud);
void __wrap_Aeron_XrStereoCommit(const AeronXrHudQuad* hud) {
	if (!s_xr.on) {
		__real_Aeron_XrStereoCommit(hud);
		return;
	}
	TieTestXr_MarkCommitted();
	if (hud) {
		s_xr.frame.has_hud = 1;
		s_xr.frame.hud = *hud;
	}
}

void __real_Aeron_XrStereoCommitHuds(const AeronXrHudQuad* hud, const AeronXrHudQuad* hud_world);
void __wrap_Aeron_XrStereoCommitHuds(const AeronXrHudQuad* hud, const AeronXrHudQuad* hud_world) {
	if (!s_xr.on) {
		__real_Aeron_XrStereoCommitHuds(hud, hud_world);
		return;
	}
	TieTestXr_MarkCommitted();
	if (hud) {
		s_xr.frame.has_hud = 1;
		s_xr.frame.hud = *hud;
	}
	if (hud_world) {
		s_xr.frame.has_world = 1;
		s_xr.frame.world = *hud_world;
	}
}

void __real_Aeron_XrStereoCommitPanels(const AeronXrHudQuad* hud_world, const AeronXrPanel* panels, int count);
void __wrap_Aeron_XrStereoCommitPanels(const AeronXrHudQuad* hud_world, const AeronXrPanel* panels, int count) {
	if (!s_xr.on) {
		__real_Aeron_XrStereoCommitPanels(hud_world, panels, count);
		return;
	}
	TieTestXr_MarkCommitted();
	if (hud_world) {
		s_xr.frame.has_world = 1;
		s_xr.frame.world = *hud_world;
	}
	s_xr.frame.panel_count = 0;
	for (int i = 0; panels && i < count && i < AERON_XR_MAX_PANELS; ++i)
		s_xr.frame.panels[s_xr.frame.panel_count++] = panels[i];
}

void __real_Aeron_XrStereoCommitConsole(const AeronXrPanel* console);
void __wrap_Aeron_XrStereoCommitConsole(const AeronXrPanel* console) {
	if (!s_xr.on) {
		__real_Aeron_XrStereoCommitConsole(console);
		return;
	}
	if (console) {
		s_xr.frame.console = 1;
		s_xr.frame.console_quad = *console;
	}
}

void __real_Aeron_XrStereoCommitCard(const AeronXrPanel* card);
void __wrap_Aeron_XrStereoCommitCard(const AeronXrPanel* card) {
	if (!s_xr.on) {
		__real_Aeron_XrStereoCommitCard(card);
		return;
	}
	if (card) {
		s_xr.frame.card = 1;
		s_xr.frame.card_quad = *card;
	}
}

void __real_Aeron_XrStereoCommitPanelLayer(int which, const AeronXrPanel* quads, int count);
void __wrap_Aeron_XrStereoCommitPanelLayer(int which, const AeronXrPanel* quads, int count) {
	if (!s_xr.on) {
		__real_Aeron_XrStereoCommitPanelLayer(which, quads, count);
		return;
	}
	if (which < 0 || which >= AERON_XR_PANEL_COUNT || !quads)
		return;
	s_xr.frame.layer_count[which] = count < 2 ? count : 2;
	for (int i = 0; i < s_xr.frame.layer_count[which]; ++i)
		s_xr.frame.layer_quads[which][i] = quads[i];
}

void __real_Aeron_XrStereoCommitEyeOverlay(void);
void __wrap_Aeron_XrStereoCommitEyeOverlay(void) {
	if (!s_xr.on) {
		__real_Aeron_XrStereoCommitEyeOverlay();
		return;
	}
	s_xr.frame.overlay = 1;
	memset(s_xr.frame.overlay_rect, 0, sizeof s_xr.frame.overlay_rect);
}

void __real_Aeron_XrStereoCommitEyeOverlayRect(const int rect[2][4]);
void __wrap_Aeron_XrStereoCommitEyeOverlayRect(const int rect[2][4]) {
	if (!s_xr.on) {
		__real_Aeron_XrStereoCommitEyeOverlayRect(rect);
		return;
	}
	s_xr.frame.overlay = 1;
	if (rect)
		memcpy(s_xr.frame.overlay_rect, rect, sizeof s_xr.frame.overlay_rect);
}

void __real_Aeron_XrStereoCommitBeams(const AeronXrPanel* beams, int count);
void __wrap_Aeron_XrStereoCommitBeams(const AeronXrPanel* beams, int count) {
	if (!s_xr.on) {
		__real_Aeron_XrStereoCommitBeams(beams, count);
		return;
	}
	s_xr.frame.beam_count = count < 2 ? count : 2;
	for (int i = 0; beams && i < s_xr.frame.beam_count; ++i)
		s_xr.frame.beams[i] = beams[i];
}

void __real_Aeron_XrStereoCommitBackdrop(void);
void __wrap_Aeron_XrStereoCommitBackdrop(void) {
	if (!s_xr.on) {
		__real_Aeron_XrStereoCommitBackdrop();
		return;
	}
	TieTestXr_MarkCommitted();
	s_xr.frame.backdrop = 1;
}

int __real_Aeron_XrVirtualScreenGeometry(float* distance_m, float* width_m, float* height_m,
										 float* height_offset_m);
int __wrap_Aeron_XrVirtualScreenGeometry(float* distance_m, float* width_m, float* height_m,
										 float* height_offset_m) {
	if (!s_xr.on)
		return __real_Aeron_XrVirtualScreenGeometry(distance_m, width_m, height_m, height_offset_m);
	/* Engine defaults: 2.1 m ahead, 2.1 m wide, 4:3 picture (1440x1080). */
	if (distance_m)
		*distance_m = 2.1f;
	if (width_m)
		*width_m = 2.1f;
	if (height_m)
		*height_m = 2.1f * 1080.0f / 1440.0f;
	if (height_offset_m)
		*height_offset_m = 0.0f;
	return 1;
}

int __real_Aeron_XrGetHand(int hand, AeronXrHand* out);
int __wrap_Aeron_XrGetHand(int hand, AeronXrHand* out) {
	if (!s_xr.on)
		return __real_Aeron_XrGetHand(hand, out);
	if (!out)
		return 0;
	memset(out, 0, sizeof *out);
	/* Resting hands in front of the seat: "x,y,z,yaw,pitch" overrides. */
	const int h = hand ? 1 : 0;
	out->trigger = s_xr.trigger[h];
	out->grip = s_xr.grip[h];
	float p[5] = { hand ? 0.15f : -0.15f, -0.35f, -0.35f, hand ? 10.0f : -10.0f, 30.0f };
	const char* env = SDL_getenv(hand ? "TIE_TEST_XR_RHAND" : "TIE_TEST_XR_LHAND");
	if (env)
		(void)SDL_sscanf(env, "%f,%f,%f,%f,%f", &p[0], &p[1], &p[2], &p[3], &p[4]);
	if (s_xr.hand_pose_set[h])
		memcpy(p, s_xr.hand_pose[h], sizeof p);
	const float d2r = 3.14159265f / 360.0f;
	const float qy[4] = { 0, sinf(p[3] * d2r), 0, cosf(p[3] * d2r) };
	const float qx[4] = { sinf(p[4] * d2r), 0, 0, cosf(p[4] * d2r) };
	float q[4];
	TieTestXr_QuatMul(qy, qx, q);
	out->valid = 1;
	out->position[0] = p[0];
	out->position[1] = p[1];
	out->position[2] = p[2];
	memcpy(out->orientation, q, sizeof q);
	out->grip_valid = 1;
	memcpy(out->grip_position, out->position, sizeof out->grip_position);
	memcpy(out->grip_orientation, q, sizeof q);
	out->index_touch = -1;
	out->thumb_touch = -1;
	return 1;
}

/* ---- scripted controller input (TieTestXr_* below) ---- */

static void TieTestXr_LogMode(const char* name, int* last, int value) {
	if (*last != (value != 0)) {
		*last = value != 0;
		printf("[tie-test] frame %llu: xr %s %s\n", (unsigned long long)TieTest_Frame(), name, value ? "on" : "off");
		fflush(stdout);
	}
}

void __real_Aeron_XrSetPointerEnabled(int enabled);
void __wrap_Aeron_XrSetPointerEnabled(int enabled) {
	if (s_xr.on)
		TieTestXr_LogMode("pointer", &s_xr.pointer, enabled);
	__real_Aeron_XrSetPointerEnabled(enabled);
}

void __real_Aeron_XrSetMenuKeys(int enabled);
void __wrap_Aeron_XrSetMenuKeys(int enabled) {
	if (s_xr.on)
		TieTestXr_LogMode("menu_keys", &s_xr.menu_keys, enabled);
	__real_Aeron_XrSetMenuKeys(enabled);
}

void __real_Aeron_XrSetFlightButtons(int enabled);
void __wrap_Aeron_XrSetFlightButtons(int enabled) {
	if (s_xr.on)
		TieTestXr_LogMode("flight_buttons", &s_xr.flight_buttons, enabled);
	__real_Aeron_XrSetFlightButtons(enabled);
}

int __real_Aeron_XrPollButton(AeronXrButton* button, int* shifted);
int __wrap_Aeron_XrPollButton(AeronXrButton* button, int* shifted) {
	if (!s_xr.on)
		return __real_Aeron_XrPollButton(button, shifted);
	if (s_xr.button_count <= 0)
		return 0;
	if (button)
		*button = (AeronXrButton)s_xr.button_queue[0];
	if (shifted)
		*shifted = 0;
	memmove(s_xr.button_queue, s_xr.button_queue + 1, sizeof(int) * (size_t)(--s_xr.button_count));
	return 1;
}

static int TieTestXr_TakeLatch(int index) {
	const int pressed = s_xr.menu_latch[index];
	s_xr.menu_latch[index] = 0;
	return pressed;
}

int __real_Aeron_XrTakeAPressed(void);
int __wrap_Aeron_XrTakeAPressed(void) { return s_xr.on ? TieTestXr_TakeLatch(0) : __real_Aeron_XrTakeAPressed(); }
int __real_Aeron_XrTakeMenuBPressed(void);
int __wrap_Aeron_XrTakeMenuBPressed(void) {
	return s_xr.on ? TieTestXr_TakeLatch(1) : __real_Aeron_XrTakeMenuBPressed();
}
int __real_Aeron_XrTakeMenuXPressed(void);
int __wrap_Aeron_XrTakeMenuXPressed(void) {
	return s_xr.on ? TieTestXr_TakeLatch(2) : __real_Aeron_XrTakeMenuXPressed();
}
int __real_Aeron_XrTakeMenuYPressed(void);
int __wrap_Aeron_XrTakeMenuYPressed(void) {
	return s_xr.on ? TieTestXr_TakeLatch(3) : __real_Aeron_XrTakeMenuYPressed();
}

float __real_Aeron_XrThrottleLever(void);
float __wrap_Aeron_XrThrottleLever(void) { return s_xr.on ? s_xr.throttle : __real_Aeron_XrThrottleLever(); }
void __real_Aeron_XrSetThrottleLever(float value);
void __wrap_Aeron_XrSetThrottleLever(float value) {
	if (!s_xr.on) {
		__real_Aeron_XrSetThrottleLever(value);
		return;
	}
	s_xr.throttle = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

void __real_Aeron_XrHapticPulse(int hand, float amplitude, int duration_ms);
void __wrap_Aeron_XrHapticPulse(int hand, float amplitude, int duration_ms) {
	if (s_xr.on) {
		printf("[tie-test] frame %llu: xr haptic hand=%d amplitude=%.2f ms=%d\n",
			   (unsigned long long)TieTest_Frame(), hand, (double)amplitude, duration_ms);
		fflush(stdout);
	}
	__real_Aeron_XrHapticPulse(hand, amplitude, duration_ms);
}

void TieTestXr_QueueButton(int button) {
	if (button < 0 || button >= AERON_XR_BUTTON_COUNT)
		return;
	if (s_xr.button_count < (int)SDL_arraysize(s_xr.button_queue))
		s_xr.button_queue[s_xr.button_count++] = button;
}

void TieTestXr_MenuPress(int index) {
	if (index >= 0 && index < 4)
		s_xr.menu_latch[index] = 1;
}

void TieTestXr_SetThrottle(float value) { __wrap_Aeron_XrSetThrottleLever(value); }

void TieTestXr_SetHandInput(int hand, float trigger, float grip) {
	const int h = hand ? 1 : 0;
	s_xr.trigger[h] = trigger;
	s_xr.grip[h] = grip;
}

/* Panel captures of a hand's trigger / grip: logged when they change. */
static void TieTestXr_LogCapture(const char* what, int* last, int hand, int captured) {
	if (hand < 0 || hand > 1 || last[hand] == (captured != 0))
		return;
	last[hand] = captured != 0;
	printf("[tie-test] frame %llu: xr capture %s hand=%d %s\n", (unsigned long long)TieTest_Frame(), what, hand,
		   captured ? "on" : "off");
	fflush(stdout);
}

void __real_Aeron_XrCaptureTrigger(int hand, int captured);
void __wrap_Aeron_XrCaptureTrigger(int hand, int captured) {
	if (s_xr.on)
		TieTestXr_LogCapture("trigger", s_xr.capture_trigger, hand, captured);
	__real_Aeron_XrCaptureTrigger(hand, captured);
}

void __real_Aeron_XrCaptureGrip(int hand, int captured);
void __wrap_Aeron_XrCaptureGrip(int hand, int captured) {
	if (s_xr.on)
		TieTestXr_LogCapture("grip", s_xr.capture_grip, hand, captured);
	__real_Aeron_XrCaptureGrip(hand, captured);
}

void TieTestXr_SetHandPose(int hand, const float pose[5]) {
	const int h = hand ? 1 : 0;
	memcpy(s_xr.hand_pose[h], pose, sizeof s_xr.hand_pose[h]);
	s_xr.hand_pose_set[h] = 1;
}

int __real_Aeron_XrGetRenderModel(int hand, AeronXrRenderModel* out);
int __wrap_Aeron_XrGetRenderModel(int hand, AeronXrRenderModel* out) {
	return s_xr.on ? 0 : __real_Aeron_XrGetRenderModel(hand, out);
}

/* ---- captures ---- */

static int TieTestXr_SaveTarget(AeronRenderTarget* rt, int w, int h, const char* prefix, const char* name,
								int opaque) {
	if (!rt || w <= 0 || h <= 0)
		return 0;
	const size_t size = (size_t)w * (size_t)h * 4u;
	uint8_t* px = SDL_malloc(size);
	if (!px)
		return 0;
	int ok = Aeron_ReadRenderTargetRawPixels(rt, px, size, (size_t)w * 4u);
	if (ok) {
		if (opaque)
			for (size_t i = 3; i < size; i += 4)
				px[i] = 255;
		char path[800];
		SDL_snprintf(path, sizeof path, "%s_%s.png", prefix, name);
		ok = TieTestWrap_SavePng(path, px, w, h);
	} else if (!s_xr.warned) {
		s_xr.warned = 1;
		Aeron_LogWarn("tie.test", "fake XR: readback of %s failed: %s", name, Aeron_RenderLastError());
	}
	SDL_free(px);
	return ok;
}

static void TieTestXr_WriteQuad(FILE* f, const char* name, const AeronXrHudQuad* q) {
	fprintf(f, "%s visible=%d pos=%.3f,%.3f,%.3f size=%.3f,%.3f head_locked=%d follow_s=%.3f\n", name, q->visible,
			(double)q->position[0], (double)q->position[1], (double)q->position[2], (double)q->size[0],
			(double)q->size[1], q->head_locked, (double)q->follow_s);
}

static void TieTestXr_WritePanel(FILE* f, const char* name, const AeronXrPanel* p) {
	fprintf(f,
			"%s rect=%d,%d,%d,%d pos=%.3f,%.3f,%.3f ori=%.4f,%.4f,%.4f,%.4f size=%.3f,%.3f source=%d eye=%d\n",
			name, p->rect[0], p->rect[1], p->rect[2], p->rect[3], (double)p->position[0], (double)p->position[1],
			(double)p->position[2], (double)p->orientation[0], (double)p->orientation[1],
			(double)p->orientation[2], (double)p->orientation[3], (double)p->size[0], (double)p->size[1],
			p->source, p->eye);
}

int TieTestXr_EndFrame(const char* prefix) {
	int written = 0;
	const TieTestXrCommit* c = &s_xr.frame;
	if (prefix && (c->began || c->committed)) {
		static const char* const panel_names[AERON_XR_PANEL_COUNT] = { "console", "card", "card2", "frame",
																	   "labels" };
		char path[800];
		SDL_snprintf(path, sizeof path, "%s_xr.txt", prefix);
		FILE* f = fopen(path, "w");
		if (f) {
			fprintf(f, "eye_size %d %d\n", s_xr.eye_w, s_xr.eye_h);
			for (int e = 0; e < 2; ++e) {
				const AeronXrView* v = &c->views[e];
				fprintf(f, "view%d pos=%.4f,%.4f,%.4f ori=%.5f,%.5f,%.5f,%.5f fov=%.4f,%.4f,%.4f,%.4f\n", e,
						(double)v->position[0], (double)v->position[1], (double)v->position[2],
						(double)v->orientation[0], (double)v->orientation[1], (double)v->orientation[2],
						(double)v->orientation[3], (double)v->fov[0], (double)v->fov[1], (double)v->fov[2],
						(double)v->fov[3]);
			}
			fprintf(f, "began=%d committed=%d backdrop=%d\n", c->began, c->committed, c->backdrop);
			if (c->has_world)
				TieTestXr_WriteQuad(f, "hud_world", &c->world);
			if (c->has_hud)
				TieTestXr_WriteQuad(f, "hud", &c->hud);
			for (int i = 0; i < c->panel_count; ++i)
				TieTestXr_WritePanel(f, "hud_panel", &c->panels[i]);
			if (c->console)
				TieTestXr_WritePanel(f, "console", &c->console_quad);
			if (c->card)
				TieTestXr_WritePanel(f, "card", &c->card_quad);
			for (int w = 0; w < AERON_XR_PANEL_COUNT; ++w)
				for (int i = 0; i < c->layer_count[w]; ++i)
					TieTestXr_WritePanel(f, panel_names[w], &c->layer_quads[w][i]);
			if (c->overlay)
				fprintf(f, "eye_overlay rect0=%d,%d,%d,%d rect1=%d,%d,%d,%d\n", c->overlay_rect[0][0],
						c->overlay_rect[0][1], c->overlay_rect[0][2], c->overlay_rect[0][3], c->overlay_rect[1][0],
						c->overlay_rect[1][1], c->overlay_rect[1][2], c->overlay_rect[1][3]);
			for (int i = 0; i < c->beam_count; ++i)
				TieTestXr_WritePanel(f, "beam", &c->beams[i]);
			fclose(f);
		}
		if (c->committed) {
			written += TieTestXr_SaveTarget(s_xr.eye[0], s_xr.eye_w, s_xr.eye_h, prefix, "eye_left", !c->overlay);
			written += TieTestXr_SaveTarget(s_xr.eye[1], s_xr.eye_w, s_xr.eye_h, prefix, "eye_right", !c->overlay);
			int hud_used = c->has_hud && c->hud.visible;
			int aux_used = 0;
			for (int i = 0; i < c->panel_count; ++i) {
				hud_used |= c->panels[i].source == 0;
				aux_used |= c->panels[i].source == 1;
			}
			if (hud_used)
				written += TieTestXr_SaveTarget(s_xr.hud[0], s_xr.eye_w, s_xr.eye_h, prefix, "hud", 0);
			if (c->has_world && c->world.visible)
				written += TieTestXr_SaveTarget(s_xr.hud[1], s_xr.eye_w, s_xr.eye_h, prefix, "hud_world", 0);
			if (aux_used)
				written += TieTestXr_SaveTarget(s_xr.hud[2], s_xr.eye_w, s_xr.eye_h, prefix, "hud_aux", 0);
			for (int w = 0; w < AERON_XR_PANEL_COUNT; ++w)
				if (c->layer_count[w] || (w == AERON_XR_PANEL_CONSOLE && c->console) ||
					(w == AERON_XR_PANEL_CARD && c->card))
					written += TieTestXr_SaveTarget(s_xr.panel[w], s_xr.panel_w[w], s_xr.panel_h[w], prefix,
													panel_names[w], 0);
		}
	}
	memset(&s_xr.frame, 0, sizeof s_xr.frame);
	memset(s_xr.panel_drawn, 0, sizeof s_xr.panel_drawn);
	return written;
}

#else /* !TIE_TEST_WRAP */

int TieTestWrap_Available(void) { return 0; }
void TieTestWrap_RequestWindowCapture(const char* path) { (void)path; }
void TieTestXr_Enable(int eye_width, int eye_height) {
	(void)eye_width;
	(void)eye_height;
}
int TieTestXr_Enabled(void) { return 0; }
void TieTestXr_SetHead(const float yaw_pitch_roll_deg[3], const float position_m[3]) {
	(void)yaw_pitch_roll_deg;
	(void)position_m;
}
int TieTestXr_EndFrame(const char* prefix) {
	(void)prefix;
	return 0;
}
uint64_t TieTestXr_CommittedFrames(void) { return 0; }
void TieTestXr_QueueButton(int button) { (void)button; }
void TieTestXr_MenuPress(int index) { (void)index; }
void TieTestXr_SetThrottle(float value) { (void)value; }
void TieTestXr_SetHandInput(int hand, float trigger, float grip) {
	(void)hand;
	(void)trigger;
	(void)grip;
}
void TieTestXr_SetHandPose(int hand, const float pose[5]) {
	(void)hand;
	(void)pose;
}

#endif

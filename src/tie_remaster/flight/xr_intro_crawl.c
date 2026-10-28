/* The opening title crawl in 3D; see xr_intro_crawl.h.
 *
 * Ported from OpenXWA-VR (src/xwa_remaster/vr_intro_crawl.c): the textures
 * (coverage mip chains coloured through the inverted tonemap), the text's
 * rasterization (stb_truetype, the film's lean) and the crawl's plane (its
 * perspective measured from XWA's intro film) are XWA's. What differs: the
 * clock is TIE Fighter's title scene's own (engine frames of 80 ms, the
 * snapshot's scene clock), the text is the game's own (TITLE.LFD, centred
 * lines, a line every 28 frames, its fade from frame 620), the logo recedes
 * as the game scales its own, the game's "along" picture opens it, and there
 * is no Hoth shot after it: the game's intro film follows on the screen. */

#include "tie_remaster/flight/xr_intro_crawl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "aeron/scene/billboard.h"
#include "aeron/scene/present.h"
#include "aeron/scene/scene3d.h"
#include "aeron/vfs.h"
#include "aeron/xr.h"
#include "tie_runtime/flight_assets/source.h"
#include "tie_runtime/input/input.h"
#include "tie_runtime/snapshot/snapshot_types.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ---- the game's title scene (src/tie/title.c), TIE95 frame numbers ------
 * Frames of 80 ms. "along" shows from the start to frame 38; the stars and
 * the logo cut in at 39, the logo shrinking (its scale 460 less a step that
 * itself shrinks every frame) until 100, its colours fading to black from 84;
 * line i of the text comes up from below the picture at start + 28 i (start
 * 100); everything fades from 620; the scene ends at 690 and the intro film
 * (scene 10) follows. */
#define TC_ALONG_END 38.0f
#define TC_ALONG_FADE 4.0f
#define TC_LOGO_FROM 39
#define TC_LOGO_FADE_FROM 84.0f
#define TC_LOGO_END 100
#define TC_LINE_FRAMES 28.0f
#define TC_TEXT_START 100.0f
#define TC_TEXT_FADE_IN 15.0f
#define TC_FADE_FROM 620.0f
#define TC_FADE_TO 689.0f
/* The game's logo picture is 256 x 135 classic pixels at scale 256, its
 * outline filling nearly all of it. */
#define TC_LOGO_OUTLINE 0.97f
#define TC_CLASSIC_W 320.0f
#define TC_CLASSIC_H 200.0f

/* ---- OpenXWA-VR's crawl plane (vr_intro_crawl.c, measured from XWA's
 * film; a 640 x 400 picture, x right, y down). A text point (xn, s) is at
 * picture y = YV + K / (tau + T0 - s), x = CX + xn (y - YV): s its place along
 * the scroll (the first line's top at 0, line i's baseline at i PITCH +
 * BASE), tau the scroll's clock. Here tau runs a line PITCH every 28 of the
 * game's frames. */
#define CRAWL_FILM_CY 200.0f
#define CRAWL_FILM_W 640.0f
#define CRAWL_YV -154.643f
#define CRAWL_K 13535.74f
#define CRAWL_T0 14.1932f
#define CRAWL_PITCH 1.36445f
#define CRAWL_BASE 0.9502f
#define CRAWL_EM_Y 1.3343f
#define CRAWL_EM_X 0.0748f
/* The text's half width at most (xn), the lines centred on 0. */
#define CRAWL_HALF_W 0.62f
#define CRAWL_SHEAR (-0.174f)
#define CRAWL_TEXT_ENTRY_DEG 35.0f
#define CRAWL_TEX_X0 (-0.66f)
#define CRAWL_TEX_S0 (-0.45f)
#define CRAWL_TEX_ANISO 330.0f

/* TIE Fighter's crawl is yellow, its logo a deeper yellow. */
static const uint8_t k_text_srgb[3] = { 235, 222, 64 };
static const uint8_t k_logo_srgb[3] = { 240, 206, 48 };

#define CRAWL_MAX_LINES TIE_MAX_TITLE_CRAWL_LINES
#define CRAWL_MAX_GLYPHS 96

typedef struct CrawlTex {
	AeronTexture* tex;
	int w, h;
} CrawlTex;

static struct {
	/* What the game shows (TieXrIntroCrawl_SetSnapshot). */
	bool title;
	float frame;
	int line_count;
	char lines[CRAWL_MAX_LINES][TIE_TITLE_CRAWL_MAX_CHARS];
	float initial_y[CRAWL_MAX_LINES];
	/* The scene. */
	bool started;    /* the title is up and the assets were tried */
	bool ok;         /* the logo and the font loaded */
	bool text_tried; /* the text was rasterized (or failed) */
	bool text_ok;
	bool active;
	int release_in; /* frames until the textures are freed */
	CrawlTex logo, text, along;
	float logo_box[4];     /* the logo's outline in its image, uv: u0 v0 u1 v1 */
	float along_rect[4];   /* "along" in classic pixels: x0 y0 x1 y1 */
	float along_gain;      /* its texels' HDR scale */
	float text_s1;         /* s at the text texture's bottom edge */
	float logo_tint[3], text_tint[3];
	float logo_scale[TC_LOGO_END - TC_LOGO_FROM + 2]; /* the game's logo scale, per frame */
	unsigned char* ttf;
	size_t ttf_size;
	/* The skip buttons held last frame (-1: not looked at yet). */
	int skip_held;
	bool skipped;
} s_crawl;

static float crawl_env(const char* name, float fallback) {
	const char* v = getenv(name);
	return v && v[0] ? (float)atof(v) : fallback;
}

static bool crawl_enabled(void) { return crawl_env("XWA_XR_INTRO_CRAWL", 1.0f) != 0.0f; }

static float crawl_clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

static float crawl_srgb_to_linear(float c) {
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float crawl_linear_to_srgb(float c) {
	return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

/* An APK asset (or a file beside the game): vr/<name>. */
static void* crawl_load(const char* name, size_t* size) {
	char path[2][96];
	snprintf(path[0], sizeof path[0], "vr/%s", name);
	snprintf(path[1], sizeof path[1], "assets/vr/%s", name);
	for (int i = 0; i < 2; ++i) {
		void* data = SDL_LoadFile(path[i], size);
		if (data)
			return data;
	}
	Aeron_LogInfo("tie.vr", "intro crawl: no vr/%s", name);
	return NULL;
}

/* ---- the colours through the tonemap -------------------------------------
 * The scene is tonemapped (AeronScenePresent's operator) before the eye
 * gain: to show the game's colours the billboards are tinted with the HDR
 * colour the operator maps onto them, found by inverting it numerically. */

static const float k_agx_inset[3][3] = { { 0.842479062253094f, 0.0784335999999992f, 0.0792237451477643f },
										 { 0.0423282422610123f, 0.878468636469772f, 0.0784336f },
										 { 0.0423756549057051f, 0.0784336f, 0.879142973793104f } };
static const float k_agx_outset[3][3] = { { 1.19687900512017f, -0.0980208811401368f, -0.0990297440797205f },
										  { -0.0528968517574562f, 1.15190312990417f, -0.0989611768448433f },
										  { -0.0529716355144438f, -0.0980434501171241f, 1.15107367264116f } };
static const float k_aces_in[3][3] = { { 0.59719f, 0.35458f, 0.04823f },
									   { 0.07600f, 0.90834f, 0.01566f },
									   { 0.02840f, 0.13383f, 0.83777f } };
static const float k_aces_out[3][3] = { { 1.60475f, -0.53108f, -0.07367f },
										{ -0.10208f, 1.10813f, -0.00605f },
										{ -0.00327f, -0.07276f, 1.07602f } };

static void crawl_mul(const float m[3][3], const float v[3], float out[3]) {
	float r[3];
	for (int i = 0; i < 3; ++i)
		r[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
	memcpy(out, r, sizeof r);
}

/* scene_tonemap.frag.hlsl: linear HDR -> the displayed sRGB value (0..1),
 * exposure 1, no bloom. */
static void crawl_tonemap(const float in[3], float out[3]) {
	float c[3] = { fmaxf(in[0], 0.0f), fmaxf(in[1], 0.0f), fmaxf(in[2], 0.0f) };
	if (AeronScenePresent_TonemapOp() == AERON_SCENE_TONEMAP_ACES) {
		const float e = AeronScenePresent_AcesExposure();
		for (int k = 0; k < 3; ++k)
			c[k] *= e;
		crawl_mul(k_aces_in, c, c);
		for (int k = 0; k < 3; ++k) {
			const float a = c[k] * (c[k] + 0.0245786f) - 0.000090537f;
			const float b = c[k] * (0.983729f * c[k] + 0.4329510f) + 0.238081f;
			c[k] = a / b;
		}
		crawl_mul(k_aces_out, c, c);
	} else {
		static const float min_ev = -12.47393f, max_ev = 4.026069f;
		crawl_mul(k_agx_inset, c, c);
		for (int k = 0; k < 3; ++k) {
			const float x = crawl_clamp01((log2f(fmaxf(c[k], 1e-10f)) - min_ev) / (max_ev - min_ev));
			const float x2 = x * x, x4 = x2 * x2;
			c[k] = crawl_clamp01(15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 +
								 0.1191f * x - 0.00232f);
		}
		if (AeronScenePresent_AgxLook() == AERON_SCENE_AGX_LOOK_PUNCHY) {
			const float pw = AeronScenePresent_AgxPunchyPower(), sat = AeronScenePresent_AgxPunchySaturation();
			for (int k = 0; k < 3; ++k)
				c[k] = powf(fmaxf(c[k], 0.0f), pw);
			const float l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
			for (int k = 0; k < 3; ++k)
				c[k] = l + sat * (c[k] - l);
		}
		crawl_mul(k_agx_outset, c, c);
		const float eotf = AeronScenePresent_EotfExponent();
		for (int k = 0; k < 3; ++k)
			c[k] = powf(fmaxf(c[k], 0.0f), eotf);
	}
	for (int k = 0; k < 3; ++k)
		out[k] = crawl_linear_to_srgb(crawl_clamp01(c[k]));
}

static float crawl_det3(float m[3][3]) {
	return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
		   m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

/* The linear HDR colour the tonemap shows as `srgb` (0..1, display). */
static void crawl_untonemap(const float srgb[3], float out[3]) {
	float y[3];
	for (int k = 0; k < 3; ++k)
		y[k] = logf(crawl_srgb_to_linear(srgb[k]) + 1e-3f);
	for (int it = 0; it < 40; ++it) {
		float x[3], f0[3], r[3], jac[3][3];
		for (int k = 0; k < 3; ++k)
			x[k] = expf(y[k]);
		crawl_tonemap(x, f0);
		float err = 0.0f;
		for (int k = 0; k < 3; ++k) {
			r[k] = f0[k] - srgb[k];
			err += r[k] * r[k];
		}
		if (err < 1e-9f)
			break;
		for (int j = 0; j < 3; ++j) {
			float yj[3] = { y[0], y[1], y[2] }, xj[3], fj[3];
			yj[j] += 1e-3f;
			for (int k = 0; k < 3; ++k)
				xj[k] = expf(yj[k]);
			crawl_tonemap(xj, fj);
			for (int k = 0; k < 3; ++k)
				jac[k][j] = (fj[k] - f0[k]) / 1e-3f;
		}
		/* (J^T J + lambda) dy = -J^T r */
		float a[3][3], g[3];
		for (int i = 0; i < 3; ++i) {
			g[i] = 0.0f;
			for (int j = 0; j < 3; ++j) {
				a[i][j] = 0.0f;
				for (int k = 0; k < 3; ++k)
					a[i][j] += jac[k][i] * jac[k][j];
			}
			for (int k = 0; k < 3; ++k)
				g[i] -= jac[k][i] * r[k];
			a[i][i] += 1e-4f + 1e-2f * a[i][i];
		}
		const float det = crawl_det3(a);
		if (fabsf(det) < 1e-20f)
			break;
		for (int i = 0; i < 3; ++i) {
			float m[3][3];
			memcpy(m, a, sizeof m);
			for (int k = 0; k < 3; ++k)
				m[k][i] = g[k];
			const float dy = crawl_det3(m) / det;
			y[i] = fminf(fmaxf(y[i] + fminf(fmaxf(dy, -2.0f), 2.0f), -12.0f), 4.0f);
		}
	}
	for (int k = 0; k < 3; ++k)
		out[k] = expf(y[k]) < 1e-5f ? 0.0f : expf(y[k]);
}

/* The tint that shows `srgb8` faded to `fade` (display values scale). */
static void crawl_tint(const uint8_t srgb8[3], float fade, float out[3]) {
	const float d[3] = { (float)srgb8[0] / 255.0f * fade, (float)srgb8[1] / 255.0f * fade,
						 (float)srgb8[2] / 255.0f * fade };
	crawl_untonemap(d, out);
}

/* ---- textures ----------------------------------------------------------- */

static int crawl_levels(int w, int h) {
	int n = 1;
	while (w > 1 || h > 1) {
		w = w > 1 ? w / 2 : 1;
		h = h > 1 ? h / 2 : 1;
		++n;
	}
	return n;
}

/* Uploads `levels` premultiplied sRGB RGBA8 images (consecutive in `px`,
 * each half the last) as one mipmapped texture. */
static bool crawl_upload(AeronCommandBuffer* cmd, CrawlTex* out, const uint8_t* px, int w, int h, int levels,
						 const char* label) {
	AeronTexture* t = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width = w,
		.height = h,
		.mip_count = levels,
		.format = AERON_TEXTURE_FORMAT_RGBA8_SRGB,
		.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = label,
	});
	if (!t)
		return false;
	AeronTextureUploadDesc up[16];
	size_t offset = 0;
	int lw = w, lh = h;
	for (int l = 0; l < levels && l < 16; ++l) {
		const size_t size = (size_t)lw * (size_t)lh * 4u;
		memset(&up[l], 0, sizeof up[l]);
		up[l].texture = t;
		up[l].mip_level = l;
		up[l].width = lw;
		up[l].height = lh;
		up[l].raw_data = px + offset;
		up[l].raw_size = (uint32_t)size;
		offset += size;
		lw = lw > 1 ? lw / 2 : 1;
		lh = lh > 1 ? lh / 2 : 1;
	}
	if (!Aeron_UploadTextureBatchCmd(cmd, up, (uint32_t)(levels < 16 ? levels : 16))) {
		Aeron_DestroyTexture(t);
		return false;
	}
	out->tex = t;
	out->w = w;
	out->h = h;
	return true;
}

static size_t crawl_chain_size(int w, int h, int levels) {
	size_t n = 0;
	for (int l = 0; l < levels; ++l) {
		n += (size_t)w * (size_t)h * 4u;
		w = w > 1 ? w / 2 : 1;
		h = h > 1 ? h / 2 : 1;
	}
	return n;
}

/* Halves a float image (box filter; an odd edge row/column folds in). */
static void crawl_halve(const float* src, int w, int h, int ch, float* dst) {
	const int dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
	for (int y = 0; y < dh; ++y) {
		const int y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : y0;
		for (int x = 0; x < dw; ++x) {
			const int x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : x0;
			for (int c = 0; c < ch; ++c)
				dst[((size_t)y * dw + x) * ch + c] =
					0.25f * (src[((size_t)y0 * w + x0) * ch + c] + src[((size_t)y0 * w + x1) * ch + c] +
							 src[((size_t)y1 * w + x0) * ch + c] + src[((size_t)y1 * w + x1) * ch + c]);
		}
	}
}

/* The texel colour for each coverage: the HDR level whose tonemapped image
 * of colour `srgb8` is `coverage` times that colour in display values, so
 * anti-aliased edges (and mip levels) blend as on a flat picture, not
 * brightened by the tonemap's curve. lut[i] for coverage i / 1023; `tint`
 * gets the HDR colour at full coverage. */
static void crawl_coverage_lut(const uint8_t srgb8[3], float lut[1024], float tint[3]) {
	const float d[3] = { (float)srgb8[0] / 255.0f, (float)srgb8[1] / 255.0f, (float)srgb8[2] / 255.0f };
	crawl_untonemap(d, tint);
	float full[3];
	crawl_tonemap(tint, full);
	const float want = full[0] + full[1] + full[2];
	lut[0] = 0.0f;
	for (int i = 1; i < 1024; ++i) {
		const float target = want * (float)i / 1023.0f;
		float lo = 0.0f, hi = 1.0f;
		for (int it = 0; it < 22; ++it) {
			const float mid = 0.5f * (lo + hi);
			const float x[3] = { tint[0] * mid, tint[1] * mid, tint[2] * mid };
			float o[3];
			crawl_tonemap(x, o);
			if (o[0] + o[1] + o[2] < target)
				lo = mid;
			else
				hi = mid;
		}
		lut[i] = 0.5f * (lo + hi);
	}
}

/* Coverage (0..1) -> premultiplied mip chain: alpha the coverage, colour
 * lut(coverage) in white (the tint colours it). Takes `cov` (malloc'd) and
 * frees it. */
static bool crawl_texture_coverage(AeronCommandBuffer* cmd, CrawlTex* out, float* cov, int w, int h,
								   const float lut[1024], const char* label) {
	const int levels = crawl_levels(w, h);
	uint8_t* chain = (uint8_t*)malloc(crawl_chain_size(w, h, levels));
	float* a = cov;
	float* b = (float*)malloc((size_t)(w / 2 + 1) * (size_t)(h / 2 + 1) * sizeof(float));
	bool ok = false;
	if (chain && a && b) {
		uint8_t enc[1024];
		for (int i = 0; i < 1024; ++i)
			enc[i] = (uint8_t)(crawl_linear_to_srgb(crawl_clamp01(lut[i])) * 255.0f + 0.5f);
		size_t offset = 0;
		int lw = w, lh = h;
		for (int l = 0; l < levels; ++l) {
			for (size_t i = 0; i < (size_t)lw * lh; ++i) {
				const float c = crawl_clamp01(a[i]);
				uint8_t* p = chain + offset + i * 4;
				p[0] = p[1] = p[2] = enc[(int)(c * 1023.0f + 0.5f)];
				p[3] = (uint8_t)(c * 255.0f + 0.5f);
			}
			offset += (size_t)lw * lh * 4u;
			if (l + 1 < levels) {
				crawl_halve(a, lw, lh, 1, b);
				float* tmp = a;
				a = b;
				b = tmp;
				lw = lw > 1 ? lw / 2 : 1;
				lh = lh > 1 ? lh / 2 : 1;
			}
		}
		ok = crawl_upload(cmd, out, chain, w, h, levels, label);
	}
	free(chain);
	free(a);
	free(b);
	return ok;
}

/* vr/starwars_logo.rgba: XWAL (straight RGBA, the outline, the rest clear;
 * only its alpha is used). The outline's extent (coverage over 40 %) is what
 * the game's logo size describes. */
static bool crawl_load_logo(AeronCommandBuffer* cmd) {
	size_t size = 0;
	uint8_t* data = (uint8_t*)crawl_load("starwars_logo.rgba", &size);
	bool ok = false;
	if (data && size >= 12 && memcmp(data, "XWAL", 4) == 0) {
		uint32_t w, h;
		memcpy(&w, data + 4, 4);
		memcpy(&h, data + 8, 4);
		if (w && h && w <= 4096 && h <= 4096 && size >= 12 + (size_t)w * h * 4u) {
			const uint8_t* px = data + 12;
			int x0 = (int)w, y0 = (int)h, x1 = -1, y1 = -1;
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					if (px[((size_t)y * w + x) * 4 + 3] >= 102) {
						x0 = (int)x < x0 ? (int)x : x0;
						x1 = (int)x > x1 ? (int)x : x1;
						y0 = (int)y < y0 ? (int)y : y0;
						y1 = (int)y > y1 ? (int)y : y1;
					}
				}
			}
			if (x1 > x0 && y1 > y0) {
				s_crawl.logo_box[0] = (float)x0 / (float)w;
				s_crawl.logo_box[1] = (float)y0 / (float)h;
				s_crawl.logo_box[2] = (float)(x1 + 1) / (float)w;
				s_crawl.logo_box[3] = (float)(y1 + 1) / (float)h;
				float* cov = (float*)malloc((size_t)w * h * sizeof(float));
				if (cov) {
					for (size_t i = 0; i < (size_t)w * h; ++i)
						cov[i] = (float)px[i * 4 + 3] / 255.0f;
					float lut[1024];
					crawl_coverage_lut(k_logo_srgb, lut, s_crawl.logo_tint);
					ok = crawl_texture_coverage(cmd, &s_crawl.logo, cov, (int)w, (int)h, lut,
												"tie.vr_intro_crawl.logo");
				}
			}
		}
	}
	SDL_free(data);
	return ok;
}

/* The game's logo scale per frame (title_user_StarWars): 460 at its cut,
 * less a step of 12 every frame that itself drops by one every 256 / 48
 * frames. */
static void crawl_logo_table(void) {
	int scale = 460, step = 12, step_f = 0;
	for (int f = TC_LOGO_FROM; f <= TC_LOGO_END + 1; ++f) {
		if (f == TC_LOGO_FROM)
			scale = 460;
		scale -= step;
		if (scale < 1)
			scale = 1;
		if (f > TC_LOGO_FROM && f < TC_LOGO_END) {
			step_f += 48;
			if (step_f >= 256) {
				step_f -= 256;
				step--;
			}
		}
		s_crawl.logo_scale[f - TC_LOGO_FROM] = (float)scale;
	}
}

/* ---- the game's "along" picture (TITLE.LFD: DELT "along", PLTT "title") -- */

/* One LFD resource read whole from the game data of the first source that
 * has the archive. */
static uint8_t* crawl_read_lfd(const TieFlightAssetSource* assets, const char* path, const char* type,
							   const char* name, size_t* out_size) {
	for (const TieFlightAssetSource* src = assets; src; src = src->fallback) {
		AeronVfs* vfs = src->vfs ? src->vfs : Aeron_GetVfs();
		uint8_t* bytes = NULL;
		size_t size = 0;
		if (!vfs || !AeronVfs_Exists(vfs, AERON_VFS_ROOT_ASSET, path) ||
			!AeronVfs_ReadAll(vfs, AERON_VFS_ROOT_ASSET, path, 16u << 20, &bytes, &size) || size < 16)
			continue;
		uint32_t dir = 0;
		memcpy(&dir, bytes + 12, 4);
		size_t off = 16 + (size_t)dir;
		for (uint32_t i = 0; i < dir / 16 && 16 + (size_t)i * 16 + 16 <= size; ++i) {
			const uint8_t* e = bytes + 16 + (size_t)i * 16;
			uint32_t len = 0;
			memcpy(&len, e + 12, 4);
			char n[9] = { 0 };
			memcpy(n, e + 4, 8);
			if (memcmp(e, type, 4) == 0 && SDL_strcasecmp(n, name) == 0 && off + 16 + len <= size) {
				uint8_t* out = (uint8_t*)malloc(len ? len : 1);
				if (out)
					memcpy(out, bytes + off + 16, len);
				free(bytes);
				*out_size = len;
				return out;
			}
			off += 16 + (size_t)len;
		}
		free(bytes);
	}
	return NULL;
}

static int16_t crawl_i16(const uint8_t* p) { return (int16_t)(p[0] | p[1] << 8); }

/* A DELT picture into palette indices (-1 clear): its bounds, then rows of
 * (u16 length << 1 | packed, i16 x, i16 y) with raw bytes or runs (a byte
 * n << 1 | repeat: the next byte n times, else n bytes). */
static int16_t* crawl_delt(const uint8_t* d, size_t n, int* x0, int* y0, int* w, int* h) {
	if (n < 8)
		return NULL;
	const int l = crawl_i16(d), t = crawl_i16(d + 2), r = crawl_i16(d + 4), b = crawl_i16(d + 6);
	*x0 = l;
	*y0 = t;
	*w = r - l + 1;
	*h = b - t + 1;
	if (*w <= 0 || *h <= 0 || *w > 1024 || *h > 1024)
		return NULL;
	int16_t* px = (int16_t*)malloc((size_t)*w * (size_t)*h * sizeof(int16_t));
	if (!px)
		return NULL;
	for (size_t i = 0; i < (size_t)*w * (size_t)*h; ++i)
		px[i] = -1;
	size_t i = 8;
	while (i + 6 <= n) {
		const unsigned sz = (unsigned)(d[i] | d[i + 1] << 8);
		if (!sz)
			break;
		const int x = crawl_i16(d + i + 2) - l, y = crawl_i16(d + i + 4) - t;
		i += 6;
		const int len = (int)(sz >> 1);
		int at = x;
		const bool row_ok = y >= 0 && y < *h;
		if (sz & 1) {
			while (at < x + len && i < n) {
				const int c = d[i++];
				const int k = c >> 1;
				if (c & 1) {
					if (i >= n)
						break;
					const int v = d[i++];
					for (int j = 0; j < k; ++j, ++at)
						if (row_ok && at >= 0 && at < *w)
							px[(size_t)y * *w + at] = (int16_t)v;
				} else {
					for (int j = 0; j < k && i < n; ++j, ++at)
						if (row_ok && at >= 0 && at < *w)
							px[(size_t)y * *w + at] = (int16_t)d[i++];
						else
							++i;
				}
			}
		} else {
			for (int j = 0; j < len && i < n; ++j, ++at)
				if (row_ok && at >= 0 && at < *w)
					px[(size_t)y * *w + at] = (int16_t)d[i++];
				else
					++i;
		}
	}
	return px;
}

/* "along" as a texture: each colour of it through the inverted tonemap,
 * so it shows in the game's own colours. */
static void crawl_load_along(AeronCommandBuffer* cmd, const TieFlightAssetSource* assets) {
	size_t dn = 0, pn = 0;
	uint8_t* delt = crawl_read_lfd(assets, "RESOURCE/TITLE.LFD", "DELT", "along", &dn);
	uint8_t* pltt = crawl_read_lfd(assets, "RESOURCE/TITLE.LFD", "PLTT", "title", &pn);
	int x0 = 0, y0 = 0, w = 0, h = 0;
	int16_t* idx = delt && pltt && pn >= 2 ? crawl_delt(delt, dn, &x0, &y0, &w, &h) : NULL;
	if (idx) {
		const int first = pltt[0], last = pltt[1];
		float hdr[256][3];
		bool used[256] = { false };
		for (size_t i = 0; i < (size_t)w * h; ++i)
			if (idx[i] >= 0)
				used[idx[i]] = true;
		float top = 1e-3f;
		for (int c = 0; c < 256; ++c) {
			hdr[c][0] = hdr[c][1] = hdr[c][2] = 0.0f;
			if (!used[c] || c < first || c > last || 2 + (size_t)(c - first) * 3 + 3 > pn)
				continue;
			const uint8_t* rgb = pltt + 2 + (c - first) * 3;
			const float d[3] = { (float)rgb[0] / 255.0f, (float)rgb[1] / 255.0f, (float)rgb[2] / 255.0f };
			crawl_untonemap(d, hdr[c]);
			top = fmaxf(top, fmaxf(hdr[c][0], fmaxf(hdr[c][1], hdr[c][2])));
		}
		/* A clear texel round it for clean edges. */
		const int tw = w + 2, th = h + 2;
		uint8_t* px = (uint8_t*)calloc((size_t)tw * th, 4);
		if (px) {
			for (int y = 0; y < h; ++y) {
				for (int x = 0; x < w; ++x) {
					const int c = idx[(size_t)y * w + x];
					if (c < 0)
						continue;
					uint8_t* p = px + ((size_t)(y + 1) * tw + x + 1) * 4;
					for (int k = 0; k < 3; ++k)
						p[k] = (uint8_t)(crawl_linear_to_srgb(crawl_clamp01(hdr[c][k] / top)) * 255.0f + 0.5f);
					p[3] = 255;
				}
			}
			if (crawl_upload(cmd, &s_crawl.along, px, tw, th, 1, "tie.vr_intro_crawl.along")) {
				s_crawl.along_rect[0] = (float)x0 - 1.0f;
				s_crawl.along_rect[1] = (float)y0 - 1.0f;
				s_crawl.along_rect[2] = (float)(x0 + w) + 1.0f;
				s_crawl.along_rect[3] = (float)(y0 + h) + 1.0f;
				s_crawl.along_gain = top;
			}
			free(px);
		}
		free(idx);
	} else {
		Aeron_LogInfo("tie.vr", "intro crawl: no \"along\" picture in RESOURCE/TITLE.LFD");
	}
	free(delt);
	free(pltt);
}

/* ---- the crawl's text ---------------------------------------------------- */

typedef struct CrawlLine {
	int cp[CRAWL_MAX_GLYPHS];
	float pen[CRAWL_MAX_GLYPHS]; /* glyph origins, font units */
	int count;
	float ink0, ink1; /* ink extent, font units */
} CrawlLine;

/* Lays one line out in font units (the game's bytes as Latin-1): glyph
 * origins with kerning, its ink extent. */
static void crawl_layout(const stbtt_fontinfo* font, const char* text, CrawlLine* ln) {
	memset(ln, 0, sizeof *ln);
	float pen = 0.0f;
	int prev = 0;
	ln->ink0 = 1e9f;
	ln->ink1 = -1e9f;
	for (const unsigned char* q = (const unsigned char*)text; *q && ln->count < CRAWL_MAX_GLYPHS; ++q) {
		const int c = *q;
		if (c == '\r' || c == '\n')
			continue;
		if (prev)
			pen += (float)stbtt_GetCodepointKernAdvance(font, prev, c);
		int adv, lsb, x0, y0, x1, y1;
		stbtt_GetCodepointHMetrics(font, c, &adv, &lsb);
		if (stbtt_GetCodepointBox(font, c, &x0, &y0, &x1, &y1)) {
			/* The box sheared like the glyph (y up). */
			ln->ink0 = fminf(ln->ink0, pen + (float)x0 + CRAWL_SHEAR * (float)(CRAWL_SHEAR < 0.0f ? y1 : y0));
			ln->ink1 = fmaxf(ln->ink1, pen + (float)x1 + CRAWL_SHEAR * (float)(CRAWL_SHEAR < 0.0f ? y0 : y1));
		}
		ln->cp[ln->count] = c;
		ln->pen[ln->count] = pen;
		ln->count++;
		pen += (float)adv;
		prev = c;
	}
	if (ln->ink1 < ln->ink0)
		ln->count = 0; /* blank: the game's paragraph break */
}

/* Rasterizes the game's lines (vr/LiberationSans-BoldItalic.ttf) into the
 * text texture, in (xn, s) texture space: line i's baseline at s = i PITCH
 * + BASE, centred on xn 0, all at one size (smaller if the widest would not
 * fit the column). */
static bool crawl_load_text(AeronCommandBuffer* cmd) {
	const int n = s_crawl.line_count;
	CrawlLine* lines = (CrawlLine*)calloc(CRAWL_MAX_LINES, sizeof(CrawlLine));
	float* cov = NULL;
	unsigned char* glyph = NULL;
	bool ok = false;
	stbtt_fontinfo font;
	if (!lines || n <= 0 || !s_crawl.ttf ||
		!stbtt_InitFont(&font, s_crawl.ttf, stbtt_GetFontOffsetForIndex(s_crawl.ttf, 0)))
		goto done;
	float widest = 0.0f;
	int last = -1;
	for (int i = 0; i < n; ++i) {
		crawl_layout(&font, s_crawl.lines[i], &lines[i]);
		if (lines[i].count) {
			widest = fmaxf(widest, lines[i].ink1 - lines[i].ink0);
			last = i;
		}
	}
	if (last < 0)
		goto done;
	int tw = (int)crawl_env("XWA_XR_CRAWL_TEXTURE", 4096.0f);
	tw = tw < 1024 ? 1024 : tw > 4096 ? 4096 : tw;
	const float ta = (float)tw / (-2.0f * CRAWL_TEX_X0); /* texels per xn */
	const float tb = ta * CRAWL_TEX_ANISO / CRAWL_K;     /* texels per unit of s */
	int asc, desc, gap;
	stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);
	const float em = stbtt_ScaleForMappingEmToPixels(&font, 1.0f); /* em per font unit */
	/* xn per font unit: XWA's size, or smaller for the widest line. */
	const float xs = fminf(CRAWL_EM_X * em, widest > 0.0f ? 2.0f * CRAWL_HALF_W / widest : 1e9f);
	const float sx = ta * xs;
	const float sy = tb * CRAWL_EM_Y * em; /* texels per font unit, down */
	const float s1 = (float)last * CRAWL_PITCH + CRAWL_BASE - (float)desc * em * CRAWL_EM_Y + 0.2f;
	int th = (int)ceilf((s1 - CRAWL_TEX_S0) * tb);
	th = th > 4096 ? 4096 : th;
	cov = (float*)calloc((size_t)tw * th, sizeof(float));
	glyph = (unsigned char*)malloc(512 * 512);
	if (!cov || !glyph)
		goto done;
	for (int i = 0; i <= last; ++i) {
		const CrawlLine* ln = &lines[i];
		if (!ln->count)
			continue;
		/* Centred: the ink's middle on xn 0. */
		const float ox = -CRAWL_TEX_X0 * ta - 0.5f * (ln->ink0 + ln->ink1) * sx; /* texel x of the pen's origin */
		const float base = ((float)i * CRAWL_PITCH + CRAWL_BASE - CRAWL_TEX_S0) * tb;
		const int by = (int)floorf(base);
		const float fy = base - (float)by;
		for (int g = 0; g < ln->count; ++g) {
			const float gxf = ox + ln->pen[g] * sx;
			const int gx = (int)floorf(gxf);
			const float fx = gxf - (float)gx;
			int x0, y0, x1, y1;
			stbtt_GetCodepointBitmapBoxSubpixel(&font, ln->cp[g], sx, sy, fx, fy, &x0, &y0, &x1, &y1);
			const int gw = x1 - x0, gh = y1 - y0;
			if (gw <= 0 || gh <= 0 || gw > 512 || gh > 512)
				continue;
			stbtt_MakeCodepointBitmapSubpixel(&font, glyph, gw, gh, gw, sx, sy, fx, fy, ln->cp[g]);
			for (int y = 0; y < gh; ++y) {
				const int ty = by + y0 + y;
				if (ty < 0 || ty >= th)
					continue;
				/* The lean: this row's height above the baseline (font units)
				 * moves it across. */
				const float above = (base - ((float)ty + 0.5f)) / sy;
				const float shift = CRAWL_SHEAR * above * sx;
				const int si = (int)floorf(shift);
				const float sf = shift - (float)si;
				for (int x = -1; x < gw; ++x) {
					const int tx = gx + x0 + x + si + 1;
					if (tx < 0 || tx >= tw)
						continue;
					const float g0 = x >= 0 ? (float)glyph[y * gw + x] : 0.0f;
					const float g1 = x + 1 < gw ? (float)glyph[y * gw + x + 1] : 0.0f;
					const float v = (g0 * sf + g1 * (1.0f - sf)) / 255.0f;
					float* d = &cov[(size_t)ty * tw + tx];
					*d = *d > v ? *d : v;
				}
			}
		}
	}
	s_crawl.text_s1 = CRAWL_TEX_S0 + (float)th / tb;
	float lut[1024];
	crawl_coverage_lut(k_text_srgb, lut, s_crawl.text_tint);
	ok = crawl_texture_coverage(cmd, &s_crawl.text, cov, tw, th, lut, "tie.vr_intro_crawl.text");
	cov = NULL; /* freed with the mip chain */
	if (ok)
		Aeron_LogInfo("tie.vr", "intro crawl: %d lines, text %dx%d", last + 1, tw, th);
done:
	free(cov);
	free(glyph);
	free(lines);
	return ok;
}

static void crawl_release(void) {
	CrawlTex* t[3] = { &s_crawl.logo, &s_crawl.text, &s_crawl.along };
	for (int i = 0; i < 3; ++i) {
		if (t[i]->tex)
			Aeron_DestroyTexture(t[i]->tex);
		memset(t[i], 0, sizeof *t[i]);
	}
	SDL_free(s_crawl.ttf);
	s_crawl.ttf = NULL;
	s_crawl.ttf_size = 0;
}

/* ---- per frame ------------------------------------------------------------ */

void TieXrIntroCrawl_SetSnapshot(const TieSnapshot* snap, bool tie98_frames) {
	s_crawl.title = snap && SDL_strcasecmp(snap->current_lfd_basename, "TITLE") == 0 &&
					SDL_strcasecmp(snap->current_film_name, "title") == 0;
	if (!s_crawl.title) {
		s_crawl.line_count = 0;
		return;
	}
	s_crawl.frame = (float)snap->scene_clock.engine_frame + snap->scene_clock.frame_progress -
					(tie98_frames ? 4.0f : 0.0f);
	const char* hold = getenv("TIE_XR_CRAWL_FRAME");
	if (hold && hold[0])
		s_crawl.frame = (float)atof(hold);
	int n = snap->title_crawl_lines ? (int)snap->title_crawl_count : 0;
	n = n > CRAWL_MAX_LINES ? CRAWL_MAX_LINES : n;
	if (n != s_crawl.line_count) {
		s_crawl.line_count = n;
		for (int i = 0; i < n; ++i) {
			snprintf(s_crawl.lines[i], sizeof s_crawl.lines[i], "%s", snap->title_crawl_lines[i].text);
			s_crawl.initial_y[i] = snap->title_crawl_lines[i].initial_y;
		}
	}
}

bool TieXrIntroCrawl_TitleUp(void) { return s_crawl.title && crawl_enabled(); }

void TieXrIntroCrawl_Update(AeronCommandBuffer* cmd, const TieFlightAssetSource* assets) {
	if (!s_crawl.title || !crawl_enabled()) {
		if (s_crawl.started) {
			s_crawl.started = false;
			s_crawl.active = false;
			s_crawl.release_in = 3; /* the last frames may still sample them */
		}
		if (s_crawl.release_in > 0 && --s_crawl.release_in == 0)
			crawl_release();
		return;
	}
	if (!s_crawl.started) {
		s_crawl.started = true;
		s_crawl.release_in = 0;
		crawl_release();
		s_crawl.text_tried = false;
		s_crawl.text_ok = false;
		const uint64_t t0 = SDL_GetTicksNS();
		s_crawl.ttf = (unsigned char*)crawl_load("LiberationSans-BoldItalic.ttf", &s_crawl.ttf_size);
		s_crawl.ok = cmd && s_crawl.ttf && crawl_load_logo(cmd);
		if (s_crawl.ok) {
			crawl_logo_table();
			crawl_load_along(cmd, assets);
			Aeron_LogInfo("tie.vr", "intro crawl: ready in %.0f ms", (double)(SDL_GetTicksNS() - t0) * 1e-6);
		} else {
			crawl_release();
			Aeron_LogInfo("tie.vr", "intro crawl: assets missing, the crawl plays on the screen");
		}
		s_crawl.active = s_crawl.ok;
		s_crawl.skip_held = -1;
		s_crawl.skipped = false;
	}
	/* The text once the game has it (it sets the lines up on its first
	 * frame). */
	if (s_crawl.ok && !s_crawl.text_tried && s_crawl.line_count > 0 && cmd) {
		s_crawl.text_tried = true;
		const uint64_t t0 = SDL_GetTicksNS();
		s_crawl.text_ok = crawl_load_text(cmd);
		if (!s_crawl.text_ok) {
			Aeron_LogWarn("tie.vr", "intro crawl: the text did not rasterize, the crawl plays on the screen");
			s_crawl.active = false;
		} else {
			Aeron_LogInfo("tie.vr", "intro crawl: text in %.0f ms", (double)(SDL_GetTicksNS() - t0) * 1e-6);
		}
	}
}

bool TieXrIntroCrawl_Active(void) { return s_crawl.title && s_crawl.active; }

/* A (X when left-handed) or the left menu button, pressed now: the
 * controllers' own buttons (the rest reach nothing while the crawl plays). */
static int crawl_skip_buttons(void) {
	AeronXrHand left, right;
	memset(&left, 0, sizeof left);
	memset(&right, 0, sizeof right);
	const bool have_left = Aeron_XrGetHand(0, &left) != 0, have_right = Aeron_XrGetHand(1, &right) != 0;
	const AeronXrHand* a_hand = Aeron_XrLeftHanded() ? &left : &right;
	const bool have_a = Aeron_XrLeftHanded() ? have_left : have_right;
	int held = 0;
	if (have_a && (a_hand->buttons & AERON_XR_HAND_BUTTON_LOWER))
		held |= 1;
	if (have_left && (left.buttons & AERON_XR_HAND_BUTTON_MENU))
		held |= 2;
	return held;
}

void TieXrIntroCrawl_PollSkip(void) {
	/* The A latch the blocked controllers keep (A or the trigger) is not
	 * ours: drop it, so it does not reach the menu scene's title later. */
	(void)Aeron_XrTakeAPressed();
	if (!TieXrIntroCrawl_Active())
		return;
	const int held = crawl_skip_buttons();
	/* Only a press during the crawl counts (not one held into it). */
	const int pressed = s_crawl.skip_held < 0 ? 0 : held & ~s_crawl.skip_held;
	s_crawl.skip_held = held;
	if (pressed && !s_crawl.skipped) {
		/* What Space does on the title: on to the intro film. */
		s_crawl.skipped = true;
		TieInput_EnqueueKey((int16_t)' ');
		Aeron_LogInfo("tie.vr", "intro crawl: skipped (%s) at frame %.0f", (pressed & 1) ? "A" : "menu button",
					  (double)s_crawl.frame);
	}
}

bool TieXrIntroCrawl_StarsOn(void) { return !TieXrIntroCrawl_Active() || s_crawl.frame >= (float)TC_LOGO_FROM; }

/* ---- drawing ------------------------------------------------------------------ */

/* The picture on the virtual screen (LOCAL metres: x right, y up, -z ahead):
 * the 4:3 frame fitted into the screen; for the crawl's plane, XWA's 640-wide
 * film picture across its width. */
typedef struct CrawlView {
	float dist, lift, w, m;
	float f; /* the film's focal length in film pixels seen from the eye */
	float k; /* world units per metre */
	float org[3];
} CrawlView;

static bool crawl_view(CrawlView* v, const float view_pos[3], float units) {
	float dist = 2.1f, sw = 2.1f, sh = 1.575f, lift = 0.0f;
	if (!Aeron_XrVirtualScreenGeometry(&dist, &sw, &sh, &lift)) {
		dist = crawl_env("AERON_XR_SCREEN_DISTANCE", dist);
		sw = crawl_env("AERON_XR_SCREEN_WIDTH", sw);
		sh = 0.75f * sw;
		lift = crawl_env("AERON_XR_SCREEN_HEIGHT_OFFSET", lift);
	}
	const float w = fminf(sw, sh * 4.0f / 3.0f);
	if (dist <= 0.05f || w <= 0.01f)
		return false;
	v->dist = dist;
	v->lift = lift;
	v->w = w;
	v->m = w / CRAWL_FILM_W;
	v->f = dist / v->m;
	v->k = units;
	memcpy(v->org, view_pos, sizeof v->org);
	return true;
}

/* LOCAL metres -> world (x right, y ahead, z up). */
static void crawl_world(const CrawlView* v, float x, float y, float z, float out[3]) {
	out[0] = v->org[0] + v->k * x;
	out[1] = v->org[1] - v->k * z;
	out[2] = v->org[2] + v->k * y;
}

static void crawl_add(AeronScene3D* scene, AeronTexture* tex, float c[4][3], const float uv[4][2],
					  const float rgb[3], float alpha) {
	AeronSceneBillboardDesc d;
	memset(&d, 0, sizeof d);
	d.texture = tex;
	d.blend = AERON_SCENE_BILLBOARD_BLEND_PMA;
	d.stage = AERON_SCENE_BILLBOARD_STAGE_OVERLAY;
	for (int i = 0; i < 4; ++i) {
		memcpy(d.corners[i], c[i], sizeof d.corners[i]);
		memcpy(d.uv[i], uv[i], sizeof d.uv[i]);
		for (int k = 0; k < 3; ++k)
			d.colors[i][k] = rgb[k];
		d.colors[i][3] = alpha;
	}
	AeronScene_AddBillboard(scene, &d);
}

/* A quad of a coverage texture made for colour `srgb8` (tint `full` at
 * full strength), faded to `fade` (the picture's values scale with it). */
static void crawl_quad(AeronScene3D* scene, AeronTexture* tex, float c[4][3], const float uv[4][2],
					   const uint8_t srgb8[3], const float full[3], float fade) {
	float rgb[3];
	if (fade >= 0.999f)
		memcpy(rgb, full, sizeof rgb);
	else
		crawl_tint(srgb8, fade, rgb);
	crawl_add(scene, tex, c, uv, rgb, fade);
}

/* "A long time ago..." where the screen is, as the game has it on its
 * picture. */
static void crawl_submit_along(AeronScene3D* scene, const CrawlView* v) {
	const float t = s_crawl.frame;
	if (!s_crawl.along.tex || t >= TC_ALONG_END)
		return;
	const float fade = fminf(crawl_clamp01(t / TC_ALONG_FADE), crawl_clamp01((TC_ALONG_END - t) / TC_ALONG_FADE));
	if (fade <= 0.0f)
		return;
	const float mx = v->w / TC_CLASSIC_W, my = 0.75f * v->w / TC_CLASSIC_H;
	const float* r = s_crawl.along_rect;
	const float x0 = (r[0] - 0.5f * TC_CLASSIC_W) * mx, x1 = (r[2] - 0.5f * TC_CLASSIC_W) * mx;
	const float y0 = v->lift - (r[3] - 0.5f * TC_CLASSIC_H) * my, y1 = v->lift - (r[1] - 0.5f * TC_CLASSIC_H) * my;
	float c[4][3];
	crawl_world(v, x0, y0, -v->dist, c[0]);
	crawl_world(v, x1, y0, -v->dist, c[1]);
	crawl_world(v, x1, y1, -v->dist, c[2]);
	crawl_world(v, x0, y1, -v->dist, c[3]);
	static const float uv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
	const float g = s_crawl.along_gain * fade;
	const float rgb[3] = { g, g, g };
	crawl_add(scene, s_crawl.along.tex, c, uv, rgb, fade);
}

/* The logo: square to the line of sight through the picture's centre, as
 * far away as gives the game's size for it this frame; it starts
 * XWA_XR_CRAWL_LOGO_DISTANCE metres away. */
static void crawl_submit_logo(AeronScene3D* scene, const CrawlView* v) {
	const float t = s_crawl.frame;
	if (!s_crawl.logo.tex || t < (float)TC_LOGO_FROM || t >= (float)TC_LOGO_END)
		return;
	const float ft = t - (float)TC_LOGO_FROM;
	const int i = (int)ft;
	const float a = s_crawl.logo_scale[i], b = s_crawl.logo_scale[i + 1];
	const float scale = fmaxf(a + (b - a) * (ft - (float)i), 1.0f);
	const float d0 = fmaxf(crawl_env("XWA_XR_CRAWL_LOGO_DISTANCE", 5.0f), 0.5f);
	const float first = s_crawl.logo_scale[0];
	const float dist = d0 * first / scale;
	/* The outline's width in metres (the same all the way): the game's size
	 * at its cut, seen d0 away. */
	const float ow = first * TC_LOGO_OUTLINE / TC_CLASSIC_W * v->w * d0 / v->dist;
	const float* bx = s_crawl.logo_box;
	const float aspect = (float)s_crawl.logo.h * (bx[3] - bx[1]) / fmaxf((float)s_crawl.logo.w * (bx[2] - bx[0]), 1.0f);
	const float oh = ow * aspect;
	const float qw = ow / (bx[2] - bx[0]), qh = oh / (bx[3] - bx[1]);
	const float cx = 0.0f, cy = v->lift * dist / v->dist;
	const float x0 = cx - (0.5f * (bx[0] + bx[2])) * qw, x1 = x0 + qw;
	const float y1 = cy + (0.5f * (bx[1] + bx[3])) * qh, y0 = y1 - qh; /* image v runs down */
	float c[4][3];
	crawl_world(v, x0, y0, -dist, c[0]);
	crawl_world(v, x1, y0, -dist, c[1]);
	crawl_world(v, x1, y1, -dist, c[2]);
	crawl_world(v, x0, y1, -dist, c[3]);
	static const float uv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
	const float fade = 1.0f - crawl_clamp01((t - TC_LOGO_FADE_FROM) / ((float)TC_LOGO_END - TC_LOGO_FADE_FROM));
	crawl_quad(scene, s_crawl.logo.tex, c, uv, k_logo_srgb, s_crawl.logo_tint, fade);
}

/* The crawl's plane (OpenXWA-VR's): the film's camera looks down at it, its
 * vanishing line YV lies f tan(phi) above the picture's centre, phi the
 * camera's pitch, h (XWA_XR_CRAWL_HEIGHT) its height above the plane. A text
 * point (xn, s) is, in the film camera's frame, at depth z = f h (tau + T0 -
 * s) / (K cos phi), x = xn h / cos phi, y = z tan phi - h / cos phi; the eye
 * sees the screen as that camera's picture, so in LOCAL space the point lies
 * at (x, y + z lift / dist, -z): linear in xn and s, one quad.
 *
 * The scroll's clock: the first line's top (s = 0) is TEXT_ENTRY_DEG below
 * the logo's line of sight when the game brings the first line up, and a
 * line follows every 28 frames. A point is seen at tan(elevation) = tan(phi)
 * + lift / dist - K / (f (tau + T0 - s)). */
static float crawl_tau(const CrawlView* v) {
	const float tan_phi = (CRAWL_FILM_CY - CRAWL_YV) / v->f;
	const float entry = fminf(fmaxf(crawl_env("XWA_XR_CRAWL_TEXT_ENTRY_DEG", CRAWL_TEXT_ENTRY_DEG), 1.0f), 80.0f);
	const float logo_el = atanf(v->lift / v->dist);
	const float den = fmaxf(tan_phi + v->lift / v->dist - tanf(logo_el - entry * 0.017453293f), 1e-3f);
	const float tau_entry = CRAWL_K / (v->f * den) - CRAWL_T0;
	const float first = s_crawl.line_count > 0 ? s_crawl.initial_y[0] - TC_CLASSIC_H : TC_TEXT_START;
	return tau_entry + (s_crawl.frame - first) * CRAWL_PITCH / TC_LINE_FRAMES;
}

static void crawl_plane_point(const CrawlView* v, float tau, float xn, float s, float out[3]) {
	const float h = fmaxf(crawl_env("XWA_XR_CRAWL_HEIGHT", 12.0f), 1.0f);
	const float tan_phi = (CRAWL_FILM_CY - CRAWL_YV) / v->f;
	const float inv_cos = sqrtf(1.0f + tan_phi * tan_phi);
	const float z = v->f * h * (tau + CRAWL_T0 - s) * inv_cos / CRAWL_K;
	const float x = xn * h * inv_cos;
	const float y = z * tan_phi - h * inv_cos;
	crawl_world(v, x, y + z * v->lift / v->dist, -z, out);
}

static void crawl_submit_text(AeronScene3D* scene, const CrawlView* v) {
	const float t = s_crawl.frame;
	const float first = s_crawl.line_count > 0 ? s_crawl.initial_y[0] - TC_CLASSIC_H : TC_TEXT_START;
	if (!s_crawl.text.tex || t < first - TC_TEXT_FADE_IN || t >= TC_FADE_TO)
		return;
	const float tau = crawl_tau(v);
	/* Lines still to come (tau + T0 - s <= 0) would lie on the plane behind
	 * the player: the quad stops a little short of them. */
	const float xa = CRAWL_TEX_X0, xb = -CRAWL_TEX_X0;
	const float sa = CRAWL_TEX_S0, s1 = s_crawl.text_s1;
	const float sb = fminf(s1, tau + CRAWL_T0 - 0.25f);
	if (sb <= sa)
		return;
	const float vb = (sb - sa) / (s1 - sa);
	float c[4][3];
	crawl_plane_point(v, tau, xa, sb, c[0]);
	crawl_plane_point(v, tau, xb, sb, c[1]);
	crawl_plane_point(v, tau, xb, sa, c[2]);
	crawl_plane_point(v, tau, xa, sa, c[3]);
	const float uv[4][2] = { { 0, vb }, { 1, vb }, { 1, 0 }, { 0, 0 } };
	const float fade = fminf(crawl_clamp01((t - (first - TC_TEXT_FADE_IN)) / TC_TEXT_FADE_IN),
							 1.0f - crawl_clamp01((t - TC_FADE_FROM) / (TC_FADE_TO - TC_FADE_FROM)));
	if (fade > 0.0f)
		crawl_quad(scene, s_crawl.text.tex, c, uv, k_text_srgb, s_crawl.text_tint, fade);
}

void TieXrIntroCrawl_Submit(AeronScene3D* scene, const float view_pos[3], float units) {
	CrawlView v;
	if (!scene || !TieXrIntroCrawl_Active() || !crawl_view(&v, view_pos, units))
		return;
	crawl_submit_along(scene, &v);
	if (s_crawl.text_ok)
		crawl_submit_text(scene, &v);
	crawl_submit_logo(scene, &v);
}

void TieXrIntroCrawl_Shutdown(void) {
	crawl_release();
	memset(&s_crawl, 0, sizeof s_crawl);
}

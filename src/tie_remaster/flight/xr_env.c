/* OpenXR menu environment; see xr_env.h. */

#include "tie_remaster/flight/xr_env.h"
#include "tie_remaster/flight/xr_env_tieart.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#include "aeron/aeron.h"
#include "aeron/asset/flight_model.h"
#include "aeron/asset/opt_model.h"
#include "aeron/log.h"
#include "aeron/scene/billboard.h"
#include "aeron/scene/draw_list2d.h"
#include "aeron/xr.h"
#include "tie_remaster/flight/pbr.h"
#include "tie_remaster/flight/point_lights.h"
#include "tie_remaster/flight/renderer_internal.h"
#include "tie_remaster/flight/stars.h"
#include "tie_remaster/flight/xr_menu_beam.h"
#include "tie_remaster/flight/xr_stereo.h"
#include "tie_remaster/gpu_debug.h"
#include "tie_runtime/flight_assets/source.h"
#include "tie_runtime/species_id.h"
#include "tie_remaster/vr/vr_gloves.h"
#include "tie_remaster/vr/vr_hands.h"

/* ---- scene layout (TIE world axes: x right, y ahead, z up; metres) ---- */

enum {
	TIE_XR_ENV_MODEL_ISD = 0,
	TIE_XR_ENV_MODEL_TIE_LN,
	TIE_XR_ENV_MODEL_TIE_IN,
	TIE_XR_ENV_MODEL_COUNT,
};

static const uint16_t k_env_species[TIE_XR_ENV_MODEL_COUNT] = {
	TIE_SPECIES_IMPERIAL_STAR_DESTROYER,
	TIE_SPECIES_TIE_FIGHTER,
	TIE_SPECIES_TIE_INTERCEPTOR,
};

#define TIE_XR_ENV_STAR_COUNT 1100 /* each direction shows twice (antipodal) */
#define TIE_XR_ENV_PLANET_DIM 512
#define TIE_XR_ENV_NEBULA_DIM 256
#define TIE_XR_ENV_GLOW_DIM 128
#define TIE_XR_ENV_SKY_DISTANCE 60000.0f /* scene units; sky quads only need a direction */
#define TIE_XR_ENV_FADE_S 1.2f
#define TIE_XR_ENV_DIM_S 0.8f

/* The sun (also the key light), up and to the left. */
static const float k_sun_yaw_deg = 105.0f, k_sun_pitch_deg = 28.0f;

/* A flight of craft flying a straight pass, restarting every period_s. */
typedef struct TieXrEnvFlight {
	int model;
	int count;
	float closest[3];   /* point of the lead's path nearest the viewer */
	float dir[3];       /* direction of flight (normalised at use) */
	float half_length;  /* metres either side of `closest` */
	float speed;        /* m/s */
	float period_s;     /* >= 2 * half_length / speed */
	float phase_s;
	float offsets[3][3]; /* wingmen in the lead's frame: right, ahead, up */
	float bank_deg;
} TieXrEnvFlight;

static const TieXrEnvFlight k_flights[] = {
	/* Three TIE fighters crossing above the screen, left to right. */
	{ TIE_XR_ENV_MODEL_TIE_LN, 3, { 0.0f, 110.0f, 42.0f }, { 1.0f, -0.18f, -0.03f }, 1500.0f, 55.0f, 62.0f, 26.75f,
	  { { 0.0f, 0.0f, 0.0f }, { -24.0f, -20.0f, -3.0f }, { 26.0f, -22.0f, 4.0f } }, -6.0f },
	/* Two TIE interceptors out of the Star Destroyer, past the lower left. */
	{ TIE_XR_ENV_MODEL_TIE_IN, 2, { -120.0f, 60.0f, -45.0f }, { -0.62f, -0.76f, -0.14f }, 1700.0f, 80.0f, 74.0f,
	  40.0f, { { 0.0f, 0.0f, 0.0f }, { 20.0f, -16.0f, 6.0f }, { 0.0f, 0.0f, 0.0f } }, 14.0f },
	/* One TIE fighter far out on the right, behind the Star Destroyer. */
	{ TIE_XR_ENV_MODEL_TIE_LN, 1, { 900.0f, 2600.0f, 380.0f }, { -0.35f, 0.94f, 0.05f }, 1800.0f, 60.0f, 80.0f,
	  20.0f, { { 0.0f, 0.0f, 0.0f } }, 0.0f },
};

typedef struct TieXrEnvModel {
	AeronSceneMesh* mesh;
	bool tried;
} TieXrEnvModel;

static struct {
	bool textures_ready;
	AeronTexture* planet;
	AeronTexture* nebula;
	AeronTexture* glow;
	TieFlightStars* stars;
	AeronTextureFormat stars_format;
	float star_dirs[TIE_XR_ENV_STAR_COUNT][4];
	bool star_dirs_ready;
	AeronScenePresentChain* chain;
	AeronTextureFormat chain_format;
	TieXrEnvModel models[TIE_XR_ENV_MODEL_COUNT];
	/* Timing: the scene fades in when it (re)starts and its craft are up. */
	uint64_t last_us;
	uint64_t ready_us;
	bool ready;
	bool rendering;
	float dim;
	float star_half_px;
	struct TieFlightRenderer* hook_renderer;
	bool logged_models;
	/* The frame round the virtual screen (vr/frame.rgba). */
	AeronTexture* frame;
	int frame_w, frame_h;
	bool frame_tried;
	bool menu_seen; /* no frame round the startup films */
	AeronDrawList2D* frame_list;
	bool frame_drawn;
	float frame_tint;
	bool frame_on; /* shown this frame */
	/* The TIE Fighter scene (xr_env_tieart.c). */
	TieFlightXrEnvMode last_mode;
	bool art_up;		 /* shown (loaded, past the startup films) */
	uint64_t art_up_us;	 /* since */
	bool screen_control; /* the screen's fade / input block are ours */
} s_env;

/* The screen as usual again (after the TIE Fighter scene's startup show). */
static void TieXrEnv_ScreenNormal(void) {
	if (!s_env.screen_control)
		return;
	s_env.screen_control = false;
	Aeron_XrSetScreenFade(1.0f);
	Aeron_XrSetInputBlocked(0);
}

TieFlightXrEnvMode TieFlightXrEnv_Mode(void) {
	const char* v = getenv("XWA_XR_ENVIRONMENT");
	if (v && (SDL_strcasecmp(v, "off") == 0 || SDL_strcasecmp(v, "none") == 0))
		return TIE_FLIGHT_XR_ENV_OFF;
	if (v && (strcmp(v, "0") == 0 || SDL_strcasecmp(v, "stars") == 0))
		return TIE_FLIGHT_XR_ENV_STARS;
	/* "1" was the fleet's value before the TIE Fighter scene came. */
	if (v && (strcmp(v, "1") == 0 || SDL_strcasecmp(v, "fleet") == 0))
		return TIE_FLIGHT_XR_ENV_FLEET;
	/* Unset, "tiefighter" (or anything else): the TIE Fighter box art. */
	return TIE_FLIGHT_XR_ENV_TIEFIGHTER;
}

/* ---- small maths ---- */

static void TieXrEnv_Dir(float yaw_deg, float pitch_deg, float out[3]) {
	const float d2r = 3.14159265f / 180.0f;
	const float cp = cosf(pitch_deg * d2r);
	out[0] = -sinf(yaw_deg * d2r) * cp; /* positive yaw = left */
	out[1] = cosf(yaw_deg * d2r) * cp;
	out[2] = sinf(pitch_deg * d2r);
}

static void TieXrEnv_Cross(const float a[3], const float b[3], float out[3]) {
	const float r[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
	memcpy(out, r, sizeof r);
}

static bool TieXrEnv_Normalize(float v[3]) {
	const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l < 1e-6f)
		return false;
	v[0] /= l;
	v[1] /= l;
	v[2] /= l;
	return true;
}

static float TieXrEnv_Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static float TieXrEnv_Smooth(float e0, float e1, float x) {
	const float t = TieXrEnv_Clamp01((x - e0) / (e1 - e0));
	return t * t * (3.0f - 2.0f * t);
}

static float TieXrEnv_EnvFloat(const char* name, float fallback) {
	const char* v = getenv(name);
	return v && v[0] ? (float)atof(v) : fallback;
}

/* ---- procedural art (nothing copied: noise and shading) ---- */

static uint32_t TieXrEnv_Hash(int x, int y, int z) {
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2246822519u + 0x9E3779B9u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

static float TieXrEnv_Noise(float x, float y, float z) {
	const int ix = (int)floorf(x), iy = (int)floorf(y), iz = (int)floorf(z);
	const float fx = x - (float)ix, fy = y - (float)iy, fz = z - (float)iz;
	const float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy), uz = fz * fz * (3.0f - 2.0f * fz);
	float c[8];
	for (int i = 0; i < 8; ++i)
		c[i] = (float)(TieXrEnv_Hash(ix + (i & 1), iy + ((i >> 1) & 1), iz + (i >> 2)) & 0xFFFFu) / 65535.0f;
	const float x00 = c[0] + (c[1] - c[0]) * ux, x10 = c[2] + (c[3] - c[2]) * ux;
	const float x01 = c[4] + (c[5] - c[4]) * ux, x11 = c[6] + (c[7] - c[6]) * ux;
	const float y0 = x00 + (x10 - x00) * uy, y1 = x01 + (x11 - x01) * uy;
	return y0 + (y1 - y0) * uz;
}

static float TieXrEnv_Fbm(float x, float y, float z, int octaves) {
	float sum = 0.0f, amp = 0.5f, norm = 0.0f;
	for (int o = 0; o < octaves; ++o) {
		sum += amp * TieXrEnv_Noise(x, y, z);
		norm += amp;
		x = x * 2.03f + 17.1f;
		y = y * 2.03f + 3.7f;
		z = z * 2.03f + 9.2f;
		amp *= 0.5f;
	}
	return sum / norm;
}

static uint8_t TieXrEnv_Srgb8(float linear) {
	const float l = TieXrEnv_Clamp01(linear);
	const float s = l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
	return (uint8_t)lrintf(s * 255.0f);
}

/* A banded ice world, lit from the upper left, with a thin blue atmosphere
 * (straight alpha). */
static void TieXrEnv_MakePlanet(uint8_t* px, int n) {
	const float radius = 0.84f;
	float light[3] = { -0.78f, 0.52f, 0.30f };
	TieXrEnv_Normalize(light);
	const float aa = 2.5f / (float)n;
	for (int j = 0; j < n; ++j) {
		for (int i = 0; i < n; ++i) {
			const float u = ((float)i + 0.5f) / (float)n * 2.0f - 1.0f;
			const float v = 1.0f - ((float)j + 0.5f) / (float)n * 2.0f;
			const float r = sqrtf(u * u + v * v);
			float rgb[3] = { 0.0f, 0.0f, 0.0f };
			float alpha = 0.0f;
			if (r < radius + aa) {
				const float nx = u / radius, ny = v / radius;
				const float nz2 = 1.0f - nx * nx - ny * ny;
				const float nz = nz2 > 0.0f ? sqrtf(nz2) : 0.0f;
				/* Body frame tilted 22 degrees for the bands. */
				const float ct = 0.927f, st = 0.375f;
				const float bx = nx * ct - ny * st, by = nx * st + ny * ct, bz = nz;
				const float warp = TieXrEnv_Fbm(bx * 3.0f, by * 3.0f, bz * 3.0f, 4);
				const float band = 0.5f + 0.5f * sinf(by * 11.0f + warp * 5.5f);
				const float detail = TieXrEnv_Fbm(bx * 9.0f + 4.0f, by * 22.0f, bz * 9.0f, 4);
				const float t = TieXrEnv_Clamp01(0.55f * band + 0.45f * detail);
				static const float pale[3] = { 0.46f, 0.52f, 0.60f };
				static const float dusk[3] = { 0.20f, 0.23f, 0.30f };
				static const float sand[3] = { 0.50f, 0.45f, 0.38f };
				float albedo[3];
				for (int c = 0; c < 3; ++c) {
					albedo[c] = dusk[c] + (pale[c] - dusk[c]) * t;
					albedo[c] += (sand[c] - albedo[c]) * 0.35f * TieXrEnv_Smooth(0.62f, 0.85f, detail);
				}
				const float ndl = nx * light[0] + ny * light[1] + nz * light[2];
				const float lit = TieXrEnv_Smooth(-0.08f, 0.35f, ndl) * (0.25f + 0.75f * TieXrEnv_Clamp01(ndl));
				const float rim = powf(1.0f - nz, 3.0f) * TieXrEnv_Smooth(-0.25f, 0.4f, ndl);
				for (int c = 0; c < 3; ++c)
					rgb[c] = albedo[c] * lit + 0.002f;
				rgb[0] += rim * 0.10f;
				rgb[1] += rim * 0.22f;
				rgb[2] += rim * 0.48f;
				alpha = TieXrEnv_Clamp01((radius + aa - r) / (2.0f * aa));
			}
			if (r > radius - aa) {
				/* Atmospheric halo on the lit limb. */
				const float ldot = (u * light[0] + v * light[1]) / (r > 1e-4f ? r : 1e-4f);
				const float halo = expf(-fmaxf(r - radius, 0.0f) / 0.018f) * TieXrEnv_Smooth(-0.35f, 0.6f, ldot);
				const float a = halo * 0.85f;
				if (a > alpha * 0.999f && r >= radius) {
					rgb[0] = 0.16f;
					rgb[1] = 0.34f;
					rgb[2] = 0.75f;
					alpha = a;
				} else if (r >= radius - aa) {
					rgb[0] += a * 0.10f;
					rgb[1] += a * 0.20f;
					rgb[2] += a * 0.45f;
				}
			}
			uint8_t* p = &px[((size_t)j * (size_t)n + (size_t)i) * 4u];
			p[0] = TieXrEnv_Srgb8(rgb[0]);
			p[1] = TieXrEnv_Srgb8(rgb[1]);
			p[2] = TieXrEnv_Srgb8(rgb[2]);
			p[3] = (uint8_t)lrintf(TieXrEnv_Clamp01(alpha) * 255.0f);
		}
	}
}

/* A soft cloud of gas for additive sky quads: warped noise in a round
 * falloff, magenta to blue. */
static void TieXrEnv_MakeNebula(uint8_t* px, int n) {
	for (int j = 0; j < n; ++j) {
		for (int i = 0; i < n; ++i) {
			const float u = ((float)i + 0.5f) / (float)n * 2.0f - 1.0f;
			const float v = ((float)j + 0.5f) / (float)n * 2.0f - 1.0f;
			const float r = sqrtf(u * u + v * v);
			const float wx = TieXrEnv_Fbm(u * 2.2f + 11.0f, v * 2.2f, 1.7f, 4);
			const float wy = TieXrEnv_Fbm(u * 2.2f, v * 2.2f + 5.0f, 8.3f, 4);
			const float d = TieXrEnv_Fbm(u * 2.6f + 2.4f * wx, v * 2.6f + 2.4f * wy, 3.1f, 5);
			const float mask = 1.0f - TieXrEnv_Smooth(0.35f, 1.0f, r + 0.25f * (wx - 0.5f));
			const float density = powf(TieXrEnv_Smooth(0.38f, 0.82f, d), 1.6f) * mask;
			const float hue = TieXrEnv_Smooth(0.3f, 0.7f, wy);
			const float rgb[3] = {
				density * (0.55f * (1.0f - hue) + 0.16f * hue),
				density * (0.14f * (1.0f - hue) + 0.24f * hue),
				density * (0.42f * (1.0f - hue) + 0.70f * hue),
			};
			uint8_t* p = &px[((size_t)j * (size_t)n + (size_t)i) * 4u];
			p[0] = TieXrEnv_Srgb8(rgb[0]);
			p[1] = TieXrEnv_Srgb8(rgb[1]);
			p[2] = TieXrEnv_Srgb8(rgb[2]);
			p[3] = (uint8_t)lrintf(TieXrEnv_Clamp01(density) * 255.0f);
		}
	}
}

/* Radial glow (sun): a small hot core in a halo that fades out well
 * inside the quad (additive; alpha only keeps the texels from the
 * billboard's alpha test). */
static void TieXrEnv_MakeGlow(uint8_t* px, int n) {
	for (int j = 0; j < n; ++j) {
		for (int i = 0; i < n; ++i) {
			const float u = ((float)i + 0.5f) / (float)n * 2.0f - 1.0f;
			const float v = ((float)j + 0.5f) / (float)n * 2.0f - 1.0f;
			const float r = sqrtf(u * u + v * v);
			const float edge = 1.0f - TieXrEnv_Smooth(0.55f, 1.0f, r);
			const float core = 1.0f - TieXrEnv_Smooth(0.03f, 0.07f, r);
			const float glow = (0.45f * expf(-r * 16.0f) + 0.10f * expf(-r * 5.0f)) * edge;
			const float value = TieXrEnv_Clamp01(core + glow);
			uint8_t* p = &px[((size_t)j * (size_t)n + (size_t)i) * 4u];
			p[0] = p[1] = p[2] = TieXrEnv_Srgb8(value);
			p[3] = r < 1.0f ? 255 : 0;
		}
	}
}

static AeronTexture* TieXrEnv_Texture(AeronCommandBuffer* cmd, int n, void (*make)(uint8_t*, int),
									  const char* name) {
	uint8_t* px = malloc((size_t)n * (size_t)n * 4u);
	if (!px)
		return NULL;
	make(px, n);
	AeronTexture* tex = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width = n,
		.height = n,
		.mip_count = 1,
		.format = AERON_TEXTURE_FORMAT_RGBA8_SRGB,
		.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = name,
	});
	if (tex && !Aeron_UploadTextureDataCmd(cmd, &(AeronTextureUploadDesc) {
													.texture = tex,
													.width = n,
													.height = n,
													.raw_data = px,
													.raw_size = (uint32_t)(n * n * 4),
												})) {
		Aeron_DestroyTexture(tex);
		tex = NULL;
	}
	free(px);
	return tex;
}

static void TieXrEnv_MakeStars(void) {
	uint32_t seed = 0x5EEDu;
	float band_axis[3] = { 0.30f, -0.25f, 0.92f }; /* the galactic plane's normal */
	TieXrEnv_Normalize(band_axis);
	for (int i = 0; i < TIE_XR_ENV_STAR_COUNT; ++i) {
		float d[3];
		for (;;) {
			for (int a = 0; a < 3; ++a) {
				seed = seed * 1664525u + 1013904223u;
				d[a] = (float)(seed >> 8) / 8388608.0f - 1.0f;
			}
			const float l2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
			if (l2 > 0.01f && l2 <= 1.0f)
				break;
		}
		TieXrEnv_Normalize(d);
		seed = seed * 1664525u + 1013904223u;
		const float pick = (float)(seed >> 8) / 16777216.0f;
		if (i % 5 < 2) {
			/* A third of them crowd towards a band across the sky. */
			const float k = d[0] * band_axis[0] + d[1] * band_axis[1] + d[2] * band_axis[2];
			for (int a = 0; a < 3; ++a)
				d[a] -= band_axis[a] * k * 0.8f;
			TieXrEnv_Normalize(d);
		}
		seed = seed * 1664525u + 1013904223u;
		const float b = (float)(seed >> 8) / 16777216.0f;
		float intensity = 0.03f + 0.35f * powf(b, 5.0f);
		if (pick > 0.985f)
			intensity = 0.7f + 0.5f * b; /* a few bright ones */
		memcpy(s_env.star_dirs[i], d, sizeof d);
		s_env.star_dirs[i][3] = intensity;
	}
	s_env.star_dirs_ready = true;
}

static bool TieXrEnv_EnsureTextures(AeronCommandBuffer* cmd) {
	if (s_env.textures_ready)
		return true;
	if (!s_env.planet)
		s_env.planet = TieXrEnv_Texture(cmd, TIE_XR_ENV_PLANET_DIM, TieXrEnv_MakePlanet, "tie.vr.env.planet");
	if (!s_env.nebula)
		s_env.nebula = TieXrEnv_Texture(cmd, TIE_XR_ENV_NEBULA_DIM, TieXrEnv_MakeNebula, "tie.vr.env.nebula");
	if (!s_env.glow)
		s_env.glow = TieXrEnv_Texture(cmd, TIE_XR_ENV_GLOW_DIM, TieXrEnv_MakeGlow, "tie.vr.env.glow");
	if (!s_env.star_dirs_ready)
		TieXrEnv_MakeStars();
	s_env.textures_ready = s_env.planet && s_env.nebula && s_env.glow;
	return s_env.textures_ready;
}

/* ---- craft from the player's flight models ---- */

AeronSceneMesh* TieFlightXrEnv_LoadModel(AeronCommandBuffer* cmd, const TieFlightAssetSource* source,
										 uint16_t species, float bounds_m[6]);

AeronSceneMesh* TieFlightXrEnv_LoadModel(AeronCommandBuffer* cmd, const TieFlightAssetSource* source,
										 uint16_t species, float bounds_m[6]) {
	for (const TieFlightAssetSource* src = source; src; src = src->fallback) {
		if (TieFlightAssetSource_IsTie95(src))
			continue; /* DOS ShipModelData, no OPT */
		uint8_t* bytes = NULL;
		size_t size = 0;
		const TieFlightAssetEntry* entry = NULL;
		char error[512];
		if (!TieFlightAssetSource_ReadModel(src, species, &bytes, &size, &entry, error, sizeof error)) {
			Aeron_LogWarn("tie.vr", "menu scene: species %u not available from %s: %s", (unsigned)species,
						  src->name ? src->name : "?", error);
			continue;
		}
		const char* label = entry ? entry->path : "vr-env";
		AeronFlightModel model;
		memset(&model, 0, sizeof model);
		bool built;
		if (TieFlightAssetSource_IsRemastered(src)) {
			built = Aeron_FlightModelBuildMemory(bytes, size, label, &model);
		} else {
			AeronOptModelError opt_error = { 0 };
			built = Aeron_OptModelBuildMemory(bytes, size, label,
											  &(AeronOptModelBuildOptions) {
												  .smooth_angle_degrees = src->smooth_angle_degrees,
												  .emissive_strength = 1.5f,
												  .emissive = true,
											  },
											  &model, &opt_error);
		}
		free(bytes);
		if (!built) {
			Aeron_LogWarn("tie.vr", "menu scene: cannot build %s", label);
			Aeron_FlightModelFree(&model);
			continue;
		}
		if (bounds_m) {
			/* min x y z, max x y z (OPT axes, metres). */
			bounds_m[0] = model.bounds.min.x;
			bounds_m[1] = model.bounds.min.y;
			bounds_m[2] = model.bounds.min.z;
			bounds_m[3] = model.bounds.max.x;
			bounds_m[4] = model.bounds.max.y;
			bounds_m[5] = model.bounds.max.z;
		}
		char name[64];
		snprintf(name, sizeof name, "vr.env.model[%u]", (unsigned)species);
		AeronSceneMeshCreateStatus status = AERON_SCENE_MESH_CREATE_SUCCESS;
		AeronSceneMesh* mesh = AeronScene_MeshCreate(cmd, &model, name, &status);
		Aeron_FlightModelFree(&model);
		if (mesh) {
			Aeron_LogInfo("tie.vr", "menu scene: %s from %s (%u vertices)", label, src->name ? src->name : "?",
						  mesh->vertex_count);
			return mesh;
		}
	}
	return NULL;
}

/* One model per frame; true when every model was tried. */
static bool TieXrEnv_LoadNextModel(AeronCommandBuffer* cmd, const TieFlightAssetSource* assets) {
	for (int m = 0; m < TIE_XR_ENV_MODEL_COUNT; ++m) {
		TieXrEnvModel* model = &s_env.models[m];
		if (model->tried)
			continue;
		model->tried = true;
		if (assets)
			model->mesh = TieFlightXrEnv_LoadModel(cmd, assets, k_env_species[m], NULL);
		return false;
	}
	return true;
}

void TieFlightXrEnv_ReleaseShips(void) {
	if (TieXrArt_ReleaseShips())
		s_env.art_up = false; /* fades in again with its craft */
	TieXrEnv_ScreenNormal();
	for (int m = 0; m < TIE_XR_ENV_MODEL_COUNT; ++m) {
		if (s_env.models[m].mesh)
			AeronScene_MeshDestroy(s_env.models[m].mesh);
		s_env.models[m].mesh = NULL;
		s_env.models[m].tried = false;
	}
	s_env.ready = false;
}

/* Row-major model -> world (scene units) for a craft at `pos` (metres)
 * heading `fwd`, the flight renderer's convention: OPT vertex x right,
 * -y ahead, z up, metres. */
static void TieXrEnv_CraftTransform(float out[16], const float pos[3], const float fwd_in[3], float bank_rad) {
	const float u = AERON_OPT_UNITS_PER_METER;
	float f[3] = { fwd_in[0], fwd_in[1], fwd_in[2] };
	TieXrEnv_Normalize(f);
	static const float world_up[3] = { 0.0f, 0.0f, 1.0f };
	float r[3], up[3];
	TieXrEnv_Cross(f, world_up, r);
	if (!TieXrEnv_Normalize(r)) {
		r[0] = 1.0f;
		r[1] = r[2] = 0.0f;
	}
	TieXrEnv_Cross(r, f, up);
	const float cb = cosf(bank_rad), sb = sinf(bank_rad);
	const float rb[3] = { r[0] * cb + up[0] * sb, r[1] * cb + up[1] * sb, r[2] * cb + up[2] * sb };
	const float ub[3] = { up[0] * cb - r[0] * sb, up[1] * cb - r[1] * sb, up[2] * cb - r[2] * sb };
	for (int row = 0; row < 3; ++row) {
		out[row * 4 + 0] = rb[row] * u;
		out[row * 4 + 1] = -f[row] * u;
		out[row * 4 + 2] = ub[row] * u;
		out[row * 4 + 3] = pos[row] * u;
	}
	out[12] = out[13] = out[14] = 0.0f;
	out[15] = 1.0f;
}

static void TieXrEnv_AddCraft(AeronScene3D* scene, const AeronSceneMesh* mesh, const float transform[16]) {
	AeronSceneMeshInstance in;
	memset(&in, 0, sizeof in);
	in.mesh = mesh;
	memcpy(in.transform, transform, sizeof in.transform);
	memcpy(in.prev_transform, transform, sizeof in.prev_transform);
	if (mesh->all_materials_single_sided)
		in.cull_mode = AERON_CULL_BACK;
	in.zero_velocity = 1;
	in.shadow_flags = AERON_SCENE_INSTANCE_NO_CAST_SHADOW | AERON_SCENE_INSTANCE_NO_RECEIVE_SHADOW;
	AeronScene_AddMeshInstance(scene, &in);
}

static void TieXrEnv_SubmitCraft(AeronScene3D* scene, float t_s) {
	/* The Star Destroyer holds station off to the right. */
	const AeronSceneMesh* isd = s_env.models[TIE_XR_ENV_MODEL_ISD].mesh;
	if (isd) {
		float pos[3];
		TieXrEnv_Dir(-36.0f, 4.0f, pos);
		for (int a = 0; a < 3; ++a)
			pos[a] *= 1700.0f;
		const float fwd[3] = { -0.86f, 0.50f, 0.02f };
		float m[16];
		TieXrEnv_CraftTransform(m, pos, fwd, 0.0f);
		TieXrEnv_AddCraft(scene, isd, m);
	}
	for (size_t f = 0; f < sizeof k_flights / sizeof k_flights[0]; ++f) {
		const TieXrEnvFlight* fl = &k_flights[f];
		const AeronSceneMesh* mesh = s_env.models[fl->model].mesh;
		if (!mesh)
			continue;
		const float travel = 2.0f * fl->half_length / fl->speed;
		const float cycle = fmodf(t_s + fl->phase_s, fl->period_s);
		if (cycle > travel)
			continue; /* between passes */
		float dir[3] = { fl->dir[0], fl->dir[1], fl->dir[2] };
		TieXrEnv_Normalize(dir);
		const float along = -fl->half_length + cycle * fl->speed;
		float lead[3];
		for (int a = 0; a < 3; ++a)
			lead[a] = fl->closest[a] + dir[a] * along;
		/* The lead's frame for the wingmen. */
		static const float world_up[3] = { 0.0f, 0.0f, 1.0f };
		float right[3], up[3];
		TieXrEnv_Cross(dir, world_up, right);
		TieXrEnv_Normalize(right);
		TieXrEnv_Cross(right, dir, up);
		for (int w = 0; w < fl->count && w < 3; ++w) {
			const float* o = fl->offsets[w];
			/* A little life: slow weaving and banking. */
			const float wobble = sinf(t_s * 0.7f + (float)(w * 2 + (int)f)) ;
			float pos[3];
			for (int a = 0; a < 3; ++a)
				pos[a] = lead[a] + right[a] * o[0] + dir[a] * o[1] + up[a] * (o[2] + 1.5f * wobble);
			const float bank = (fl->bank_deg + 4.0f * wobble) * 3.14159265f / 180.0f;
			float m[16];
			TieXrEnv_CraftTransform(m, pos, dir, bank);
			TieXrEnv_AddCraft(scene, mesh, m);
		}
	}
}

/* ---- sky quads ---- */

static void TieXrEnv_SkyQuad(AeronScene3D* scene, AeronTexture* tex, AeronSceneBillboardBlend blend,
							 const float dir_in[3], float half_angle_deg, float roll_deg, const float color[4]) {
	float dir[3] = { dir_in[0], dir_in[1], dir_in[2] };
	if (!tex || !TieXrEnv_Normalize(dir))
		return;
	static const float world_up[3] = { 0.0f, 0.0f, 1.0f };
	float right[3], up[3];
	TieXrEnv_Cross(dir, world_up, right);
	if (!TieXrEnv_Normalize(right)) {
		right[0] = 1.0f;
		right[1] = right[2] = 0.0f;
	}
	TieXrEnv_Cross(right, dir, up);
	const float roll = roll_deg * 3.14159265f / 180.0f;
	const float cr = cosf(roll), sr = sinf(roll);
	const float h = TIE_XR_ENV_SKY_DISTANCE * tanf(half_angle_deg * 3.14159265f / 180.0f);
	float rx[3], uy[3], c[3];
	for (int a = 0; a < 3; ++a) {
		rx[a] = (right[a] * cr + up[a] * sr) * h;
		uy[a] = (up[a] * cr - right[a] * sr) * h;
		c[a] = dir[a] * TIE_XR_ENV_SKY_DISTANCE;
	}
	AeronSceneBillboardDesc b;
	memset(&b, 0, sizeof b);
	b.texture = tex;
	b.blend = blend;
	b.stage = AERON_SCENE_BILLBOARD_STAGE_SKY;
	static const float su[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
	static const float sv[4] = { 1.0f, 1.0f, -1.0f, -1.0f };
	for (int k = 0; k < 4; ++k) {
		for (int a = 0; a < 3; ++a)
			b.corners[k][a] = c[a] + rx[a] * su[k] + uy[a] * sv[k];
		b.uv[k][0] = 0.5f + 0.5f * su[k];
		b.uv[k][1] = 0.5f - 0.5f * sv[k];
		memcpy(b.colors[k], color, sizeof b.colors[k]);
	}
	AeronScene_AddBillboard(scene, &b);
}

static void TieXrEnv_SubmitSky(AeronScene3D* scene, bool fleet) {
	if (!fleet)
		return;
	/* Nebula clouds (additive), planet (alpha), sun (additive). */
	static const struct {
		float yaw, pitch, half, roll, gain;
	} k_clouds[] = {
		{ 28.0f, 42.0f, 42.0f, 20.0f, 0.40f },  { 150.0f, 12.0f, 50.0f, 75.0f, 0.38f },
		{ -120.0f, -18.0f, 46.0f, 140.0f, 0.34f }, { -40.0f, 55.0f, 34.0f, 200.0f, 0.26f },
		{ 75.0f, -45.0f, 38.0f, 300.0f, 0.22f },
	};
	for (size_t i = 0; i < sizeof k_clouds / sizeof k_clouds[0]; ++i) {
		float d[3];
		TieXrEnv_Dir(k_clouds[i].yaw, k_clouds[i].pitch, d);
		const float g = k_clouds[i].gain;
		const float color[4] = { g, g, g, g };
		TieXrEnv_SkyQuad(scene, s_env.nebula, AERON_SCENE_BILLBOARD_BLEND_ADDITIVE, d, k_clouds[i].half,
						 k_clouds[i].roll, color);
	}
	{
		float d[3];
		TieXrEnv_Dir(k_sun_yaw_deg, k_sun_pitch_deg, d);
		static const float sun[4] = { 9.0f, 7.8f, 6.2f, 1.0f };
		TieXrEnv_SkyQuad(scene, s_env.glow, AERON_SCENE_BILLBOARD_BLEND_ADDITIVE, d, 10.0f, 0.0f, sun);
	}
	{
		/* Lower left; its texture is lit from the upper left, like the sun. */
		float d[3];
		TieXrEnv_Dir(48.0f, -24.0f, d);
		static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		TieXrEnv_SkyQuad(scene, s_env.planet, AERON_SCENE_BILLBOARD_BLEND_ALPHA, d, 24.0f, 0.0f, white);
	}
}

/* ---- per-frame ---- */

static void TieXrEnv_HookStars(AeronCommandBuffer* cmd, AeronRenderPass* pass, int rt_w, int rt_h, void* user) {
	TieFlightRenderer* g = (TieFlightRenderer*)user;
	if (!pass || !g || !g->scene || !s_env.stars)
		return;
	TieFlightStars_DrawCustom(s_env.stars, cmd, pass, AeronScene_ViewProj(g->scene), rt_w, rt_h,
							  s_env.star_half_px);
}

static bool TieXrEnv_OutputEye(TieFlightRenderer* g, AeronCommandBuffer* cmd, int eye, AeronRenderTarget* target,
							   AeronTextureFormat format, float gain) {
	if (!s_env.chain || s_env.chain_format != format) {
		if (s_env.chain)
			AeronScenePresentChain_Destroy(s_env.chain);
		s_env.chain = AeronScenePresentChain_Create(format);
		s_env.chain_format = s_env.chain ? format : AERON_TEXTURE_FORMAT_UNKNOWN;
	}
	AeronRenderTarget* scene_rt = AeronScene_SceneRt(g->scene);
	if (!scene_rt)
		scene_rt = AeronScene_ColorRt(g->scene);
	if (!s_env.chain || !scene_rt || !target)
		return false;
	AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
		.color_target = target,
		.clear_color = 1,
		.clear_color_rgba = { 0.0f, 0.0f, 0.0f, 1.0f },
		.command_buffer = cmd,
		.debug_label = "TIE VR menu scene tonemap",
	});
	if (!pass)
		return false;
	const float tint[4] = { gain, gain, gain, 1.0f };
	AeronScenePresentChain_Draw(s_env.chain, pass, Aeron_RenderTargetGetTexture(scene_rt), g->sampler_linear, NULL,
								0.0f, g->rt_w, g->rt_h, 1.0f, tint, 0);
	/* The hands' alpha, for the overlay that shows them over the screen. */
	AeronDrawList2D* hands_alpha = TieVrHands_EyeAlpha(eye);
	if (hands_alpha)
		AeronDrawList_RenderIntoPass(hands_alpha, cmd, pass, target);
	Aeron_EndRenderPass(pass);
	return true;
}

/* ---- the frame round the virtual screen ----
 *
 * Same rules as OpenXWA-VR (vr_env.c): an image asset "XWAL", u32 width,
 * u32 height, straight RGBA, shipped in the APK as assets/vr/frame.rgba (or
 * next to the executable as vr/frame.rgba; XWA_XR_FRAME=<path> overrides it,
 * XWA_XR_FRAME=0 turns it off); never part of the source tree. Its opening
 * (k_frame_opening, image pixels) is the frame's clear 4:3 area enlarged
 * 2 %: the 4:3 picture fills it exactly, the bezel overlaps under 1 % of the
 * picture's edges, dark backing in the image fills any gap. The frame is a
 * compositor layer (AERON_XR_PANEL_FRAME) XWA_XR_FRAME_DEPTH (2 cm) in front
 * of the picture, scaled so its opening lines up with the picture from the
 * viewer; XWA_XR_FRAME_OVERSCAN > 1 shows the picture smaller in it. There
 * is none round the startup films, before the first menu. */
static const float k_frame_opening[4] = { 86.9f, 124.2f, 1187.9f, 949.9f }; /* x0, y0, x1, y1 */

static float TieXrEnv_SrgbToLinear(float c) {
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float TieXrEnv_LinearToSrgb(float c) {
	c = TieXrEnv_Clamp01(c);
	return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static void TieXrEnv_LoadFrame(AeronCommandBuffer* cmd) {
	if (s_env.frame_tried)
		return;
	s_env.frame_tried = true;
	const char* override = getenv("XWA_XR_FRAME");
	if (override && strcmp(override, "0") == 0)
		return;
	const char* paths[] = { override && override[0] ? override : NULL, "vr/frame.rgba", "assets/vr/frame.rgba" };
	for (size_t i = 0; i < sizeof paths / sizeof paths[0] && !s_env.frame; ++i) {
		if (!paths[i])
			continue;
		size_t size = 0;
		uint8_t* data = (uint8_t*)SDL_LoadFile(paths[i], &size);
		if (!data)
			continue;
		uint32_t w = 0, h = 0;
		if (size >= 12 && memcmp(data, "XWAL", 4) == 0) {
			memcpy(&w, data + 4, 4);
			memcpy(&h, data + 8, 4);
		}
		if (w && h && w <= AERON_XR_FRAME_WIDTH && h <= AERON_XR_FRAME_HEIGHT && size >= 12 + (size_t)w * h * 4u) {
			/* Premultiplied in linear light, stored sRGB. */
			uint8_t* px = malloc((size_t)w * h * 4u);
			if (px) {
				float lut[256];
				for (int k = 0; k < 256; ++k)
					lut[k] = TieXrEnv_SrgbToLinear((float)k / 255.0f);
				const uint8_t* rgba = data + 12;
				for (size_t k = 0; k < (size_t)w * h; ++k) {
					const float a = (float)rgba[k * 4 + 3] / 255.0f;
					for (int c = 0; c < 3; ++c)
						px[k * 4 + c] = (uint8_t)(TieXrEnv_LinearToSrgb(lut[rgba[k * 4 + c]] * a) * 255.0f + 0.5f);
					px[k * 4 + 3] = rgba[k * 4 + 3];
				}
				AeronTexture* t = Aeron_CreateTexture(&(AeronTextureDesc) {
					.width = (int)w,
					.height = (int)h,
					.mip_count = 1,
					.format = AERON_TEXTURE_FORMAT_RGBA8_SRGB,
					.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
					.debug_name = "tie.vr_env.frame",
				});
				if (t && Aeron_UploadTextureDataCmd(cmd, &(AeronTextureUploadDesc) {
															 .texture = t,
															 .width = (int)w,
															 .height = (int)h,
															 .raw_data = px,
															 .raw_size = (uint32_t)((size_t)w * h * 4u),
														 })) {
					s_env.frame = t;
					s_env.frame_w = (int)w;
					s_env.frame_h = (int)h;
				} else if (t) {
					Aeron_DestroyTexture(t);
				}
				free(px);
			}
		}
		SDL_free(data);
	}
	if (s_env.frame)
		Aeron_LogInfo("tie.vr", "screen frame %dx%d", s_env.frame_w, s_env.frame_h);
	else
		Aeron_LogInfo("tie.vr", "no screen frame (vr/frame.rgba)");
}

/* The frame quad in head-relative metres (OpenXR: x right, y up, -z ahead):
 * centre and size. False without a frame or a virtual screen. */
static bool TieXrEnv_FrameQuad(float centre[3], float size[2]) {
	if (!s_env.frame)
		return false;
	float dist = 0.0f, sw = 0.0f, sh = 0.0f, lift = 0.0f;
	if (!Aeron_XrVirtualScreenGeometry(&dist, &sw, &sh, &lift) || dist <= 0.01f || sw <= 0.0f || sh <= 0.0f)
		return false;
	/* The 4:3 picture as the virtual screen shows it, centred. */
	const float overscan = fmaxf(TieXrEnv_EnvFloat("XWA_XR_FRAME_OVERSCAN", 1.0f), 0.5f);
	const float pic_w = fminf(sw, sh * 4.0f / 3.0f) / overscan;
	const float frame_dist = fmaxf(dist - TieXrEnv_EnvFloat("XWA_XR_FRAME_DEPTH", 0.02f), 0.2f);
	const float scale = frame_dist / dist;
	const float px = scale * pic_w / (k_frame_opening[2] - k_frame_opening[0]);
	const float tw = (float)s_env.frame_w, th = (float)s_env.frame_h;
	size[0] = tw * px;
	size[1] = th * px;
	/* Image centre relative to the opening's centre (image y runs down). */
	centre[0] = (0.5f * tw - 0.5f * (k_frame_opening[0] + k_frame_opening[2])) * px;
	centre[1] = lift * scale - (0.5f * th - 0.5f * (k_frame_opening[1] + k_frame_opening[3])) * px;
	centre[2] = -frame_dist;
	return true;
}

/* Draws the frame into its layer when the tint changed (once otherwise);
 * false when there is no frame layer this frame. */
static bool TieXrEnv_DrawFrame(AeronCommandBuffer* cmd, float tint) {
	if (!s_env.frame)
		return false;
	tint = TieXrEnv_Clamp01(tint);
	if (s_env.frame_drawn && fabsf(tint - s_env.frame_tint) <= 0.004f)
		return true;
	int w = 0, h = 0;
	AeronRenderTarget* target = Aeron_XrStereoPanelTarget(AERON_XR_PANEL_FRAME, &w, &h);
	if (!target)
		return false;
	if (!s_env.frame_list)
		s_env.frame_list = AeronDrawList_Create(8);
	if (!s_env.frame_list)
		return false;
	AeronDrawList_Begin(s_env.frame_list, target, w, h, AERON_DRAWLIST2D_CLEAR, NULL);
	AeronDrawList2DSprite sp;
	memset(&sp, 0, sizeof sp);
	sp.texture = s_env.frame;
	sp.src_u1 = 1.0f;
	sp.src_v1 = 1.0f;
	sp.dst_w = (float)s_env.frame_w;
	sp.dst_h = (float)s_env.frame_h;
	/* Premultiplied: fading the opacity fades the colour with it. */
	sp.tint[0] = sp.tint[1] = sp.tint[2] = tint;
	sp.tint[3] = tint;
	sp.blend = AERON_BLIT2D_BLEND_NONE; /* premultiplied texels as they are */
	sp.filter = AERON_BLIT2D_FILTER_NEAREST;
	AeronDrawList_AddSprite(s_env.frame_list, &sp);
	AeronDrawList_Render(s_env.frame_list, cmd);
	s_env.frame_drawn = true;
	s_env.frame_tint = tint;
	return true;
}

static void TieXrEnv_CommitFrame(void) {
	float q[3], size[2];
	if (!TieXrEnv_FrameQuad(q, size))
		return;
	AeronXrPanel p;
	memset(&p, 0, sizeof p);
	p.rect[2] = s_env.frame_w;
	p.rect[3] = s_env.frame_h;
	p.position[0] = q[0];
	p.position[1] = q[1];
	p.position[2] = q[2];
	p.orientation[3] = 1.0f;
	p.size[0] = size[0];
	p.size[1] = size[1];
	Aeron_XrStereoCommitPanelLayer(AERON_XR_PANEL_FRAME, &p, 1);
}

static bool TieXrEnv_RenderEyes(TieFlightRenderer* g, AeronCommandBuffer* cmd, const AeronXrStereoFrame* xr,
								float t_s, TieFlightXrEnvMode mode, bool ships, float fade, float gain, float reveal) {
	const bool fleet = mode == TIE_FLIGHT_XR_ENV_FLEET;
	const bool art = mode == TIE_FLIGHT_XR_ENV_TIEFIGHTER;
	AeronScene3D* scene = g->scene;
	/* The seated viewer at the origin looking along +y (TIE world), the
	 * head pose on top: OpenXR LOCAL (x right, y up, -z ahead) maps to
	 * (x, -z, y). Camera rows: eye x, y (down), z (ahead) in world. */
	AeronSceneCamera base;
	memset(&base, 0, sizeof base);
	base.ori[0] = 0.70710678f; /* w */
	base.ori[1] = 0.70710678f; /* x */
	base.h_half_rad = 0.8f;
	base.v_half_rad = 0.8f;
	base.near_z = 2.0f;
	if (art)
		TieXrArt_ViewPos(base.pos); /* the TIE Fighter scene: a little behind its centre */
	AeronSceneCamera eyes[2];
	for (int e = 0; e < 2; ++e) {
		TieFlightXr_EyeCamera(&base, &xr->views[e], &eyes[e]);
		eyes[e].viewport = (AeronRectI) { 0, 0, xr->width, xr->height };
	}

	AeronScene_SetTemporal(scene, &(AeronSceneTemporalDesc) { .mode = AERON_TEMPORAL_OFF });
	g->fsr_reset_pending = true;
	if (!AeronScene_Begin(scene, &eyes[0]))
		return false;
	AeronScene_SetPost(scene, &(AeronScenePostDesc) { 0 });
	AeronScene_SetMotionContext(scene, NULL, 0);
	static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	AeronScene_SetClearColor(scene, black);
	if (!AeronScene_SetMeshSampler(scene, g->opt_sampler))
		return false;
	s_env.star_half_px = fmaxf(0.75f, (float)xr->height / 1500.0f);
	AeronScene_SetPassHook(scene, AERON_SCENE_HOOK_BEFORE_OPAQUE, s_env.stars ? TieXrEnv_HookStars : NULL, g);
	AeronScene_SetPassHook(scene, AERON_SCENE_HOOK_AFTER_OPAQUE, NULL, NULL);
	AeronScene_SetPassHook(scene, AERON_SCENE_HOOK_AFTER_UPSCALE, NULL, NULL);

	TieXrEnv_SubmitSky(scene, fleet);
	if (fleet && ships)
		TieXrEnv_SubmitCraft(scene, t_s);
	if (art && s_env.art_up)
		TieXrArt_Submit(scene, ships, fade);
	/* The controllers or gloves (VR Options > Hands), in front of the
	 * virtual screen where they reach over it; none while the screen is
	 * away (the TIE Fighter scene's startup show). */
	if (reveal > 0.0f) {
		AeronXrPanel screen;
		memset(&screen, 0, sizeof screen);
		float d = 0.0f, w = 0.0f, h = 0.0f, off = 0.0f;
		float fq[3], fsize[2];
		bool have_screen = false;
		if (s_env.frame_on && TieXrEnv_FrameQuad(fq, fsize)) {
			/* The frame always takes in the picture behind it. */
			have_screen = true;
			memcpy(screen.position, fq, sizeof fq);
			screen.orientation[3] = 1.0f;
			screen.size[0] = fsize[0];
			screen.size[1] = fsize[1];
		} else if ((have_screen = Aeron_XrVirtualScreenGeometry(&d, &w, &h, &off) && w > 0.0f && h > 0.0f)) {
			screen.position[1] = off;
			screen.position[2] = -d;
			screen.orientation[3] = 1.0f;
			screen.size[0] = w;
			screen.size[1] = h;
		}
		TieVrHands_SubmitMenu(scene, cmd, &base, TieFlightXr_UnitsPerMeter(), xr, have_screen ? &screen : NULL);
	}

	if (!TieFlightPointLights_ConfigureScene(scene))
		return false;
	TieFlightPointLightParams point_params;
	TieFlightPointLights_GetParams(&point_params);
	float sun[3];
	TieXrEnv_Dir(k_sun_yaw_deg, k_sun_pitch_deg, sun);
	TieXrArtLighting art_light;
	if (art) {
		TieXrArt_Lighting(&art_light);
		memcpy(sun, art_light.key_dir, sizeof sun);
	}
	int render_w = xr->width, render_h = xr->height;
	AeronScene_RenderDims(scene, &render_w, &render_h);
	const TieFlightPbrAoParams ao = { .intensity = 0.0f, .power = 1.0f, .rt_w = (float)render_w,
									  .rt_h = (float)render_h };
	TieFlightPbrPassUniforms pbr;
	TieFlightPbr_BuildPassUniforms(&pbr, eyes[0].pos, sun, false, &ao, &point_params);
	/* The sun, a cool fill from the nebulae; everything rises out of the
	 * dark as the scene fades in. */
	const float sun_color[3] = { 1.15f, 1.08f, 0.98f };
	const float side[3] = { 0.070f, 0.075f, 0.110f };
	const float top[3] = { 0.085f, 0.075f, 0.120f };
	const float bottom[3] = { 0.040f, 0.045f, 0.065f };
	/* Plain diffuse fills: the planet's shine from below left, and a soft
	 * nebula glow from behind the viewer so craft against the sun are not
	 * black cut-outs. */
	float fill_dir[2][3];
	TieXrEnv_Dir(48.0f, -24.0f, fill_dir[0]);
	TieXrEnv_Dir(-160.0f, 12.0f, fill_dir[1]);
	static const float fill_col[2][3] = { { 0.10f, 0.12f, 0.17f }, { 0.20f, 0.15f, 0.24f } };
	memset(pbr.extra_dir, 0, sizeof pbr.extra_dir);
	memset(pbr.extra_col, 0, sizeof pbr.extra_col);
	for (int l = 0; l < 2; ++l) {
		for (int c = 0; c < 3; ++c) {
			pbr.extra_dir[l][c] = fill_dir[l][c];
			pbr.extra_col[l][c] = fill_col[l][c] * fade;
		}
	}
	for (int c = 0; c < 3; ++c) {
		pbr.sun_color[c] = sun_color[c] * fade;
		pbr.amb_pos_x[c] = pbr.amb_neg_x[c] = pbr.amb_pos_y[c] = pbr.amb_neg_y[c] = side[c] * fade;
		pbr.amb_pos_z[c] = top[c] * fade;
		pbr.amb_neg_z[c] = bottom[c] * fade;
	}
	if (art) {
		/* The TIE Fighter cover's light: the key from the upper right, the
		 * explosion's orange fill from ahead below, a violet ambient. */
		memset(pbr.extra_dir, 0, sizeof pbr.extra_dir);
		memset(pbr.extra_col, 0, sizeof pbr.extra_col);
		for (int c = 0; c < 3; ++c) {
			pbr.sun_color[c] = art_light.key_color[c] * fade;
			pbr.extra_dir[0][c] = art_light.fill_dir[c];
			pbr.extra_col[0][c] = art_light.fill_color[c] * fade;
			pbr.amb_pos_x[c] = pbr.amb_neg_x[c] = pbr.amb_pos_y[c] = pbr.amb_neg_y[c] =
				art_light.ambient_sides[c] * fade;
			pbr.amb_pos_z[c] = art_light.ambient_top[c] * fade;
			pbr.amb_neg_z[c] = art_light.ambient_bottom[c] * fade;
		}
	}
	AeronScene_SetFrameUniformData(scene, AERON_SHADER_STAGE_FRAGMENT, 1, &pbr, sizeof pbr);

	TIE_GPU_PUSH(cmd, "VR menu scene, left eye");
	bool ok = AeronScene_Render(scene, cmd) != 0 &&
			  TieXrEnv_OutputEye(g, cmd, 0, xr->eye_targets[0], xr->format, gain);
	TIE_GPU_POP(cmd);
	if (ok) {
		TieVrHands_FinishRightEye(cmd); /* its hand mask, filled meanwhile */
		TIE_GPU_PUSH(cmd, "VR menu scene, right eye");
		ok = AeronScene_SetCamera(scene, &eyes[1]) && AeronScene_Render(scene, cmd) &&
			 TieXrEnv_OutputEye(g, cmd, 1, xr->eye_targets[1], xr->format, gain);
		TIE_GPU_POP(cmd);
	}
	/* Leave nothing of ours on the shared scene. */
	AeronScene_SetPassHook(scene, AERON_SCENE_HOOK_BEFORE_OPAQUE, NULL, NULL);
	return ok;
}

bool TieFlightXrEnv_Render(TieFlightRenderer* g, const TieFlightAssetSource* assets, uint64_t time_us, bool ships,
						   bool dimmed) {
	const TieFlightXrEnvMode mode = TieFlightXrEnv_Mode();
	if (mode != s_env.last_mode) {
		/* Another scene (VR options): the TIE Fighter scene's fight starts
		 * over when it comes back, and everything fades in again. */
		TieXrArt_Restart();
		s_env.art_up = false;
		s_env.ready = false;
		s_env.last_mode = mode;
	}
	const bool art = mode == TIE_FLIGHT_XR_ENV_TIEFIGHTER;
	if (!art)
		TieXrEnv_ScreenNormal();
	AeronXrStereoFrame xr;
	if (mode == TIE_FLIGHT_XR_ENV_OFF || !g || !g->scene || !Aeron_XrStereoBegin(&xr) || xr.width <= 0 ||
		xr.height <= 0 || !xr.eye_targets[0] || !xr.eye_targets[1]) {
		s_env.rendering = false;
		TieXrEnv_ScreenNormal();
		return false;
	}
	if (!TieFlightRenderer_EnsureOutputSize(g, xr.width, xr.height))
		return false;

	/* Clock: a restart (first frame, or back from flight) fades in again. */
	const bool restart = !s_env.rendering || time_us < s_env.last_us || time_us - s_env.last_us > 500000u;
	const float dt = restart ? 0.0f : (float)(time_us - s_env.last_us) * 1e-6f;
	s_env.last_us = time_us;
	if (restart) {
		s_env.ready = false;
		s_env.art_up = false;
		s_env.dim = dimmed ? 1.0f : 0.0f;
	}
	s_env.rendering = true;
	const float target_dim = dimmed ? 1.0f : 0.0f;
	const float step = dt / TIE_XR_ENV_DIM_S;
	s_env.dim = s_env.dim < target_dim ? fminf(target_dim, s_env.dim + step) : fmaxf(target_dim, s_env.dim - step);

	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd)
		return false;
	bool ok = TieXrEnv_EnsureTextures(cmd);
	TieXrEnv_LoadFrame(cmd);
	if (!dimmed)
		s_env.menu_seen = true;
	s_env.frame_on = s_env.frame && s_env.menu_seen;
	if (ok && (!s_env.stars || s_env.stars_format != g->rt_format)) {
		if (s_env.stars)
			TieFlightStars_Destroy(s_env.stars);
		s_env.stars = TieFlightStars_Create(g->rt_format);
		s_env.stars_format = g->rt_format;
		if (s_env.stars && !TieFlightStars_PrepareCustom(s_env.stars, cmd, (const float(*)[4])s_env.star_dirs,
														 TIE_XR_ENV_STAR_COUNT)) {
			TieFlightStars_Destroy(s_env.stars);
			s_env.stars = NULL;
		}
	}
	const bool fleet = mode == TIE_FLIGHT_XR_ENV_FLEET;
	bool models_done = true;
	if (ok && fleet && ships)
		models_done = TieXrEnv_LoadNextModel(cmd, assets);
	/* The TIE Fighter scene loads behind the startup films (stars only
	 * meanwhile, as in OpenXWA-VR) and comes up with the first menu. */
	if (ok && art)
		models_done = TieXrArt_Prepare(cmd, assets, ships);
	if (models_done && !s_env.ready) {
		s_env.ready = true;
		s_env.ready_us = time_us;
		if (fleet && ships && !s_env.logged_models) {
			s_env.logged_models = true;
			Aeron_LogInfo("tie.vr", "menu scene ready: star destroyer %s, TIE fighter %s, TIE interceptor %s",
						  s_env.models[0].mesh ? "yes" : "no", s_env.models[1].mesh ? "yes" : "no",
						  s_env.models[2].mesh ? "yes" : "no");
		}
	}
	if (!art && mode != TIE_FLIGHT_XR_ENV_OFF && s_env.menu_seen)
		TieXrArt_SkipStartupShow();
	if (art && s_env.ready && s_env.menu_seen && !s_env.art_up) {
		s_env.art_up = true;
		s_env.art_up_us = time_us;
	}
	float fade;
	if (art) {
		fade = s_env.art_up ? TieXrEnv_Smooth(0.0f, 1.0f,
											  (float)(time_us - s_env.art_up_us) * 1e-6f /
												  fmaxf(TieXrEnv_EnvFloat("XWA_XR_ENV_FADE_SECONDS", 1.5f), 0.01f))
							: 0.0f;
	} else {
		fade = s_env.ready ? TieXrEnv_Smooth(0.0f, 1.0f,
											 (float)(time_us - s_env.ready_us) * 1e-6f / TIE_XR_ENV_FADE_S)
						   : 0.0f;
	}
	/* XWA_XR_ENV_BRIGHTNESS is the displayed brightness (applied in linear
	 * light); films and loading ease down to XWA_XR_ENV_FILM_DIM of it. The
	 * TIE Fighter scene's stars stay while it loads (its craft and sprites
	 * fade in). */
	const float brightness = TieXrEnv_Clamp01(TieXrEnv_EnvFloat("XWA_XR_ENV_BRIGHTNESS", 0.8f));
	const float film = TieXrEnv_Clamp01(TieXrEnv_EnvFloat("XWA_XR_ENV_FILM_DIM", 0.5f));
	const float dim = s_env.dim * s_env.dim * (3.0f - 2.0f * s_env.dim);
	const float gain = powf(brightness * (1.0f - dim * (1.0f - film)) * (art ? 1.0f : fade), 2.2f);

	/* TIE_XR_ENV_TIME=<s> freezes the craft at that moment (tests). */
	const char* fixed_time = getenv("TIE_XR_ENV_TIME");
	const float t_s = fixed_time && fixed_time[0] ? (float)atof(fixed_time) : (float)((double)time_us * 1e-6);
	/* The TIE Fighter scene's fight, from its first frame up; the startup
	 * show keeps the screen away (and the controllers on A only) until the
	 * fight has come to the cover's moment. */
	float reveal = 1.0f;
	if (art && s_env.art_up) {
		if (ships)
			TieXrArt_Advance(dt, xr.views[0].orientation);
		reveal = TieXrArt_ScreenReveal();
		s_env.screen_control = true;
		Aeron_XrSetScreenFade(reveal);
		Aeron_XrSetInputBlocked(reveal <= 0.0f);
	}
	if (ok)
		ok = TieXrEnv_RenderEyes(g, cmd, &xr, t_s, mode, ships, fade, gain, reveal);
	/* The pointer beam over the screen; none during films and loading. */
	AeronXrPanel beam;
	const bool beam_on = ok && !dimmed && reveal > 0.0f && xr.beam_target &&
						 TieFlightXrBeam_Frame(cmd, xr.beam_target, xr.beam_width, xr.beam_height, xr.views, &beam);
	if (!ok) {
		Aeron_CancelCommandBuffer(cmd);
		return false;
	}
	/* The frame dims and fades with the scene (and comes in with the screen). */
	const float frame_gain = art ? powf(brightness * (1.0f - dim * (1.0f - film)) * fade, 2.2f) : gain;
	const bool frame_ok = s_env.frame_on && reveal > 0.0f &&
						  TieXrEnv_DrawFrame(cmd, powf(frame_gain, 1.0f / 2.2f) * reveal);
	if (!Aeron_SubmitCommandBuffer(cmd)) {
		Aeron_RequestFatalRendererError("VR menu scene command-buffer submission");
		return false;
	}
	/* The eyes with the virtual screen in front of them; while the startup
	 * show keeps the screen away, the eyes alone (a screen faded to 0 would
	 * still be an opaque quad). */
	if (reveal > 0.0f)
		Aeron_XrStereoCommitBackdrop();
	else
		Aeron_XrStereoCommitHuds(NULL, NULL);
	if (frame_ok)
		TieXrEnv_CommitFrame();
	TieVrHands_CommitMenu();
	if (beam_on)
		Aeron_XrStereoCommitBeams(&beam, 1);
	return true;
}

void TieFlightXrEnv_Shutdown(void) {
	TieFlightXrEnv_ReleaseShips();
	TieXrArt_Shutdown();
	if (s_env.planet)
		Aeron_DestroyTexture(s_env.planet);
	if (s_env.nebula)
		Aeron_DestroyTexture(s_env.nebula);
	if (s_env.glow)
		Aeron_DestroyTexture(s_env.glow);
	if (s_env.stars)
		TieFlightStars_Destroy(s_env.stars);
	if (s_env.chain)
		AeronScenePresentChain_Destroy(s_env.chain);
	if (s_env.frame)
		Aeron_DestroyTexture(s_env.frame);
	if (s_env.frame_list)
		AeronDrawList_Destroy(s_env.frame_list);
	TieFlightXrBeam_Shutdown();
	memset(&s_env, 0, sizeof s_env);
}

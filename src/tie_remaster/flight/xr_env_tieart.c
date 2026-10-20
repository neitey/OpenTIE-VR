/* The "TIE Fighter" menu scene; see xr_env_tieart.h.
 *
 * Ported from OpenXWA-VR (src/xwa_remaster/vr_env.c, ENV_TIEART) as closely as
 * the two games allow: the same layout, attitudes, flight paths, timeline,
 * salvos, explosion and helmet. What differs:
 *   - craft: TIE Fighter's own flight models (T/I, T/F, T/B, X-W, A-W) through
 *     the selected flight asset source instead of XWA's OPTs;
 *   - explosion and fire: TIE Fighter's own explosion animations (XACT
 *     XPLO05C and BOOM_N1A from RES640/SPECIES.LFD) instead of XWA's
 *     Resdata/Explosions.dat groups 2005/2003; the laser bolts, nebula clouds
 *     and glow are generated as in XWA;
 *   - sounds: TIE Fighter's flight effects (RESOURCE/SFXBLAST.LFD and
 *     SFXDOE.LFD, Creative Voice) instead of XWA's wave\ files, scaled by the
 *     in-flight sound effects volume;
 *   - the startup show as XWA's: the screen away while the fight plays (A
 *     skips to its end), then the title (the logo, vr/logo.rgba, large where
 *     the screen will be, "Press (A) to Continue" in OpenTIE's menu font)
 *     until A brings the screen in; without a logo the screen comes in by
 *     itself. The title's music (XWA's debriefing theme) is left to TIE's
 *     own frontend music. */

#include "tie_remaster/flight/xr_env_tieart.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#include "aeron/aeron.h"
#include "aeron/asset/opt_model.h"
#include "aeron/audio.h"
#include "aeron/log.h"
#include "aeron/scene/billboard.h"
#include "aeron/scene/draw_list2d.h"
#include "aeron/scene/font_atlas.h"
#include "aeron/scene/text2d.h"
#include "aeron/scene/scene3d.h"
#include "aeron/vfs.h"
#include "tie_formats/xact.h"
#include "tie_runtime/flight_assets/source.h"
#include "tie_runtime/runtime/inflight_state.h"
#include "tie_runtime/species_id.h"

/* xr_env.c: one craft model from the player's flight models; `bounds_m`
 * (min x y z, max x y z) when not NULL. */
struct AeronSceneMesh* TieFlightXrEnv_LoadModel(AeronCommandBuffer* cmd,
												const struct TieFlightAssetSource* source, uint16_t species,
												float bounds_m[6]);

#define ART_PI 3.14159265358979f
#define ART_K AERON_OPT_UNITS_PER_METER

/* ---- the scene ----------------------------------------------------------
 * The cover of TIE Fighter (its logo left out): a TIE Interceptor coming at
 * the viewer in front of a huge explosion in which an X-wing breaks up, a
 * second Interceptor upper left and a TIE fighter on the left firing green,
 * a TIE Bomber upper right firing, a small TIE fighter on the right, Darth
 * Vader's helmet looming behind it all over purple clouds. Placed and
 * oriented as on the cover (view-relative), pushed out from the middle just
 * far enough to clear the menu screen; the helmet is the backdrop the screen
 * stands in front of. Directions: yaw (degrees, + right of the screen), pitch
 * (+ up), distance in metres from the viewer; with `view` the nose and up are
 * as the viewer sees the craft (x right, y away along the line of sight to
 * it, z up). */
enum { ART_TI, ART_TF, ART_TB, ART_XW, ART_AW, ART_MODEL_COUNT };

static const uint16_t k_art_species[ART_MODEL_COUNT] = {
	TIE_SPECIES_TIE_INTERCEPTOR, TIE_SPECIES_TIE_FIGHTER, TIE_SPECIES_TIE_BOMBER,
	TIE_SPECIES_X_WING,          TIE_SPECIES_A_WING,
};

typedef struct ArtShip {
	int model;
	float yaw, pitch, dist;
	float nose[3], up[3];
	int view;
} ArtShip;

static const ArtShip k_ships[] = {
	/* 0: the big Interceptor below the screen, coming at us, cockpit window
	 * toward us, seen a little from above, turned slightly to our left. */
	{ ART_TI, -8.0f, -40.0f, 12.0f, { -0.12f, -0.97f, 0.20f }, { 0.10f, -0.25f, 0.96f }, 1 },
	/* 1: Interceptor upper left: three-quarters from the front, wings rising
	 * to the right, firing to the upper left. */
	{ ART_TI, -32.0f, 19.0f, 40.0f, { -0.60f, -0.45f, 0.65f }, { -0.31f, 0.88f, 0.33f }, 1 },
	/* 2: TIE fighter on the left: window to the front left, firing left. */
	{ ART_TF, -31.0f, -4.0f, 55.0f, { -0.75f, -0.55f, 0.10f }, { -0.08f, 0.27f, 0.89f }, 1 },
	/* 3: TIE Bomber upper right: from the front above, rolled, firing to
	 * the upper right. */
	{ ART_TB, 36.0f, 16.0f, 28.0f, { 0.30f, -0.60f, 0.75f }, { -0.45f, 0.60f, 0.66f }, 1 },
	/* 4: the small TIE fighter right of the middle, coming at us, its
	 * cockpit window toward us. */
	{ ART_TF, 28.0f, 2.0f, 90.0f, { 0.28f, -0.94f, -0.18f }, { 0.05f, -0.17f, 0.98f }, 1 },
	/* 5: the X-wing breaking up in the explosion, nose down to the right. */
	{ ART_XW, 18.0f, -32.0f, 45.0f, { 0.55f, -0.35f, -0.75f }, { 0.30f, -0.60f, 0.50f }, 1 },
	/* 6, 7: not on the cover: in the animation an X-wing runs from the big
	 * Interceptor and an A-wing from the TIE fighter on the left, both gone
	 * from the view by the cover's moment (see the runners). */
	{ ART_XW, 80.0f, 30.0f, 300.0f, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, 0 },
	{ ART_AW, -80.0f, 30.0f, 300.0f, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, 0 },
	/* 8, 9: not on the cover: two A-wings screaming past the viewer early in
	 * the animation (see the passers). */
	{ ART_AW, 80.0f, -30.0f, 300.0f, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, 0 },
	{ ART_AW, -80.0f, -30.0f, 300.0f, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, 0 },
};
#define ART_SHIPS ((int)(sizeof k_ships / sizeof k_ships[0]))
#define ART_XWING 5 /* the X-wing shot down on the cover */

static int art_is_tie(int ship) {
	const int m = k_ships[ship].model;
	return m == ART_TI || m == ART_TF || m == ART_TB;
}

/* The passers: ships 8 and 9 fly straight in from far ahead, cross each
 * other's course PASS_CROSS_M ahead of the viewer and pass it at PASS_AT
 * seconds, one on the left, one on the right (viewer's x right, y ahead,
 * z up; metres at the moment they pass). */
#define ART_FIRST_PASSER 8
#define ART_PASSERS 2
#define ART_PASS_AT 3.0f
#define ART_PASS_SPEED 140.0f
#define ART_PASS_CROSS_M 35.0f
static const float k_pass[ART_PASSERS][2] = { { -7.0f, 3.0f }, { 7.5f, -2.0f } };

#define ART_RUNNERS 2
#define ART_CHASE_SHIP 0 /* the Interceptor after the jinking X-wing */
#define ART_FIRST_RUNNER 6
/* Who runs from whom, and how it gets away: the break comes where it is
 * break_m from the viewer (0: never by distance) or end_s before the
 * cover's moment, whichever is first; a barrel roll of roll_s (0: none),
 * then a pull at pull_w rad/s toward `away` (viewer's x right, y ahead,
 * z up). */
static const struct {
	int ship, chaser;
	float lead_m, break_m, end_s, roll_s, pull_w;
	float away[3];
	int weave; /* jinks instead (see art_weave_pose) */
} k_runners[ART_RUNNERS] = {
	/* The X-wing from the big Interceptor: jinking left and right three
	 * times ahead of it, the Interceptor correcting after it, until it is
	 * hit and disappears in the great explosion. */
	{ 6, 0, 50.0f, 0.0f, 0.0f, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 1 },
	/* The A-wing from the TIE fighter on the left: it holds on to the end
	 * of the TIE's path (where the TIE will be on the cover), then curves
	 * away out past the viewer's left. */
	{ 7, 2, 50.0f, 0.0f, 0.02f, 0.0f, 0.8f, { -1.0f, -0.4f, 0.3f }, 0 },
};

/* The explosion's centre behind the big Interceptor: yaw, pitch, metres,
 * diameter in metres. */
static const float k_blast[4] = { -6.0f, -33.0f, 70.0f, 58.0f };

/* Frozen laser salvos: shooter -> target (ship indices) from the shooter's
 * guns, as spans of the shooter-target distance along its nose. */
typedef enum ArtGuns {
	ART_GUNS_XWING,       /* four wingtip cannons, red */
	ART_GUNS_TIE,         /* two chin guns, green */
	ART_GUNS_AWING,       /* two wingtip cannons, red */
	ART_GUNS_INTERCEPTOR, /* TIE Interceptor: two wingtip guns, green */
} ArtGuns;

typedef struct ArtSalvo {
	int shooter, target;
	ArtGuns guns;
	float spans[2][2];
	int span_count;
} ArtSalvo;

static const ArtSalvo k_salvos[] = {
	/* The long green streaks of the Interceptor upper left. */
	{ 1, 0, ART_GUNS_INTERCEPTOR, { { 0.05f, 1.10f } }, 1 },
	/* The TIE fighter's fire to the left. */
	{ 2, 0, ART_GUNS_TIE, { { 0.05f, 0.75f } }, 1 },
	/* The bomber's streak to the upper right. */
	{ 3, 0, ART_GUNS_TIE, { { 0.10f, 1.60f } }, 1 },
};
#define ART_SALVOS ((int)(sizeof k_salvos / sizeof k_salvos[0]))

/* The animation: the seconds before the cover's moment, which then holds. */
#define ART_ANIM_SECONDS 7.0f
#define ART_BOOM_AT (ART_ANIM_SECONDS - 1.4f)
#define ART_SALVO_SECONDS 0.5f
#define ART_BOLT_SPEED 320.0f /* m/s */
#define ART_BOLTS 64
#define ART_REVEAL_SECONDS 0.6f
/* The X-wing (ship 5) is hit at these times, then blows up at BOOM_AT. */
static const float k_hits[3] = { ART_ANIM_SECONDS - 2.6f, ART_ANIM_SECONDS - 2.15f,
								 ART_ANIM_SECONDS - 1.75f };
/* The hunter's shots that miss before those. */
static const float k_misses[2] = { ART_ANIM_SECONDS - 3.6f, ART_ANIM_SECONDS - 3.1f };

/* ---- textures ---------------------------------------------------------- */

enum {
	ART_TEX_BEAM,   /* generated: a bolt's profile (additive) */
	ART_TEX_NEBULA, /* generated: the purple clouds */
	ART_TEX_GLOW,   /* generated: a soft round glow (the blast's flash) */
	ART_TEX_VADER,  /* vr/vader.rgba */
	ART_TEX_LOGO,   /* vr/logo.rgba: above the screen, and the startup title */
	ART_TEX_BLAST,  /* the explosion at its height (XPLO05C) */
	ART_TEX_FIRE,   /* a cloud of fire (BOOM_N1A) */
	ART_TEX_BOOM,   /* the explosion growing (XPLO05C's first frames) */
	ART_TEX_BOOM_LAST = ART_TEX_BOOM + 3,
	ART_TEX_COUNT
};
#define ART_BOOM_FRAMES (ART_TEX_BOOM_LAST - ART_TEX_BOOM + 1)

/* TIE Fighter's explosion animations (RES640/SPECIES.LFD XACT entries): the
 * big fighter explosion (its frames 0..3 grow into its height, frame 3) and
 * the fire cloud of the big starship explosion. */
#define ART_XACT_BOOM "XPLO05C"
#define ART_XACT_BLAST_FRAME 3
#define ART_XACT_FIRE "BOOM_N1A"
#define ART_XACT_FIRE_FRAME 4
/* TIE Fighter's explosion frames are cropped to the fire; XWA's sprites have
 * a clear margin round it. The quads are drawn this much smaller so the fire
 * comes out as large as XWA's. */
#define ART_XACT_FILL 0.7f

/* ---- sounds ------------------------------------------------------------ */

enum {
	ES_TIE_LASER,
	ES_REBEL_LASER,
	ES_TIE_ENGINE,
	ES_TIE_FLYBY,
	ES_BOOM,
	ES_HIT0,
	ES_HIT1,
	ES_HIT2,
	ES_XW_FLYBY,
	ES_AW_FLYBY,
	ES_COUNT
};

/* TIE Fighter's own: the flight's effect bank (RESOURCE/SFXBLAST.LFD; record
 * n is effect 4 + n) and its engine loops (RESOURCE/SFXDOE.LFD). The Imperial
 * laser (effect 6), the rebel laser (4), the TIE engine loop, the TIE fly-by
 * (42), the big explosion (18), hits, the X-wing (44) and A-wing (46)
 * fly-bys. */
static const struct {
	const char* bank;
	const char* name;
} k_es_source[ES_COUNT] = {
	{ "RESOURCE/SFXBLAST.LFD", "tl-sht-1" }, { "RESOURCE/SFXBLAST.LFD", "reb-ls-2" },
	{ "RESOURCE/SFXDOE.LFD", "tienglp" },    { "RESOURCE/SFXBLAST.LFD", "ti-lr-1" },
	{ "RESOURCE/SFXBLAST.LFD", "ex-big-2" }, { "RESOURCE/SFXBLAST.LFD", "hit-80" },
	{ "RESOURCE/SFXBLAST.LFD", "hit-4" },    { "RESOURCE/SFXBLAST.LFD", "ht-sys-2" },
	{ "RESOURCE/SFXBLAST.LFD", "xw-nf-5" },  { "RESOURCE/SFXBLAST.LFD", "aw-nf-3" },
};

/* ---- state ------------------------------------------------------------- */

typedef struct ArtRunner {
	int ok, chaser;
	float lead; /* seconds ahead of the chaser on its path */
	float tb;   /* the break: barrel roll, then the pull away */
	float v, roll_time, helix;
	float pb[3], nb[3], ub[3]; /* at the break (world units) */
	float pull_up[3];          /* the pull's turn centre direction */
	float pull_w, pull_bank;
	int flyby_done;
} ArtRunner;

static struct {
	struct {
		struct AeronSceneMesh* mesh;
		bool tried;
		float bounds[6];
	} models[ART_MODEL_COUNT];
	AeronTexture* tex[ART_TEX_COUNT];
	int tex_w[ART_TEX_COUNT], tex_h[ART_TEX_COUNT];
	bool generated_tried, vader_tried, logo_tried, sprites_tried, sounds_tried;
	bool logged;
	float view_pos[3];
	/* The fight. */
	float anim_t;
	int anim_paths;
	struct {
		float v;     /* speed, world units per second */
		float w;     /* turn rate into the pose, rad/s */
		float alpha; /* the last turn's angle (pulling into the pose) */
		float beta;  /* an earlier turn's angle (0: none) */
		float b1;    /* that turn's bank from level */
	} anim_fl[8];
	struct {
		float p[3], d[3], life;
		int green, alive;
	} bolts[ART_BOLTS];
	float next_shot[8];
	ArtRunner runner[ART_RUNNERS];
	int hunter; /* the TIE that shoots the X-wing down (-1: none) */
	float transform[ART_SHIPS][16];
	bool hidden[ART_SHIPS];
	bool posed;
	/* TIE_XR_ENV_TIME: the moment played to. */
	bool fixed;
	float fixed_t;
	bool mute;
	/* The startup show, once per run: the fight with the screen away, then
	 * (with a logo) the title until A, then the screen coming in. */
	bool title_done;
	bool title_started;
	bool title_go;	 /* A pressed: the screen is coming in */
	float go_t;		 /* seconds since */
	float shown_t;	 /* seconds the title has been up */
	bool pressed;	 /* an A press this frame not taken by the title */
	TieXrArtScreen screen;
	bool have_screen;
	/* Sounds. */
	AeronClip clip[ES_COUNT];
	AeronVoice engine[8];
	int flyby_done[8];
	float prev_dist[8];
	float right[3]; /* the head's right ear, scene axes */
	bool has_head;
} s_art = { .hunter = -1 };

static float s_weave_acc_max; /* the weave's hardest pull, 1/s^2 */

static float art_env(const char* name, float fallback) {
	const char* v = getenv(name);
	return v && v[0] ? (float)atof(v) : fallback;
}

/* ---- maths --------------------------------------------------------------- */

static float art_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static float art_len(const float a[3]) { return sqrtf(art_dot(a, a)); }

static int art_norm(float v[3]) {
	const float l = art_len(v);
	if (!(l > 1e-6f))
		return 0;
	v[0] /= l;
	v[1] /= l;
	v[2] /= l;
	return 1;
}

static void art_cross(const float a[3], const float b[3], float out[3]) {
	const float r[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
	memcpy(out, r, sizeof r);
}

/* Rotate v about the unit axis by `angle` radians (Rodrigues). */
static void art_rotate(float v[3], const float axis[3], float angle) {
	const float c = cosf(angle), s = sinf(angle), d = art_dot(axis, v);
	float x[3];
	art_cross(axis, v, x);
	for (int a = 0; a < 3; ++a)
		v[a] = v[a] * c + x[a] * s + axis[a] * d * (1.0f - c);
}

/* Unit direction of yaw/pitch degrees (yaw 0 = forward +y, + = right). */
static void art_dir(float yaw, float pitch, float out[3]) {
	const float y = yaw * ART_PI / 180.0f, p = pitch * ART_PI / 180.0f;
	out[0] = sinf(y) * cosf(p);
	out[1] = cosf(y) * cosf(p);
	out[2] = sinf(p);
}

static float art_smooth(float x) {
	x = fminf(fmaxf(x, 0.0f), 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

/* Smootherstep: its pull (second derivative) has no jumps either, so the
 * lean it gives never snaps. */
static float art_smoother(float x) {
	x = fminf(fmaxf(x, 0.0f), 1.0f);
	return x * x * x * (x * (6.0f * x - 15.0f) + 10.0f);
}

/* Model -> scene transform from a nose and an up direction: the model's
 * forward (-y) onto `nose`, its z onto the up made orthogonal, x onto
 * nose x up (OPT axes: x right, -y ahead, z up, metres). */
static void art_pose_matrix(const float nose_in[3], const float up_in[3], const float pos[3], float m[16]) {
	float n[3] = { nose_in[0], nose_in[1], nose_in[2] }, r[3], u[3];
	art_norm(n);
	art_cross(n, up_in, r);
	art_norm(r);
	art_cross(r, n, u);
	for (int i = 0; i < 3; ++i) {
		m[i * 4 + 0] = ART_K * r[i];
		m[i * 4 + 1] = -ART_K * n[i];
		m[i * 4 + 2] = ART_K * u[i];
		m[i * 4 + 3] = pos[i];
	}
	m[12] = m[13] = m[14] = 0.0f;
	m[15] = 1.0f;
}

/* World position (units) of ship i on the cover. */
static void art_ship_pos(int i, float out[3]) {
	const ArtShip* e = &k_ships[i];
	art_dir(e->yaw, e->pitch, out);
	for (int a = 0; a < 3; ++a)
		out[a] *= e->dist * ART_K;
}

/* Orthonormal nose / right / up of ship i on the cover. */
static void art_ship_basis(int i, float n[3], float r[3], float u[3]) {
	const ArtShip* e = &k_ships[i];
	float up[3];
	memcpy(n, e->nose, 3 * sizeof(float));
	memcpy(up, e->up, sizeof up);
	if (e->view) {
		/* From the frame of the line of sight to the craft. */
		static const float z[3] = { 0.0f, 0.0f, 1.0f };
		float f[3], vr[3], vu[3];
		art_dir(e->yaw, e->pitch, f);
		art_cross(f, z, vr);
		art_norm(vr);
		art_cross(vr, f, vu);
		for (int a = 0; a < 3; ++a) {
			n[a] = vr[a] * e->nose[0] + f[a] * e->nose[1] + vu[a] * e->nose[2];
			up[a] = vr[a] * e->up[0] + f[a] * e->up[1] + vu[a] * e->up[2];
		}
	}
	art_norm(n);
	art_cross(n, up, r);
	art_norm(r);
	art_cross(r, n, u);
}

/* The screen's cone (degrees): craft in it are hidden behind the screen. */
static int art_behind_screen(const float p[3]) {
	const float yaw = atan2f(p[0], p[1]) * 180.0f / ART_PI;
	const float pitch = atan2f(p[2], sqrtf(p[0] * p[0] + p[1] * p[1])) * 180.0f / ART_PI;
	return p[1] > 0.0f && fabsf(yaw) < 23.0f && pitch > -21.0f && pitch < 30.0f;
}

/* ---- textures ------------------------------------------------------------ */

static float art_srgb_to_linear(float c) {
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float art_linear_to_srgb(float c) {
	return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static bool art_upload(AeronCommandBuffer* cmd, int slot, const uint8_t* px, int w, int h,
					   AeronTextureFormat format, const char* name) {
	AeronTexture* t = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width = w,
		.height = h,
		.mip_count = 1,
		.format = format,
		.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = name,
	});
	if (!t || !Aeron_UploadTextureDataCmd(cmd, &(AeronTextureUploadDesc) {
												   .texture = t,
												   .width = w,
												   .height = h,
												   .raw_data = px,
												   .raw_size = (uint32_t)((size_t)w * (size_t)h * 4u),
											   })) {
		if (t)
			Aeron_DestroyTexture(t);
		return false;
	}
	s_art.tex[slot] = t;
	s_art.tex_w[slot] = w;
	s_art.tex_h[slot] = h;
	return true;
}

/* Straight-alpha sRGB RGBA -> premultiplied (in linear light) sRGB texture,
 * the billboards' PMA convention. */
static bool art_make_texture(AeronCommandBuffer* cmd, int slot, const uint8_t* rgba, int w, int h) {
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
		return false;
	uint8_t* px = (uint8_t*)malloc((size_t)w * (size_t)h * 4u);
	if (!px)
		return false;
	float lut[256];
	for (int i = 0; i < 256; ++i)
		lut[i] = art_srgb_to_linear((float)i / 255.0f);
	for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) {
		const float a = (float)rgba[i * 4 + 3] / 255.0f;
		for (int c = 0; c < 3; ++c)
			px[i * 4 + c] = (uint8_t)(art_linear_to_srgb(lut[rgba[i * 4 + c]] * a) * 255.0f + 0.5f);
		px[i * 4 + 3] = rgba[i * 4 + 3];
	}
	const bool ok = art_upload(cmd, slot, px, w, h, AERON_TEXTURE_FORMAT_RGBA8_SRGB, "tie.vr_env.art.sprite");
	free(px);
	return ok;
}

/* The beam profile: a bright core inside a soft glow across V, tapered at
 * both ends along U (additive; colour comes from the tint). */
static void art_make_beam(AeronCommandBuffer* cmd) {
	enum { BW = 64, BH = 32 };
	uint8_t px[BW * BH * 4];
	for (int y = 0; y < BH; ++y) {
		const float v = fabsf(((float)y + 0.5f) / (float)BH * 2.0f - 1.0f);
		for (int x = 0; x < BW; ++x) {
			float along = fminf(((float)x + 0.5f), ((float)BW - (float)x - 0.5f)) / 8.0f;
			along = fminf(along, 1.0f);
			const float core = expf(-v * v * 30.0f), glow = 0.45f * expf(-v * v * 4.0f);
			const float a = fminf(core + glow, 1.0f) * along;
			uint8_t* p = &px[(y * BW + x) * 4];
			p[0] = p[1] = p[2] = (uint8_t)(a * 255.0f + 0.5f);
			p[3] = 255;
		}
	}
	(void)art_upload(cmd, ART_TEX_BEAM, px, BW, BH, AERON_TEXTURE_FORMAT_RGBA8_UNORM, "tie.vr_env.art.beam");
}

/* A soft round glow (additive; colour from the tint). */
static void art_make_glow(AeronCommandBuffer* cmd) {
	enum { G = 64 };
	uint8_t px[G * G * 4];
	for (int y = 0; y < G; ++y) {
		for (int x = 0; x < G; ++x) {
			const float dx = ((float)x + 0.5f) / (float)G * 2.0f - 1.0f,
						dy = ((float)y + 0.5f) / (float)G * 2.0f - 1.0f;
			const float r2 = dx * dx + dy * dy;
			const float a =
				r2 >= 1.0f ? 0.0f : fminf(1.0f, expf(-r2 * 9.0f) + 0.35f * expf(-r2 * 2.5f)) * (1.0f - r2);
			uint8_t* p = &px[(y * G + x) * 4];
			p[0] = p[1] = p[2] = (uint8_t)(a * 255.0f + 0.5f);
			p[3] = 255;
		}
	}
	(void)art_upload(cmd, ART_TEX_GLOW, px, G, G, AERON_TEXTURE_FORMAT_RGBA8_UNORM, "tie.vr_env.art.glow");
}

/* Value noise on a wrapping lattice, for the clouds. */
static float art_hash(int x, int y, int seed) {
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return (float)((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
}

static float art_noise(float x, float y, int seed) {
	const int xi = (int)floorf(x), yi = (int)floorf(y);
	float fx = x - (float)xi, fy = y - (float)yi;
	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);
	const float a = art_hash(xi, yi, seed), b = art_hash(xi + 1, yi, seed);
	const float c = art_hash(xi, yi + 1, seed), d = art_hash(xi + 1, yi + 1, seed);
	return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static float art_fbm(float x, float y, int seed, int octaves) {
	float sum = 0.0f, amp = 0.5f, norm = 0.0f;
	for (int o = 0; o < octaves; ++o) {
		sum += amp * art_noise(x, y, seed + o * 17);
		norm += amp;
		x *= 2.03f;
		y *= 2.03f;
		amp *= 0.5f;
	}
	return sum / norm;
}

/* The clouds: a long magenta-violet cloud, brighter pink filaments in its
 * core, ragged dark lanes, fading to nothing at the edges (XWA's box-art
 * nebula, tinted purple-blue where it is drawn). */
static void art_make_nebula(AeronCommandBuffer* cmd) {
	enum { NW = 512, NH = 256 };
	uint8_t* px = (uint8_t*)malloc((size_t)NW * NH * 4u);
	if (!px)
		return;
	for (int y = 0; y < NH; ++y) {
		for (int x = 0; x < NW; ++x) {
			const float u = ((float)x + 0.5f) / (float)NW * 2.0f - 1.0f;
			const float v = ((float)y + 0.5f) / (float)NH * 2.0f - 1.0f;
			/* Domain warp for billowing shapes. */
			const float wx = art_fbm(u * 3.0f, v * 1.5f, 7, 4) - 0.5f;
			const float wy = art_fbm(u * 3.0f + 5.2f, v * 1.5f + 1.3f, 11, 4) - 0.5f;
			const float n = art_fbm(u * 4.0f + wx * 2.0f, v * 2.0f + wy * 2.0f, 3, 6);
			const float fil = art_fbm(u * 10.0f + wx * 4.0f, v * 5.0f + wy * 4.0f, 23, 5);
			/* Elliptical falloff, bent a little along its length. */
			const float bend = v - 0.25f * sinf(u * 2.2f);
			const float r = sqrtf(u * u + bend * bend * 1.6f);
			const float edge = fmaxf(0.0f, 1.0f - r);
			float density = fmaxf(0.0f, n - 0.32f) * 2.2f * edge * edge;
			const float lanes =
				fmaxf(0.0f, 0.55f - art_fbm(u * 6.0f + 3.1f, v * 3.0f - wx * 3.0f, 41, 5)) * 1.8f;
			density = fmaxf(0.0f, density - lanes * 0.35f * edge);
			const float core = powf(fmaxf(0.0f, fil - 0.45f) * 2.0f, 2.0f) * density;
			/* Violet body, magenta-pink to white-hot filaments. */
			const float rr = 0.66f * density + 1.0f * core, gg = 0.18f * density + 0.62f * core,
						bb = 0.80f * density + 0.90f * core;
			const float a = fminf(1.0f, density * 1.4f + core);
			uint8_t* p = &px[((size_t)y * NW + x) * 4];
			/* Straight alpha: the colour without coverage. */
			const float inv = a > 1e-4f ? 1.0f / a : 0.0f;
			p[0] = (uint8_t)(fminf(1.0f, rr * inv) * 255.0f + 0.5f);
			p[1] = (uint8_t)(fminf(1.0f, gg * inv) * 255.0f + 0.5f);
			p[2] = (uint8_t)(fminf(1.0f, bb * inv) * 255.0f + 0.5f);
			p[3] = (uint8_t)(a * 255.0f + 0.5f);
		}
	}
	(void)art_make_texture(cmd, ART_TEX_NEBULA, px, NW, NH);
	free(px);
}

/* Image assets: "XWAL", u32 width, u32 height, straight RGBA, shipped in the
 * APK (assets/vr/<name>.rgba) or next to the executable (vr/<name>.rgba);
 * never part of the source tree. `override_env` names a variable that
 * overrides the path (0 = none). */
static bool art_load_rgba(AeronCommandBuffer* cmd, int slot, const char* name, const char* override_env) {
	const char* override = getenv(override_env);
	if (override && strcmp(override, "0") == 0)
		return false;
	char asset[2][64];
	snprintf(asset[0], sizeof asset[0], "vr/%s.rgba", name);
	snprintf(asset[1], sizeof asset[1], "assets/vr/%s.rgba", name);
	const char* paths[] = { override && override[0] ? override : NULL, asset[0], asset[1] };
	for (size_t i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
		if (!paths[i])
			continue;
		size_t size = 0;
		uint8_t* data = (uint8_t*)SDL_LoadFile(paths[i], &size);
		if (!data)
			continue;
		bool ok = false;
		if (size >= 12 && memcmp(data, "XWAL", 4) == 0) {
			uint32_t w, h;
			memcpy(&w, data + 4, 4);
			memcpy(&h, data + 8, 4);
			if (w && h && w <= 4096 && h <= 4096 && size >= 12 + (size_t)w * h * 4u)
				ok = art_make_texture(cmd, slot, data + 12, (int)w, (int)h);
		}
		SDL_free(data);
		if (ok)
			return true;
	}
	Aeron_LogInfo("tie.vr", "menu scene: no %s (vr/%s.rgba)", name, name);
	return false;
}

/* One LFD resource (type, name; case-insensitive) read whole from the game
 * data of the first source that has the archive. */
static uint8_t* art_read_lfd(const TieFlightAssetSource* assets, const char* path, const char* type,
							 const char* name, size_t* out_size) {
	for (const TieFlightAssetSource* src = assets; src; src = src->fallback) {
		AeronVfs* vfs = src->vfs ? src->vfs : Aeron_GetVfs();
		uint8_t* bytes = NULL;
		size_t size = 0;
		if (!vfs || !AeronVfs_Exists(vfs, AERON_VFS_ROOT_ASSET, path) ||
			!AeronVfs_ReadAll(vfs, AERON_VFS_ROOT_ASSET, path, 64u << 20, &bytes, &size) || size < 16)
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

/* The explosion's frames: TIE Fighter's own XACT animations. */
static void art_load_sprites(AeronCommandBuffer* cmd, const TieFlightAssetSource* assets) {
	static const char* const sets[2] = { "RES640/SPECIES.LFD", "RES320/SPECIES.LFD" };
	for (int pass = 0; pass < 2; ++pass) {
		const char* xact = pass == 0 ? ART_XACT_BOOM : ART_XACT_FIRE;
		uint8_t* bytes = NULL;
		size_t size = 0;
		for (int s = 0; s < 2 && !bytes; ++s)
			bytes = art_read_lfd(assets, sets[s], "XACT", xact, &size);
		if (!bytes) {
			Aeron_LogWarn("tie.vr", "menu scene: no %s explosion sprites in SPECIES.LFD", xact);
			continue;
		}
		TieRgbaFrames frames = { 0 };
		TieFormatError error = { 0 };
		if (TieXact_DecodeRgba8(bytes, size, &frames, &error)) {
			for (int f = 0; f < (int)frames.count; ++f) {
				const TieRgbaFrame* fr = &frames.frames[f];
				if (pass == 0 && f < ART_BOOM_FRAMES)
					art_make_texture(cmd, ART_TEX_BOOM + f, fr->rgba, fr->width, fr->height);
				if (pass == 0 && f == ART_XACT_BLAST_FRAME)
					art_make_texture(cmd, ART_TEX_BLAST, fr->rgba, fr->width, fr->height);
				if (pass == 1 && f == ART_XACT_FIRE_FRAME)
					art_make_texture(cmd, ART_TEX_FIRE, fr->rgba, fr->width, fr->height);
			}
			TieRgbaFrames_Free(&frames);
		} else {
			Aeron_LogWarn("tie.vr", "menu scene: cannot decode %s: %s", xact, error.message);
		}
		free(bytes);
	}
}

/* ---- sounds ---------------------------------------------------------------
 * Creative Voice (the flight banks' records): 8-bit unsigned blocks (type 1,
 * continued by type 2) or type 9 (8-bit unsigned or 16-bit signed PCM). */
static AeronClip art_voc_clip(const uint8_t* d, size_t n) {
	if (n < 26 || memcmp(d, "Creative Voice File", 19) != 0)
		return 0;
	size_t at = (size_t)(d[20] | d[21] << 8);
	uint8_t* pcm = (uint8_t*)malloc(n);
	if (!pcm)
		return 0;
	size_t bytes = 0;
	int rate = 0, bits = 8;
	while (at + 4 <= n && d[at] != 0) {
		const int type = d[at];
		size_t len = (size_t)d[at + 1] | (size_t)d[at + 2] << 8 | (size_t)d[at + 3] << 16;
		const uint8_t* body = d + at + 4;
		if (len > n - (at + 4))
			len = n - (at + 4);
		if (type == 1 && len >= 2 && body[1] == 0) {
			if (!rate)
				rate = (int)(1000000 / (256 - body[0]));
			memcpy(pcm + bytes, body + 2, len - 2);
			bytes += len - 2;
		} else if (type == 2) {
			memcpy(pcm + bytes, body, len);
			bytes += len;
		} else if (type == 9 && len >= 12 && body[5] == 1 && (body[4] == 8 || body[4] == 16)) {
			rate = (int)(body[0] | body[1] << 8 | body[2] << 16 | (uint32_t)body[3] << 24);
			bits = body[4];
			memcpy(pcm + bytes, body + 12, len - 12);
			bytes += len - 12;
		}
		at += 4 + len;
	}
	AeronClip clip = 0;
	const size_t frames = bytes / (size_t)(bits / 8);
	if (rate > 0 && frames)
		clip = Aeron_AudioClipCreate(pcm, frames, rate, 1, bits == 8 ? AERON_PCM_U8 : AERON_PCM_S16);
	free(pcm);
	return clip;
}

static void art_load_sounds(const TieFlightAssetSource* assets) {
	int loaded = 0;
	for (int i = 0; i < ES_COUNT; ++i) {
		size_t size = 0;
		uint8_t* rec = art_read_lfd(assets, k_es_source[i].bank, "VOIC", k_es_source[i].name, &size);
		if (!rec)
			rec = art_read_lfd(assets, k_es_source[i].bank, "BLAS", k_es_source[i].name, &size);
		if (rec) {
			s_art.clip[i] = art_voc_clip(rec, size);
			free(rec);
		}
		loaded += s_art.clip[i] != 0;
	}
	Aeron_LogInfo("tie.vr", "menu scene: %d of %d scene sounds", loaded, ES_COUNT);
}

static float es_master(void);

/* The in-flight sound effects volume (0..15), XWA_XR_SCENE_SFX_VOLUME on top
 * (0 = silent). */
static float es_master(void) {
	TieInflightOptions o;
	memset(&o, 0, sizeof o);
	TieInflightOptions_Get(&o);
	return fminf(fmaxf((float)o.sound_effects_volume / 15.0f, 0.0f), 1.0f) *
		   fmaxf(art_env("XWA_XR_SCENE_SFX_VOLUME", 1.0f), 0.0f);
}

/* Gain (falling off past ref_m metres) and pan for a sound at p (world
 * units, from the viewer). */
static void es_place(const float p[3], float ref_m, float* gain, float* pan) {
	const float d = art_len(p);
	*gain = fminf(1.0f, ref_m / fmaxf(d / ART_K, 0.5f));
	static const float ear[3] = { 1.0f, 0.0f, 0.0f };
	const float* r = s_art.has_head ? s_art.right : ear;
	*pan = d > 0.0f ? 0.8f * art_dot(p, r) / d : 0.0f;
}

static void es_play(int s, const float p[3], float ref_m, float gain, float pitch) {
	if (s_art.mute || !s_art.clip[s])
		return;
	float g, pan;
	es_place(p, ref_m, &g, &pan);
	g *= gain * es_master();
	if (g > 0.003f)
		(void)Aeron_AudioVoicePlay(s_art.clip[s], fminf(g, 1.0f), pan, pitch, 0);
}

static void es_stop(void) {
	for (int i = 0; i < 8; ++i)
		if (s_art.engine[i])
			Aeron_AudioVoiceStop(s_art.engine[i]);
	memset(s_art.engine, 0, sizeof s_art.engine);
	memset(s_art.flyby_done, 0, sizeof s_art.flyby_done);
	memset(s_art.prev_dist, 0, sizeof s_art.prev_dist);
}

/* ---- the fight ------------------------------------------------------------
 * The seconds of the fight before the cover's moment, flown the way the
 * live battle's craft fly in XWA: the nose always along the flight path,
 * rolling only into a turn. Each craft comes in fast on a straight line,
 * rolls as it enters its last turn and pulls through it (turning toward its
 * own top side, the turn's centre above its canopy) straight into its pose on
 * the cover, which is where that turn has brought it. Of a few turn angles
 * and rates the one whose path keeps clearest of the viewer and least behind
 * the screen is taken; the X-wing's and its killer's are picked together so
 * the TIE's guns bear on the X-wing as it is hit. Then the great explosion
 * grows into the cover's and the still holds. */

#define ART_CANDIDATES 234 /* 6 last-turn angles x 3 rates x 13 first turns */

static void art_pose(int ship, float t, float pos[3], float n[3], float u[3]);
static void art_flight_pose(int ship, float t, float pos[3], float n[3], float u[3]);

static float art_speed(int ship) {
	float v = 70.0f; /* m/s */
	if (k_ships[ship].model == ART_TI)
		v = 80.0f;
	else if (k_ships[ship].model == ART_TB)
		v = 55.0f;
	else if (!art_is_tie(ship))
		v = 62.0f;
	return v * art_env("XWA_XR_TIEART_SPEED", 1.0f) * ART_K;
}

static void art_candidate(int ship, int c) {
	static const float alphas[6] = { 25.0f, 45.0f, 65.0f, 85.0f, 110.0f, 140.0f };
	static const float rates[3] = { 0.7f, 1.0f, 1.3f };
	/* No first turn, or one of 60/120/180 degrees banked 55 or 85 degrees
	 * either way (85: nearly a flat turn across the sky). */
	static const float firsts[13][2] = {
		{ 0.0f, 0.0f },     { 60.0f, 55.0f },   { 60.0f, -55.0f },  { 60.0f, 85.0f },   { 60.0f, -85.0f },
		{ 120.0f, 55.0f },  { 120.0f, -55.0f }, { 120.0f, 85.0f },  { 120.0f, -85.0f }, { 180.0f, 55.0f },
		{ 180.0f, -55.0f }, { 180.0f, 85.0f },  { 180.0f, -85.0f },
	};
	const float deg = ART_PI / 180.0f;
	s_art.anim_fl[ship].v = art_speed(ship);
	s_art.anim_fl[ship].alpha = alphas[c % 6] * deg;
	s_art.anim_fl[ship].w = rates[(c / 6) % 3];
	s_art.anim_fl[ship].beta = firsts[(c / 18) % 13][0] * deg;
	s_art.anim_fl[ship].b1 = firsts[(c / 18) % 13][1] * deg;
}

/* The sky's up across nose n (or `fallback` when n is vertical). */
static void art_level(const float n[3], const float fallback[3], float out[3]) {
	static const float z[3] = { 0.0f, 0.0f, 1.0f };
	const float nz = art_dot(n, z);
	for (int a = 0; a < 3; ++a)
		out[a] = z[a] - n[a] * nz;
	if (!art_norm(out))
		memcpy(out, fallback, 3 * sizeof(float));
}

/* The signed roll about n taking a to b. */
static float art_roll(const float a[3], const float b[3], const float n[3]) {
	float x[3];
	art_cross(a, b, x);
	return atan2f(art_dot(x, n), art_dot(a, b));
}

/* How well a path does: clear of the viewer, in view (the last seconds
 * most), not behind the screen. */
static float art_path_score(int ship) {
	float e[3];
	art_ship_pos(ship, e);
	const float end_m = art_len(e) / ART_K;
	float closest = 1e9f, roll_max = 0.0f, prev_u[3] = { 0.0f, 0.0f, 0.0f };
	int hidden = 0, seen = 0;
	float out_of_view = 0.0f, steep = 0.0f;
	for (int i = 0; i <= 40; ++i) {
		float p[3], n[3], u[3];
		art_pose(ship, ART_ANIM_SECONDS * (float)i / 40.0f * 0.999f, p, n, u);
		if (i > 0) {
			/* Turn plus roll, per second: the attitude's rate. */
			roll_max = fmaxf(roll_max, acosf(fminf(1.0f, art_dot(prev_u, u))) / (ART_ANIM_SECONDS / 40.0f));
		}
		memcpy(prev_u, u, sizeof prev_u);
		for (int a = 0; a < 3; ++a)
			p[a] /= ART_K;
		const float d = art_len(p);
		if (i < 40)
			closest = fminf(closest, d);
		const int late = i >= 24 ? 3 : 1;
		hidden += art_behind_screen(p) * late;
		const float yaw = atan2f(p[0], p[1]) * 180.0f / ART_PI;
		const float pitch = atan2f(p[2], sqrtf(p[0] * p[0] + p[1] * p[1])) * 180.0f / ART_PI;
		/* In front and near enough to watch. */
		seen += (i >= 8 && fabsf(yaw) < 75.0f && pitch > -45.0f && pitch < 55.0f) *
				((d < 350.0f) + 2 * (d < 160.0f)) * late;
		/* From 0.4 of the way in (about 4 s before the end) every craft in
		 * the middle of the view: how far out of it. */
		if (i >= 16)
			out_of_view +=
				fmaxf(fabsf(yaw) - 38.0f, 0.0f) + fmaxf(pitch - 25.0f, 0.0f) + fmaxf(-32.0f - pitch, 0.0f);
		/* No diving in steeply from above (or climbing from below). */
		if (i >= 8)
			steep += fmaxf(fabsf(n[2]) - 0.5f, 0.0f);
		/* The X-wing's chaser: the chase right in the middle of the view. */
		if (ship == ART_CHASE_SHIP && i >= 7 && i <= 30)
			out_of_view += 2.0f * (fabsf(yaw) + fabsf(pitch + 6.0f));
	}
	/* Snap rolls look wrong: more than about 2 rad/s costs. */
	return fminf(closest, fmaxf(0.7f * end_m, 8.0f)) - (closest < 5.0f ? 100.0f : 0.0f) -
		   1.5f * (float)hidden + 0.12f * (float)seen - 15.0f * fmaxf(roll_max - 2.0f, 0.0f) -
		   0.35f * out_of_view - 4.0f * steep;
}

/* The runners: each flies its chaser's path 50 m ahead of it, then breaks
 * away: a barrel roll (a roll round a small helix, the nose along the path
 * all the way) if it has one, then a pull out of the forward view. */
static int art_runner_of(int ship) {
	for (int r = 0; r < ART_RUNNERS; ++r)
		if (k_runners[r].ship == ship)
			return r;
	return -1;
}

static void art_runner_plan(void) {
	const float T = ART_ANIM_SECONDS;
	for (int r = 0; r < ART_RUNNERS; ++r) {
		ArtRunner* rn = &s_art.runner[r];
		const int chaser = k_runners[r].chaser;
		rn->ok = 0;
		if (ART_SHIPS <= k_runners[r].ship || !s_art.anim_fl[chaser].v)
			continue;
		const float v = s_art.anim_fl[chaser].v;
		const float lead = k_runners[r].lead_m * ART_K / v;
		if (k_runners[r].weave) {
			rn->chaser = chaser;
			rn->lead = lead;
			rn->v = v;
			rn->tb = ART_ANIM_SECONDS;
			rn->flyby_done = 1; /* it never comes close */
			rn->ok = 1;
			continue;
		}
		float tb = T - lead - k_runners[r].end_s;
		const float break_m =
			k_runners[r].break_m * (r == 0 ? art_env("XWA_XR_RUNNER_BREAK_M", 130.0f) / 130.0f : 1.0f);
		for (float t = 0.0f; break_m > 0.0f && t < tb; t += 0.02f) {
			float p[3], nn[3], uu[3];
			art_flight_pose(chaser, t + lead, p, nn, uu);
			if (art_len(p) / ART_K < break_m) {
				tb = t;
				break;
			}
		}
		rn->chaser = chaser;
		rn->lead = lead;
		rn->tb = fmaxf(tb, 0.0f);
		rn->v = v;
		rn->roll_time = k_runners[r].roll_s;
		rn->helix = 3.0f * ART_K;
		art_flight_pose(chaser, rn->tb + lead, rn->pb, rn->nb, rn->ub);
		const float* away = k_runners[r].away;
		const float along = art_dot(away, rn->nb);
		for (int a = 0; a < 3; ++a)
			rn->pull_up[a] = away[a] - rn->nb[a] * along;
		if (!art_norm(rn->pull_up))
			memcpy(rn->pull_up, rn->ub, sizeof rn->pull_up);
		rn->pull_w = k_runners[r].pull_w;
		rn->pull_bank = art_roll(rn->ub, rn->pull_up, rn->nb);
		rn->flyby_done = 0;
		rn->ok = 1;
	}
}

static void art_runner_pose(int r, float t, float pos[3], float n[3], float u[3]) {
	const ArtRunner* rn = &s_art.runner[r];
	if (!rn->ok)
		return;
	if (t < rn->tb) {
		art_flight_pose(rn->chaser, t + rn->lead, pos, n, u);
		return;
	}
	const float v = rn->v, D1 = rn->roll_time, Rh = rn->helix;
	const float* nb = rn->nb;
	const float* ub = rn->ub;
	const float s = t - rn->tb;
	if (s < D1) {
		/* The barrel roll: a full roll while flying round a helix whose axis
		 * runs along the old heading, its centre toward the canopy. */
		const float x = s / D1;
		const float phi = 2.0f * ART_PI * x * x * (3.0f - 2.0f * x);
		const float dphi = 2.0f * ART_PI * 6.0f * x * (1.0f - x) / D1;
		memcpy(u, ub, 3 * sizeof(float));
		art_rotate(u, nb, phi);
		float side[3];
		art_cross(nb, u, side); /* du/dphi */
		for (int a = 0; a < 3; ++a) {
			pos[a] = rn->pb[a] + nb[a] * v * s + Rh * (ub[a] - u[a]);
			n[a] = nb[a] * v - Rh * dphi * side[a];
		}
		art_norm(n);
		const float d = art_dot(u, n);
		for (int a = 0; a < 3; ++a)
			u[a] -= n[a] * d;
		art_norm(u);
		return;
	}
	/* The pull: rolling onto the turn as it begins, then round. */
	const float s2 = s - D1;
	const float w = rn->pull_w, R = v / w, th = w * s2;
	const float* up = rn->pull_up;
	for (int a = 0; a < 3; ++a) {
		n[a] = nb[a] * cosf(th) + up[a] * sinf(th);
		u[a] = up[a] * cosf(th) - nb[a] * sinf(th);
		pos[a] = rn->pb[a] + nb[a] * v * D1 + R * (up[a] - u[a]);
	}
	const float f = art_smooth(s2 / 0.45f);
	if (f < 1.0f)
		art_rotate(u, n, -(1.0f - f) * rn->pull_bank);
}

static void art_paths(void) {
	const int count = ART_SHIPS < 8 ? ART_SHIPS : 8;
	s_art.anim_paths = 1; /* art_pose follows anim_fl from here */
	static float scores[8][ART_CANDIDATES];
	for (int i = 0; i < count; ++i) {
		int best = 0;
		if (art_runner_of(i) >= 0)
			continue; /* flies after its chaser (art_runner_plan) */
		for (int c = 0; c < ART_CANDIDATES; ++c) {
			art_candidate(i, c);
			scores[i][c] = art_path_score(i);
			if (scores[i][c] > scores[i][best])
				best = c;
		}
		art_candidate(i, best);
		s_art.next_shot[i] = 1.2f + 0.27f * (float)i;
	}
	/* The X-wing's killer: a TIE fighter (any TIE when there is none). It and
	 * the X-wing take the paths, among their good ones, on which its guns bear
	 * best on the X-wing while it hits it. */
	s_art.hunter = -1;
	art_runner_plan();
	if (count <= ART_XWING)
		return;
	int fighters = 0;
	for (int i = 0; i < count; ++i)
		fighters += i != ART_XWING && k_ships[i].model == ART_TF;
	/* Each one's best few paths only. */
	enum { TOP = 24 };
	int top[8][TOP];
	for (int i = 0; i < count && i < ART_FIRST_RUNNER; ++i) {
		for (int j = 0; j < TOP; ++j) {
			top[i][j] = -1;
			for (int c = 0; c < ART_CANDIDATES; ++c) {
				int taken = 0;
				for (int q = 0; q < j; ++q)
					taken |= top[i][q] == c;
				if (!taken && (top[i][j] < 0 || scores[i][c] > scores[i][top[i][j]]))
					top[i][j] = c;
			}
		}
	}
	float best = -1e9f;
	int best_h = -1, best_hc = 0, best_xc = 0;
	for (int h = 0; h < count && h < ART_FIRST_RUNNER; ++h) {
		/* The TIE fighter on the right (any TIE when there is none). */
		if (h == ART_XWING || !art_is_tie(h) || (fighters && k_ships[h].model != ART_TF) ||
			(fighters > 1 && k_ships[h].yaw <= 0.0f))
			continue;
		for (int hj = 0; hj < TOP; ++hj) {
			const int hc = top[h][hj];
			art_candidate(h, hc);
			for (int xj = 0; xj < TOP; ++xj) {
				const int xc = top[ART_XWING][xj];
				art_candidate(ART_XWING, xc);
				float miss = 0.0f, range = 0.0f;
				for (int q = 0; q < 3; ++q) {
					float hp[3], hn[3], hu[3], xp[3], xn[3], xu[3], dir[3];
					art_pose(h, k_hits[q] - 0.15f, hp, hn, hu);
					art_pose(ART_XWING, k_hits[q], xp, xn, xu);
					for (int a = 0; a < 3; ++a)
						dir[a] = xp[a] - hp[a];
					range += art_len(dir) / ART_K;
					art_norm(dir);
					miss += acosf(fminf(1.0f, fmaxf(-1.0f, art_dot(hn, dir)))) * 180.0f / ART_PI;
				}
				miss /= 3.0f;
				range /= 3.0f;
				const float score = scores[h][hc] + scores[ART_XWING][xc] - 1.5f * miss -
									(range > 150.0f ? (range - 150.0f) * 0.3f : 0.0f);
				if (score > best) {
					best = score;
					best_h = h;
					best_hc = hc;
					best_xc = xc;
				}
			}
		}
		/* Back to its own best until it wins. */
		int own = 0;
		for (int c = 0; c < ART_CANDIDATES; ++c)
			own = scores[h][c] > scores[h][own] ? c : own;
		art_candidate(h, own);
	}
	{
		int own = 0;
		for (int c = 0; c < ART_CANDIDATES; ++c)
			own = scores[ART_XWING][c] > scores[ART_XWING][own] ? c : own;
		art_candidate(ART_XWING, own);
	}
	if (best_h >= 0) {
		s_art.hunter = best_h;
		art_candidate(best_h, best_hc);
		art_candidate(ART_XWING, best_xc);
	}
}

/* ---- the jinking X-wing ----------------------------------------------------
 * It flies its chaser's path 50 m ahead of it and breaks off it in three
 * long, smooth banked turns, left, right, left (as the viewer sees it): a
 * sweep out to the left, back across, and straightening onto the path again.
 * Each turn rolls in smoothly and then holds its bank and turn rate, the
 * craft leaning into the curve, its lift toward the turn's centre, rolling
 * over as the turn reverses; then it slides onto the great explosion's centre
 * as it starts and is gone in it. The chaser follows its weaving a moment
 * late, smaller. Both keep the nose along their path. */
#define ART_WEAVE_FROM 1.6f
#define ART_WEAVE_TO (ART_BOOM_AT - 0.3f)
#define ART_WEAVE_SWING_M 40.0f  /* how far out the sweep goes */
#define ART_WEAVE_ROLL_S 0.6f    /* rolling from one turn into the next */
#define ART_WEAVE_BANK_DEG 65.0f /* the bank held through a turn */
#define ART_WEAVE_SAMPLES 512

/* The weave's sideways swing at t, 0..1 (0 outside its window): flown from
 * its turn rate, +1 for the first quarter (the turn out), -1 for the middle
 * half (the long turn back), +1 for the last quarter (onto the path again),
 * each change a smooth roll. Even about the middle with no net turn, so it
 * ends on the path, heading along it. */
static float art_weave(float t) {
	static float table[ART_WEAVE_SAMPLES + 1];
	static int built;
	const float span = ART_WEAVE_TO - ART_WEAVE_FROM;
	if (!built) {
		const float q = 0.25f * span, w = ART_WEAVE_ROLL_S, ds = span / (float)ART_WEAVE_SAMPLES;
		float rate[ART_WEAVE_SAMPLES + 1], pos = 0.0f, neg = 0.0f;
		for (int i = 0; i <= ART_WEAVE_SAMPLES; ++i) {
			const float x = (float)i * ds;
			const float in = art_smoother(x / w) * art_smoother((span - x) / w);
			rate[i] = in * (1.0f - 2.0f * art_smoother((x - q) / w + 0.5f) +
							2.0f * art_smoother((x - 3.0f * q) / w + 0.5f));
			pos += fmaxf(rate[i], 0.0f);
			neg += fmaxf(-rate[i], 0.0f);
		}
		float heading = 0.0f, side = 0.0f, peak = 1e-6f;
		for (int i = 0; i <= ART_WEAVE_SAMPLES; ++i) {
			table[i] = side;
			peak = fmaxf(peak, fabsf(side));
			heading += (rate[i] < 0.0f ? rate[i] * pos / neg : rate[i]) * ds;
			side += heading * ds;
		}
		for (int i = 0; i <= ART_WEAVE_SAMPLES; ++i)
			table[i] /= peak;
		for (int i = 1; i < ART_WEAVE_SAMPLES; ++i)
			s_weave_acc_max =
				fmaxf(s_weave_acc_max, fabsf(table[i + 1] - 2.0f * table[i] + table[i - 1]) / (ds * ds));
		built = 1;
	}
	if (t <= ART_WEAVE_FROM || t >= ART_WEAVE_TO)
		return 0.0f;
	const float f = (t - ART_WEAVE_FROM) / span * (float)ART_WEAVE_SAMPLES;
	const int i = (int)f;
	if (i >= ART_WEAVE_SAMPLES)
		return table[ART_WEAVE_SAMPLES];
	return table[i] + (table[i + 1] - table[i]) * (f - (float)i);
}

/* The offset (world units) from its base path at t: chased = 0 for the
 * runner, 1 for its chaser. */
static void art_weave_offset(int r, int chased, float t, float out[3]) {
	const int chaser = k_runners[r].chaser;
	const float lead = chased ? 0.0f : s_art.runner[r].lead;
	float p[3], n[3], u[3], side[3];
	art_flight_pose(chaser, t + lead, p, n, u);
	static const float z[3] = { 0.0f, 0.0f, 1.0f };
	art_cross(n, z, side); /* the heading's right, level */
	if (!art_norm(side)) {
		art_cross(n, u, side);
		art_norm(side);
	}
	/* The Interceptor follows its turns a moment late, cutting inside them. */
	const float swing =
		chased ? 0.55f * ART_WEAVE_SWING_M * art_weave(t - 0.45f) : ART_WEAVE_SWING_M * art_weave(t);
	for (int a = 0; a < 3; ++a)
		out[a] = side[a] * swing * ART_K;
	if (!chased) {
		/* Onto the explosion's centre as it starts. */
		float b[3], at[3], an[3], au[3];
		art_dir(k_blast[0], k_blast[1], b);
		art_flight_pose(chaser, ART_BOOM_AT + lead, at, an, au);
		const float w = art_smoother((t - (ART_BOOM_AT - 2.2f)) / 2.2f);
		for (int a = 0; a < 3; ++a) {
			b[a] = s_art.view_pos[a] + b[a] * k_blast[2] * ART_K;
			out[a] += (b[a] - at[a]) * w;
		}
	}
}

static void art_weave_pose(int r, int chased, float t, float pos[3], float n[3], float u[3]) {
	const int chaser = k_runners[r].chaser;
	const float lead = chased ? 0.0f : s_art.runner[r].lead;
	const float h = 0.02f;
	float b0[3], b1[3], b2[3], o0[3], o1[3], o2[3], bn[3], bu[3], tn[3], tu[3];
	art_flight_pose(chaser, t + lead - h, b0, tn, tu);
	art_flight_pose(chaser, t + lead, b1, bn, bu);
	art_flight_pose(chaser, t + lead + h, b2, tn, tu);
	art_weave_offset(r, chased, t - h, o0);
	art_weave_offset(r, chased, t, o1);
	art_weave_offset(r, chased, t + h, o2);
	float acc[3] = { 0.0f, 0.0f, 0.0f };
	for (int a = 0; a < 3; ++a) {
		pos[a] = b1[a] + o1[a];
		n[a] = (b2[a] + o2[a]) - (b0[a] + o0[a]);
	}
	/* The lean follows the turn's pull with a little lag (its pull over the
	 * last fifth of a second): a heavy craft rolls into a turn a moment
	 * behind its stick. */
	for (int q = 0; q < 4; ++q) {
		const float tq = t - 0.03f - 0.06f * (float)q;
		float p0[3], p1[3], p2[3];
		art_weave_offset(r, chased, tq - h, p0);
		art_weave_offset(r, chased, tq, p1);
		art_weave_offset(r, chased, tq + h, p2);
		for (int a = 0; a < 3; ++a)
			acc[a] += 0.25f * (p2[a] - 2.0f * p1[a] + p0[a]) / (h * h);
	}
	if (!art_norm(n))
		memcpy(n, bn, sizeof bn);
	/* Lift toward the turn: the base attitude's up, banked toward the turn in
	 * proportion to its sideways pull, ART_WEAVE_BANK_DEG at its hardest (held
	 * through each turn, rolled over smoothly between). */
	float side[3], up[3];
	static const float zup[3] = { 0.0f, 0.0f, 1.0f };
	art_cross(n, zup, side);
	if (!art_norm(side)) {
		art_cross(n, bu, side);
		art_norm(side);
	}
	(void)art_weave(0.0f); /* the table (and its hardest pull) built */
	const float a_ref = (chased ? 0.55f : 1.0f) * ART_WEAVE_SWING_M * ART_K * s_weave_acc_max;
	const float pull = a_ref > 0.0f ? fmaxf(-1.0f, fminf(1.0f, art_dot(acc, side) / a_ref)) : 0.0f;
	/* (Eased up, so the longer, gentler middle turn still leans well in.) */
	const float phi = copysignf(powf(fabsf(pull), 0.6f), pull) * ART_WEAVE_BANK_DEG * ART_PI / 180.0f;
	const float along = art_dot(bu, n);
	for (int a = 0; a < 3; ++a)
		up[a] = bu[a] - n[a] * along;
	if (!art_norm(up))
		memcpy(up, zup, sizeof up);
	float right[3];
	art_cross(n, up, right); /* the rotation of up about the nose toward it */
	for (int a = 0; a < 3; ++a)
		u[a] = up[a] * cosf(phi) + right[a] * sinf(phi);
	art_norm(u);
}

/* The runner whose chaser `ship` is (weaving after it), or -1. */
static int art_weave_chaser_of(int ship) {
	for (int r = 0; r < ART_RUNNERS; ++r)
		if (k_runners[r].weave && k_runners[r].chaser == ship && s_art.runner[r].ok)
			return r;
	return -1;
}

/* A passer's pose: a straight line through its passing point, level, the
 * nose along it. */
static void art_passer_pose(int ship, float t, float pos[3], float n[3], float u[3]) {
	const float* at = k_pass[ship - ART_FIRST_PASSER];
	/* Heading toward and past the viewer, its x through 0 at the crossing. */
	float d[3] = { at[0] / ART_PASS_CROSS_M, -1.0f, 0.0f };
	art_norm(d);
	const float s = ART_PASS_SPEED * (t - ART_PASS_AT);
	for (int a = 0; a < 3; ++a) {
		pos[a] = ((a == 0 ? at[0] : a == 2 ? at[1] : 0.0f) + d[a] * s) * ART_K;
		n[a] = d[a];
	}
	u[0] = 0.0f;
	u[1] = 0.0f;
	u[2] = 1.0f;
}

static int art_is_passer(int ship) {
	return ship >= ART_FIRST_PASSER && ship < ART_FIRST_PASSER + ART_PASSERS;
}

static void art_pose(int ship, float t, float pos[3], float n[3], float u[3]) {
	if (s_art.anim_paths && art_is_passer(ship) && ship < ART_SHIPS && t < ART_ANIM_SECONDS) {
		art_passer_pose(ship, t, pos, n, u);
		return;
	}
	if (s_art.anim_paths && ship < 8 && t < ART_ANIM_SECONDS) {
		const int r = art_runner_of(ship);
		if (r >= 0) {
			if (k_runners[r].weave) {
				float rr[3];
				art_ship_basis(ship, n, rr, u);
				art_ship_pos(ship, pos);
				if (s_art.runner[r].ok)
					art_weave_pose(r, 0, t, pos, n, u);
			} else {
				art_flight_pose(ship, t, pos, n, u);
				art_runner_pose(r, t, pos, n, u);
			}
			return;
		}
		const int c = art_weave_chaser_of(ship);
		if (c >= 0) {
			art_weave_pose(c, 1, t, pos, n, u);
			return;
		}
	}
	art_flight_pose(ship, t, pos, n, u);
}

/* The pose at t, flown backwards from the cover's: the last turn (a pull
 * toward the canopy, its centre above it), before it the earlier turn (a
 * pull banked b1 from level), before that the straight run in, level. Rolls
 * happen only where a turn begins: into the first turn, and from its bank
 * into the last turn's. */
static void art_flight_pose(int ship, float t, float pos[3], float n[3], float u[3]) {
	float r[3];
	art_ship_basis(ship, n, r, u);
	art_ship_pos(ship, pos);
	if (!s_art.anim_paths || ship >= 8 || t >= ART_ANIM_SECONDS)
		return;
	if (art_runner_of(ship) >= 0)
		return; /* runners have no flight of their own */
	const float T = ART_ANIM_SECONDS;
	const float v = s_art.anim_fl[ship].v, w = s_art.anim_fl[ship].w;
	const float alpha = s_art.anim_fl[ship].alpha, beta = s_art.anim_fl[ship].beta;
	const float R = v / w;
	const float t2 = alpha / w, t1 = beta / w; /* seconds in each turn */
	const float tau = T - t;
	/* The junction of the turns (J) and the first turn's start (B). */
	float nj[3], uj[3], pj[3], u1[3], nb[3], ub[3], pb[3];
	for (int a = 0; a < 3; ++a) {
		nj[a] = n[a] * cosf(alpha) - u[a] * sinf(alpha);
		uj[a] = u[a] * cosf(alpha) + n[a] * sinf(alpha);
		pj[a] = pos[a] + u[a] * R - uj[a] * R;
	}
	art_level(nj, uj, u1);
	art_rotate(u1, nj, s_art.anim_fl[ship].b1);
	for (int a = 0; a < 3; ++a) {
		nb[a] = nj[a] * cosf(beta) - u1[a] * sinf(beta);
		ub[a] = u1[a] * cosf(beta) + nj[a] * sinf(beta);
		pb[a] = pj[a] + u1[a] * R - ub[a] * R;
	}
	float level_b[3];
	art_level(nb, ub, level_b);
	const float bank_b = art_roll(level_b, ub, nb); /* into the first turn */
	const float bank_j = art_roll(u1, uj, nj);      /* first turn's bank into the last's */
	/* The roll into a turn starts with it (never on the straight); between
	 * the turns it spans the junction; each is done before the next. */
	float f_b = 1.0f, f_j;
	if (t1 > 0.0f) {
		const float entry = T - t2 - t1;
		const float rt_b = fmaxf(fminf(0.5f + 0.6f * fabsf(bank_b), 0.5f * t1), 1e-3f);
		f_b = art_smooth((t - entry) / rt_b);
		const float half = fmaxf(fminf(fminf(0.25f + 0.3f * fabsf(bank_j), 0.5f * t1), t2), 1e-3f);
		f_j = art_smooth((t - (T - t2 - half)) / (2.0f * half));
	} else {
		const float rt_j = fmaxf(fminf(0.5f + 0.6f * fabsf(bank_j), t2), 1e-3f);
		f_j = art_smooth((t - (T - t2)) / rt_j);
	}
	float geo[3], off;
	if (tau <= t2) {
		const float th = w * tau;
		float n0[3], r0[3], u0[3];
		art_ship_basis(ship, n0, r0, u0);
		for (int a = 0; a < 3; ++a) {
			n[a] = n0[a] * cosf(th) - u0[a] * sinf(th);
			geo[a] = u0[a] * cosf(th) + n0[a] * sinf(th);
			pos[a] = pos[a] + u0[a] * R - geo[a] * R;
		}
		off = -(1.0f - f_j) * bank_j;
	} else if (tau <= t2 + t1) {
		const float th = w * (tau - t2);
		for (int a = 0; a < 3; ++a) {
			n[a] = nj[a] * cosf(th) - u1[a] * sinf(th);
			geo[a] = u1[a] * cosf(th) + nj[a] * sinf(th);
			pos[a] = pj[a] + u1[a] * R - geo[a] * R;
		}
		off = -(1.0f - f_b) * bank_b + f_j * bank_j;
	} else {
		const float run = v * (tau - t2 - t1);
		for (int a = 0; a < 3; ++a) {
			n[a] = nb[a];
			geo[a] = level_b[a];
			pos[a] = pb[a] - nb[a] * run;
		}
		off = f_b * bank_b + f_j * bank_j;
	}
	memcpy(u, geo, sizeof geo);
	if (fabsf(off) > 1e-5f)
		art_rotate(u, n, off);
}

static void art_spawn_bolt(const float p[3], const float d[3], float life, int green) {
	for (int b = 0; b < ART_BOLTS; ++b) {
		if (!s_art.bolts[b].alive) {
			memcpy(s_art.bolts[b].p, p, 3 * sizeof(float));
			memcpy(s_art.bolts[b].d, d, 3 * sizeof(float));
			s_art.bolts[b].life = life;
			s_art.bolts[b].green = green;
			s_art.bolts[b].alive = 1;
			return;
		}
	}
}

/* The hunter's pair of bolts at the X-wing as it will be at `at` (+ a miss
 * offset in metres), timed to arrive then; fired when its flight time before
 * `at` has come. */
static void art_hunter_shot(float before, float t, float at, float miss_m) {
	const int h = s_art.hunter;
	if (h < 0)
		return;
	float hp[3], hn[3], hu[3], x[3], xn[3], xu[3];
	art_pose(ART_XWING, at, x, xn, xu);
	art_pose(h, at, hp, hn, hu);
	const float dx[3] = { x[0] - hp[0], x[1] - hp[1], x[2] - hp[2] };
	const float flight = art_len(dx) / ART_K / ART_BOLT_SPEED;
	const float fire = at - flight;
	if (!(before < fire && t >= fire))
		return;
	float pos[3], n[3], u[3], r[3];
	art_pose(h, t, pos, n, u);
	art_cross(n, u, r);
	x[2] += miss_m * ART_K;
	x[0] += 0.5f * miss_m * ART_K;
	for (int g = -1; g <= 1; g += 2) {
		float p[3], d[3];
		for (int a = 0; a < 3; ++a) {
			p[a] = pos[a] + (r[a] * (float)g * 0.7f + n[a] * 2.0f) * ART_K;
			d[a] = x[a] - p[a];
		}
		const float len = art_len(d) / ART_K;
		art_norm(d);
		/* Gone as they strike (or fly on past for a miss). */
		art_spawn_bolt(p, d, miss_m != 0.0f ? 1.2f : len / ART_BOLT_SPEED, 1);
	}
	es_play(ES_TIE_LASER, pos, 18.0f, 1.0f, 1.0f);
}

/* The engines (fading out once the fight holds) and the fly-bys. */
static void es_update_craft(float t, float dt) {
	const float T = ART_ANIM_SECONDS;
	const float hum = t < T ? 1.0f : 1.0f - art_smooth((t - T) / 1.5f);
	for (int i = 0; i < ART_SHIPS && i < 8; ++i) {
		if (!art_is_tie(i))
			continue;
		float pos[3], n[3], u[3];
		art_pose(i, t, pos, n, u);
		const float d = art_len(pos) / ART_K;
		const float closing = s_art.prev_dist[i] > 0.0f && dt > 0.0f ? (d - s_art.prev_dist[i]) / dt : 0.0f;
		s_art.prev_dist[i] = d;
		/* Doppler, a little exaggerated. */
		const float pitch = fminf(fmaxf(300.0f / (300.0f + closing), 0.8f), 1.25f);
		if (s_art.clip[ES_TIE_ENGINE] && !s_art.mute) {
			float g, pan;
			es_place(pos, 9.0f, &g, &pan);
			g *= 0.55f * hum * es_master();
			if (hum <= 0.0f) {
				if (s_art.engine[i]) {
					Aeron_AudioVoiceStop(s_art.engine[i]);
					s_art.engine[i] = 0;
				}
			} else if (!s_art.engine[i]) {
				s_art.engine[i] = Aeron_AudioVoicePlayFrom(s_art.clip[ES_TIE_ENGINE], fminf(g, 1.0f), pan,
														   pitch, 1, (size_t)i * 2477u);
			} else {
				Aeron_AudioVoiceSetGain(s_art.engine[i], fminf(g, 1.0f));
				Aeron_AudioVoiceSetPan(s_art.engine[i], pan);
				Aeron_AudioVoiceSetPitch(s_art.engine[i], pitch);
			}
		}
		/* The scream as one comes in close. */
		if (!s_art.flyby_done[i] && t < T && d < 32.0f && closing < -2.0f) {
			s_art.flyby_done[i] = 1;
			es_play(ES_TIE_FLYBY, pos, 25.0f, 1.0f, pitch);
		}
	}
}

/* Advances the clock by dt, places the craft, fires the bolts. */
static void art_animate(float dt, bool skip) {
	if (!s_art.anim_paths)
		art_paths();
	const float T = ART_ANIM_SECONDS;
	/* On the startup show, A during the fight jumps to the cover's moment. */
	if (skip && s_art.anim_t > 0.5f && s_art.anim_t < T) {
		if (s_art.anim_t < ART_BOOM_AT) {
			/* Jumped past the blast: still heard. */
			float p[3];
			art_dir(k_blast[0], k_blast[1], p);
			for (int a = 0; a < 3; ++a)
				p[a] *= k_blast[2] * ART_K;
			es_play(ES_BOOM, p, 60.0f, 1.0f, 1.0f);
		}
		s_art.anim_t = T;
	}
	const float before = s_art.anim_t;
	s_art.anim_t = fminf(s_art.anim_t + dt, T + 60.0f);
	const float t = s_art.anim_t;
	for (int i = 0; i < ART_SHIPS; ++i) {
		float pos[3], n[3], u[3];
		art_pose(i, t, pos, n, u);
		art_pose_matrix(n, u, pos, s_art.transform[i]);
		/* The runner is gone by the cover's moment. */
		const int r = art_runner_of(i);
		s_art.hidden[i] =
			r >= 0 && (t >= T || !s_art.runner[r].ok || (k_runners[r].weave && t >= ART_BOOM_AT + 0.12f));
		/* The passers are gone once well past the viewer. */
		if (art_is_passer(i) && t >= ART_PASS_AT + 1.5f)
			s_art.hidden[i] = true;
	}
	s_art.posed = true;
	/* Their scream just before they pass. */
	if (before < ART_PASS_AT - 0.35f && t >= ART_PASS_AT - 0.35f) {
		for (int ship = ART_FIRST_PASSER; ship < ART_FIRST_PASSER + ART_PASSERS && ship < ART_SHIPS; ++ship) {
			float pos[3], n[3], u[3];
			art_pose(ship, ART_PASS_AT, pos, n, u);
			es_play(ES_AW_FLYBY, pos, 30.0f, 1.1f, ship == ART_FIRST_PASSER ? 1.0f : 1.06f);
		}
	}
	for (int r = 0; r < ART_RUNNERS; ++r) {
		if (s_art.runner[r].ok && !s_art.runner[r].flyby_done && t >= s_art.runner[r].tb + 0.25f && t < T) {
			float pos[3], n[3], u[3];
			art_pose(k_runners[r].ship, t, pos, n, u);
			s_art.runner[r].flyby_done = 1;
			es_play(ES_XW_FLYBY, pos, 30.0f, 1.2f, 1.0f);
		}
	}
	/* Bolts: the cover's shooters, the big Interceptor and the X-wing fire
	 * along their noses in bursts until the cover's moment. */
	static const int shooters[5] = { 1, 2, 3, 0, 5 };
	for (int si = 0; si < 5 && t < T - ART_SALVO_SECONDS - 0.1f; ++si) {
		const int ship = shooters[si];
		if ((ship == ART_XWING && t > ART_BOOM_AT) ||
			(ship == s_art.hunter && t > k_misses[0] - 1.0f && t < ART_BOOM_AT))
			continue; /* the hunter's shots at the X-wing are its own */
		if (before < s_art.next_shot[ship] && t >= s_art.next_shot[ship]) {
			float pos[3], n[3], u[3], r[3];
			art_pose(ship, t, pos, n, u);
			art_cross(n, u, r);
			const int green = ship != ART_XWING;
			const float spread = green ? 0.7f : 5.2f;
			es_play(green ? ES_TIE_LASER : ES_REBEL_LASER, pos, 18.0f, 0.8f, 0.96f + 0.08f * (float)(si & 1));
			for (int g = -1; g <= 1; g += 2) {
				float p[3];
				for (int a = 0; a < 3; ++a)
					p[a] = pos[a] + (r[a] * (float)g * spread + n[a] * 2.0f) * ART_K;
				art_spawn_bolt(p, n, 1.2f, green);
			}
			s_art.next_shot[ship] = t + 0.42f + 0.06f * (float)si;
		}
	}
	/* The hunter: bursts of three at the X-wing, two bursts walking in (all
	 * misses), then three ending in the hits. */
	if (t < ART_BOOM_AT) {
		for (int b = 0; b < 5; ++b) {
			const float at = b < 2 ? k_misses[b] : k_hits[b - 2];
			for (int sh = 0; sh < 3; ++sh) {
				const float side = (sh & 1) ? -1.0f : 1.0f;
				const float miss = b < 2 ? side * (7.0f - 1.5f * (float)sh) : sh < 2 ? side * 3.0f : 0.0f;
				art_hunter_shot(before, t, at - 0.13f * (float)(2 - sh), miss);
			}
		}
	}
	for (int b = 0; b < ART_BOLTS; ++b) {
		if (!s_art.bolts[b].alive)
			continue;
		s_art.bolts[b].life -= dt;
		for (int a = 0; a < 3; ++a)
			s_art.bolts[b].p[a] += s_art.bolts[b].d[a] * ART_BOLT_SPEED * ART_K * dt;
		s_art.bolts[b].alive = s_art.bolts[b].life > 0.0f && t < T;
	}
	/* The cover's salvos, the X-wing's hits, the blast. */
	const float salvo_at = T - ART_SALVO_SECONDS;
	if (before < salvo_at && t >= salvo_at) {
		for (int si = 0; si < ART_SALVOS; ++si) {
			float pos[3], n[3], u[3];
			art_pose(k_salvos[si].shooter, t, pos, n, u);
			const ArtGuns g = k_salvos[si].guns;
			es_play(g == ART_GUNS_TIE || g == ART_GUNS_INTERCEPTOR ? ES_TIE_LASER : ES_REBEL_LASER, pos,
					18.0f, 1.0f, 1.0f);
		}
	}
	for (int h = 0; h < 3; ++h) {
		if (before < k_hits[h] && t >= k_hits[h]) {
			float pos[3], n[3], u[3];
			art_pose(ART_XWING, t, pos, n, u);
			es_play(ES_HIT0 + h, pos, 30.0f, 0.8f, 1.0f);
		}
	}
	if (before < ART_BOOM_AT && t >= ART_BOOM_AT) {
		float p[3];
		art_dir(k_blast[0], k_blast[1], p);
		for (int a = 0; a < 3; ++a)
			p[a] *= k_blast[2] * ART_K;
		es_play(ES_BOOM, p, 60.0f, 1.0f, 1.0f);
		es_play(ES_HIT2, p, 60.0f, 0.7f, 0.8f);
	}
	/* A TIE screaming past right at the cover's moment: the nearest. */
	if (before < T && t >= T) {
		int nearest = -1;
		float nd = 1e30f, np[3] = { 0.0f, 0.0f, 0.0f };
		for (int i = 0; i < ART_SHIPS && i < 8; ++i) {
			float p[3], n[3], u[3];
			if (!art_is_tie(i))
				continue;
			art_pose(i, T - 0.05f, p, n, u);
			const float d = art_len(p);
			if (d < nd) {
				nd = d;
				nearest = i;
				memcpy(np, p, sizeof np);
			}
		}
		if (nearest >= 0)
			es_play(ES_TIE_FLYBY, np, 25.0f, 1.2f, 1.0f);
	}
	es_update_craft(t, dt);
}

/* ---- the startup show -----------------------------------------------------
 * Once per run, when the menus first come up: the fight plays with the
 * screen away and the controllers reaching nothing (A during it jumps to its
 * end); then the title: the logo large where the screen will be, "Press (A)
 * to Continue" under it; A brings the screen in (its frame and the logo above
 * it with it). Without a logo (vr/logo.rgba) the screen comes in by itself
 * once the fight is over. XWA_XR_TITLE=0: none of it. */

static bool art_title_active(void) { return !s_art.title_done && art_env("XWA_XR_TITLE", 1.0f) != 0.0f; }

void TieXrArt_SkipStartupShow(void) {
	if (!s_art.title_started)
		s_art.title_done = true;
}

static bool art_has_title(void) { return s_art.tex[ART_TEX_LOGO] != NULL; }

/* The title is up: once the fight has come to the cover's moment. */
static bool art_title_shown(void) {
	return art_title_active() && s_art.title_started && art_has_title() && s_art.anim_t >= ART_ANIM_SECONDS;
}

float TieXrArt_ScreenReveal(void) {
	if (!art_title_active() || !s_art.title_started)
		return 1.0f; /* (films and loading before it: the screen as usual) */
	float x;
	if (art_has_title()) {
		/* The title holds until A is pressed. */
		if (!s_art.title_go)
			return 0.0f;
		x = s_art.go_t / ART_REVEAL_SECONDS;
	} else {
		if (s_art.anim_t < ART_ANIM_SECONDS)
			return 0.0f;
		x = (s_art.anim_t - ART_ANIM_SECONDS) / ART_REVEAL_SECONDS;
	}
	x = fminf(fmaxf(x, 0.0f), 1.0f);
	return fmaxf(x * x * (3.0f - 2.0f * x), 1e-3f);
}

/* Once a frame (the scene up): the title's clock and its A. */
static void art_title_update(float dt) {
	s_art.pressed = false;
	if (!art_title_active())
		return;
	s_art.title_started = true;
	/* The title waits for A only (the controllers reach nothing else). */
	bool pressed = Aeron_XrTakeAPressed() != 0;
	if (art_title_shown()) {
		s_art.shown_t += dt;
		if (!s_art.title_go && pressed && s_art.shown_t >= 0.4f) {
			s_art.title_go = true;
			pressed = false;
		}
	}
	s_art.pressed = pressed;
	if (s_art.title_go) {
		s_art.go_t += dt;
		if (s_art.go_t >= ART_REVEAL_SECONDS)
			s_art.title_done = true; /* once per run */
	}
}

/* ---- per frame -------------------------------------------------------------- */

void TieXrArt_ViewPos(float out[3]) {
	/* XWA_XR_ENV_BACK: the viewer sits this far (metres) behind the scene's
	 * reference point, matching the screen being as much further away than
	 * its original 2.1 m. */
	out[0] = 0.0f;
	out[1] = -art_env("XWA_XR_ENV_BACK", 1.1f) * ART_K;
	out[2] = 0.0f;
}

void TieXrArt_Advance(float dt, const float head_q[4]) {
	TieXrArt_ViewPos(s_art.view_pos);
	if (head_q) {
		/* (1,0,0) turned by the head (LOCAL: x right, y up, -z ahead), into
		 * the scene's axes (x right, y ahead, z up). */
		const float x = head_q[0], y = head_q[1], z = head_q[2], w = head_q[3];
		const float lx = 1.0f - 2.0f * (y * y + z * z), ly = 2.0f * (x * y + w * z),
					lz = 2.0f * (x * z - w * y);
		s_art.right[0] = lx;
		s_art.right[1] = -lz;
		s_art.right[2] = ly;
		s_art.has_head = true;
	}
	const bool title = art_title_active();
	art_title_update(fminf(fmaxf(dt, 0.0f), 0.1f));
	/* On the startup show, A during the fight jumps to the cover's moment. */
	const bool skip = s_art.pressed;
	const char* fixed = getenv("TIE_XR_ENV_TIME");
	if (fixed && fixed[0]) {
		/* Played silently up to that moment in small steps (the bolts fly as
		 * they would), then held. */
		const float target = fmaxf((float)atof(fixed), 0.0f);
		if (!s_art.fixed || target != s_art.fixed_t || !s_art.anim_paths) {
			s_art.fixed = true;
			s_art.fixed_t = target;
			s_art.mute = true;
			s_art.anim_t = 0.0f;
			memset(s_art.bolts, 0, sizeof s_art.bolts);
			s_art.anim_paths = 0;
			art_animate(0.0f, false);
			while (s_art.anim_t < target - 1e-4f)
				art_animate(fminf(1.0f / 90.0f, target - s_art.anim_t), false);
			s_art.mute = false;
		}
		art_animate(0.0f, false);
	} else {
		art_animate(fminf(fmaxf(dt, 0.0f), 0.1f), skip);
	}
	if (title && !art_has_title() && s_art.anim_t >= ART_ANIM_SECONDS + ART_REVEAL_SECONDS)
		s_art.title_done = true; /* once per run (no title: in by itself) */
}

/* ---- billboards --------------------------------------------------------- */

#define ART_SKY_DIST 60000.0f /* sky sprites only need a direction */

static float s_fade; /* the scene's fade in, this frame */

/* Queue a quad: corners (world units) bottom-left, bottom-right, top-right,
 * top-left; tint linear RGBA (HDR allowed). */
static void art_quad_tex(AeronScene3D* scene, AeronTexture* tex, AeronSceneBillboardStage stage,
						 AeronSceneBillboardBlend blend, float c[4][3], const float tint[4]) {
	if (!tex)
		return;
	static const float uv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
	AeronSceneBillboardDesc d;
	memset(&d, 0, sizeof d);
	d.texture = tex;
	d.blend = blend;
	d.stage = stage;
	for (int v = 0; v < 4; ++v) {
		memcpy(d.corners[v], c[v], sizeof d.corners[v]);
		memcpy(d.uv[v], uv[v], sizeof d.uv[v]);
		/* The scene's fade in: PMA fades its opacity (colour with it),
		 * additive its light. */
		for (int k = 0; k < 3; ++k)
			d.colors[v][k] = tint[k] * (blend == AERON_SCENE_BILLBOARD_BLEND_PMA ? tint[3] : 1.0f) * s_fade;
		d.colors[v][3] = tint[3] * (blend == AERON_SCENE_BILLBOARD_BLEND_PMA ? s_fade : 1.0f);
	}
	AeronScene_AddBillboard(scene, &d);
}

static void art_quad(AeronScene3D* scene, int slot, AeronSceneBillboardStage stage,
					 AeronSceneBillboardBlend blend, float c[4][3], const float tint[4]) {
	if (!s_art.tex[slot])
		return;
	if (slot == ART_TEX_BLAST || slot == ART_TEX_FIRE ||
		(slot >= ART_TEX_BOOM && slot <= ART_TEX_BOOM_LAST)) {
		float mid[3];
		for (int k = 0; k < 3; ++k)
			mid[k] = 0.25f * (c[0][k] + c[1][k] + c[2][k] + c[3][k]);
		for (int v = 0; v < 4; ++v)
			for (int k = 0; k < 3; ++k)
				c[v][k] = mid[k] + (c[v][k] - mid[k]) * ART_XACT_FILL;
	}
	art_quad_tex(scene, s_art.tex[slot], stage, blend, c, tint);
}

/* Corners of a rectangle at `center` spanned by half-axes `ax` (right) and
 * `ay` (up). */
static void art_rect(const float center[3], const float ax[3], const float ay[3], float c[4][3]) {
	for (int k = 0; k < 3; ++k) {
		c[0][k] = center[k] - ax[k] - ay[k];
		c[1][k] = center[k] + ax[k] - ay[k];
		c[2][k] = center[k] + ax[k] + ay[k];
		c[3][k] = center[k] - ax[k] + ay[k];
	}
}

/* A viewer-facing square of `size` (world units) at `center`, rolled by
 * `roll` degrees. */
static void art_facing(const float center[3], float size, float roll, float c[4][3]) {
	float f[3] = { center[0] - s_art.view_pos[0], center[1] - s_art.view_pos[1],
				   center[2] - s_art.view_pos[2] };
	art_norm(f);
	static const float z[3] = { 0.0f, 0.0f, 1.0f };
	float r[3], u[3];
	art_cross(f, z, r);
	art_norm(r);
	art_cross(r, f, u);
	const float rr = roll * ART_PI / 180.0f, cr = cosf(rr), sr = sinf(rr);
	float ax[3], ay[3];
	for (int k = 0; k < 3; ++k) {
		ax[k] = (r[k] * cr + u[k] * sr) * 0.5f * size;
		ay[k] = (u[k] * cr - r[k] * sr) * 0.5f * size;
	}
	art_rect(center, ax, ay, c);
}

/* A sky sprite `w_deg` x `h_deg` toward yaw/pitch, rolled. */
static void art_sky_rect(AeronScene3D* scene, int slot, float yaw, float pitch, float w_deg, float h_deg,
						 float roll, const float tint[4]) {
	if (!s_art.tex[slot])
		return;
	float f[3], r[3], u[3], center[3];
	static const float z[3] = { 0.0f, 0.0f, 1.0f };
	art_dir(yaw, pitch, f);
	for (int k = 0; k < 3; ++k)
		center[k] = s_art.view_pos[k] + f[k] * ART_SKY_DIST;
	art_cross(f, z, r);
	art_norm(r);
	art_cross(r, f, u);
	const float rr = roll * ART_PI / 180.0f, cr = cosf(rr), sr = sinf(rr);
	const float hw = ART_SKY_DIST * tanf(fminf(w_deg, 170.0f) * ART_PI / 360.0f);
	const float hh = ART_SKY_DIST * tanf(fminf(h_deg, 170.0f) * ART_PI / 360.0f);
	float ax[3], ay[3], c[4][3];
	for (int k = 0; k < 3; ++k) {
		ax[k] = (r[k] * cr + u[k] * sr) * hw;
		ay[k] = (u[k] * cr - r[k] * sr) * hh;
	}
	art_rect(center, ax, ay, c);
	art_quad(scene, slot, AERON_SCENE_BILLBOARD_STAGE_SKY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, tint);
}

/* A beam strip from `a` to `b` (world units), `width` wide, facing the
 * viewer. */
static void art_beam(AeronScene3D* scene, const float a[3], const float b[3], float width,
					 const float tint[4]) {
	float along[3], mid[3], side[3];
	for (int k = 0; k < 3; ++k) {
		along[k] = b[k] - a[k];
		mid[k] = 0.5f * (a[k] + b[k]) - s_art.view_pos[k];
	}
	art_cross(along, mid, side);
	art_norm(side);
	float c[4][3];
	for (int k = 0; k < 3; ++k) {
		side[k] *= 0.5f * width;
		c[0][k] = a[k] - side[k];
		c[1][k] = b[k] - side[k];
		c[2][k] = b[k] + side[k];
		c[3][k] = a[k] + side[k];
	}
	art_quad(scene, ART_TEX_BEAM, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_ADDITIVE,
			 c, tint);
}

static const float k_red[4] = { 2.4f, 0.35f, 0.25f, 1.0f };
static const float k_green[4] = { 0.4f, 2.4f, 0.45f, 1.0f };
static const float k_white[4] = { 1.5f, 1.4f, 1.3f, 1.0f };

/* The cover's salvos along their shooters' noses; `reach` 0..1: how far the
 * bolts have come out of the guns yet (fired into the cover's moment). */
static void art_submit_salvos(AeronScene3D* scene, float reach) {
	for (int si = 0; si < ART_SALVOS; ++si) {
		const ArtSalvo* sv = &k_salvos[si];
		float from[3], to[3], n[3], r[3], u[3];
		art_ship_pos(sv->shooter, from);
		art_ship_pos(sv->target, to);
		const float dx[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
		const float dist = art_len(dx);
		/* Out of the guns where the craft is now. */
		art_pose(sv->shooter, s_art.anim_t, from, n, u);
		art_cross(n, u, r);
		float reach_end = 0.0f;
		for (int sp = 0; sp < sv->span_count; ++sp)
			reach_end = fmaxf(reach_end, sv->spans[sp][1]);
		reach_end *= reach;
		static const float xw_guns[4][2] = {
			{ 5.2f, 1.3f }, { -5.2f, 1.3f }, { 5.2f, -1.3f }, { -5.2f, -1.3f }
		};
		static const float tie_guns[2][2] = { { 0.6f, -1.0f }, { -0.6f, -1.0f } };
		static const float aw_guns[2][2] = { { 3.3f, 0.0f }, { -3.3f, 0.0f } };
		static const float int_guns[2][2] = { { 3.6f, 1.6f }, { -3.6f, 1.6f } };
		const float(*guns)[2] = xw_guns;
		int gun_count = 4;
		const float* tint = k_red;
		switch (sv->guns) {
			case ART_GUNS_TIE:
				guns = tie_guns;
				gun_count = 2;
				tint = k_green;
				break;
			case ART_GUNS_AWING:
				guns = aw_guns;
				gun_count = 2;
				break;
			case ART_GUNS_INTERCEPTOR:
				guns = int_guns;
				gun_count = 2;
				tint = k_green;
				break;
			default:
				break;
		}
		for (int sp = 0; sp < sv->span_count; ++sp) {
			if (sv->spans[sp][0] >= reach_end)
				continue;
			const float end = fminf(sv->spans[sp][1], reach_end);
			for (int g = 0; g < gun_count; ++g) {
				float a0[3], a1[3];
				for (int a = 0; a < 3; ++a) {
					const float gun = from[a] + (r[a] * guns[g][0] + u[a] * guns[g][1]) * ART_K;
					a0[a] = gun + n[a] * dist * sv->spans[sp][0];
					a1[a] = gun + n[a] * dist * end;
				}
				art_beam(scene, a0, a1, 0.4f * ART_K, tint);
			}
		}
	}
}

/* A great explosion centred on `at` (yaw, pitch, metres, diameter in metres):
 * the blast with two fire clouds in it. */
static void art_submit_fireball(AeronScene3D* scene, const float at[4]) {
	float p[3], c[4][3];
	static const struct {
		float dyaw, dpitch, size, roll;
		int tex;
	} puffs[] = {
		{ 0.0f, 0.0f, 1.0f, 0.0f, ART_TEX_BLAST },
		{ -6.0f, 4.0f, 0.62f, 40.0f, ART_TEX_FIRE },
		{ 7.0f, -3.0f, 0.55f, -60.0f, ART_TEX_FIRE },
	};
	for (size_t i = 0; i < sizeof puffs / sizeof puffs[0]; ++i) {
		art_dir(at[0] + puffs[i].dyaw, at[1] + puffs[i].dpitch, p);
		for (int a = 0; a < 3; ++a)
			p[a] = s_art.view_pos[a] + p[a] * (at[2] - (float)i * 3.0f) * ART_K;
		art_facing(p, at[3] * puffs[i].size * ART_K, puffs[i].roll, c);
		art_quad(scene, puffs[i].tex, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c,
				 k_white);
	}
}

static void art_submit_bolts(AeronScene3D* scene) {
	for (int b = 0; b < ART_BOLTS; ++b) {
		if (!s_art.bolts[b].alive)
			continue;
		float a0[3];
		for (int a = 0; a < 3; ++a)
			a0[a] = s_art.bolts[b].p[a] - s_art.bolts[b].d[a] * 5.0f * ART_K;
		art_beam(scene, a0, s_art.bolts[b].p, 0.4f * ART_K, s_art.bolts[b].green ? k_green : k_red);
	}
}

/* The helmet and the clouds behind, the fire, the great explosion with the
 * X-wing breaking up in it; the helmet fades in once the fight has come to
 * the cover's moment. */
static void art_submit_sprites(AeronScene3D* scene, bool ships) {
	const float t = ships ? s_art.anim_t : ART_ANIM_SECONDS + 60.0f;
	const float T = ART_ANIM_SECONDS;
	/* Vader's helmet, looming behind the screen and the fight, a dark
	 * blue-violet; XWA_XR_VADER_BRIGHTNESS scales it. */
	const float vader = art_smooth((t - T) / art_env("XWA_XR_VADER_FADE_SECONDS", 2.5f));
	if (s_art.tex[ART_TEX_VADER] && vader > 0.0f) {
		const float b = art_env("XWA_XR_VADER_BRIGHTNESS", 1.0f);
		/* 15 % darker and slightly see-through (premultiplied: the colour
		 * carries the opacity too). */
		const float dark = 0.85f, opacity = 0.8f * vader;
		const float tint[4] = { 0.18f * b * dark * opacity, 0.17f * b * dark * opacity,
								0.42f * b * dark * opacity, opacity };
		const float h = 39.8f;
		const float w = h * (float)s_art.tex_w[ART_TEX_VADER] / (float)s_art.tex_h[ART_TEX_VADER];
		const float pitch = art_env("XWA_XR_VADER_PITCH", 31.0f); /* the eyes above the screen */
		art_sky_rect(scene, ART_TEX_VADER, 0.0f, pitch, w, h, 0.0f, tint);
	}
	/* Purple clouds below. */
	{
		static const struct {
			float yaw, pitch, w, h, roll, gain;
		} clouds[] = {
			{ -30.0f, -42.0f, 110.0f, 40.0f, 6.0f, 0.55f },
			{ 35.0f, -40.0f, 100.0f, 36.0f, -8.0f, 0.5f },
			{ 0.0f, -58.0f, 120.0f, 46.0f, 0.0f, 0.45f },
		};
		for (size_t i = 0; i < sizeof clouds / sizeof clouds[0]; ++i) {
			const float g = clouds[i].gain;
			const float tint[4] = { 0.55f * g, 0.45f * g, 1.0f * g, 1.0f };
			art_sky_rect(scene, ART_TEX_NEBULA, clouds[i].yaw, clouds[i].pitch, clouds[i].w, clouds[i].h,
						 clouds[i].roll, tint);
		}
	}
	if (!ships)
		return;
	float c[4][3];
	if (t >= T) {
		/* The cover's moment, held. */
		art_submit_salvos(scene, 1.0f);
		float q[3];
		art_submit_fireball(scene, k_blast);
		art_ship_pos(ART_XWING, q);
		art_facing(q, 16.0f * ART_K, 70.0f, c);
		art_quad(scene, ART_TEX_FIRE, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c,
				 k_white);
		art_facing(q, 10.0f * ART_K, -20.0f, c);
		art_quad(scene, ART_TEX_BLAST, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA,
				 c, k_white);
		return;
	}
	/* The run-up: bolts in flight, the X-wing hit, then the blast; the
	 * cover's salvos leave the guns in the last half second. */
	art_submit_bolts(scene);
	if (t > T - ART_SALVO_SECONDS)
		art_submit_salvos(scene, art_smooth((t - (T - ART_SALVO_SECONDS)) / ART_SALVO_SECONDS));
	float xw[3], n[3], u[3];
	art_pose(ART_XWING, t, xw, n, u);
	for (int h = 0; h < 3; ++h) {
		const float a = (t - k_hits[h]) / 0.35f;
		if (a > 0.0f && a < 1.0f) {
			const float tint[4] = { 1.5f, 1.4f, 1.3f, 1.0f - a };
			art_facing(xw, (3.0f + 5.0f * a) * ART_K, 40.0f * (float)h, c);
			art_quad(scene, ART_TEX_FIRE, AERON_SCENE_BILLBOARD_STAGE_OVERLAY,
					 AERON_SCENE_BILLBOARD_BLEND_PMA, c, tint);
		}
	}
	if (t >= ART_BOOM_AT) {
		/* The great explosion growing into the cover's: the explosion's
		 * frames up to its height (the cover's blast), the flames swelling
		 * with it, a flash at the start. */
		const float g = (t - ART_BOOM_AT) / (T - ART_BOOM_AT);
		int frame = (int)(g * ((float)ART_BOOM_FRAMES - 0.01f));
		frame = frame < 0 ? 0 : frame > ART_BOOM_FRAMES - 1 ? ART_BOOM_FRAMES - 1 : frame;
		const int slot = s_art.tex[ART_TEX_BOOM + frame] ? ART_TEX_BOOM + frame : ART_TEX_BLAST;
		const float grow = 0.35f + 0.65f * art_smooth(g);
		float p[3];
		art_dir(k_blast[0], k_blast[1], p);
		for (int a = 0; a < 3; ++a)
			p[a] = s_art.view_pos[a] + p[a] * k_blast[2] * ART_K;
		art_facing(p, k_blast[3] * grow * ART_K, 0.0f, c);
		art_quad(scene, slot, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c,
				 k_white);
		static const struct {
			float dyaw, dpitch, size, roll;
		} puffs[2] = { { -6.0f, 4.0f, 0.62f, 40.0f }, { 7.0f, -3.0f, 0.55f, -60.0f } };
		const float fire = art_smooth((g - 0.25f) / 0.75f);
		for (int i = 0; i < 2 && fire > 0.0f; ++i) {
			float q[3];
			art_dir(k_blast[0] + puffs[i].dyaw, k_blast[1] + puffs[i].dpitch, q);
			for (int a = 0; a < 3; ++a)
				q[a] = s_art.view_pos[a] + q[a] * (k_blast[2] - (float)(i + 1) * 3.0f) * ART_K;
			art_facing(q, k_blast[3] * puffs[i].size * fire * ART_K, puffs[i].roll, c);
			art_quad(scene, ART_TEX_FIRE, AERON_SCENE_BILLBOARD_STAGE_OVERLAY,
					 AERON_SCENE_BILLBOARD_BLEND_PMA, c, k_white);
		}
		const float flash = 1.0f - (t - ART_BOOM_AT) / 0.6f;
		if (flash > 0.0f && s_art.tex[ART_TEX_GLOW]) {
			const float f = 1.6f * flash * flash;
			const float tint[4] = { 1.0f * f, 0.75f * f, 0.45f * f, 1.0f };
			art_facing(p, k_blast[3] * 0.9f * ART_K, 0.0f, c);
			art_quad(scene, ART_TEX_GLOW, AERON_SCENE_BILLBOARD_STAGE_OVERLAY,
					 AERON_SCENE_BILLBOARD_BLEND_ADDITIVE, c, tint);
		}
		/* The X-wing catching fire as it breaks up. */
		const float burn = art_smooth((t - ART_BOOM_AT - 0.3f) / 1.1f);
		if (burn > 0.0f) {
			art_facing(xw, 16.0f * burn * ART_K, 70.0f, c);
			art_quad(scene, ART_TEX_FIRE, AERON_SCENE_BILLBOARD_STAGE_OVERLAY,
					 AERON_SCENE_BILLBOARD_BLEND_PMA, c, k_white);
			art_facing(xw, 10.0f * burn * ART_K, -20.0f, c);
			art_quad(scene, ART_TEX_BLAST, AERON_SCENE_BILLBOARD_STAGE_OVERLAY,
					 AERON_SCENE_BILLBOARD_BLEND_PMA, c, k_white);
		}
	}
}

/* ---- the logo and the title ------------------------------------------------ */

/* "Press (A) to Continue": its font and texture (made once). */
static struct {
	AeronFontAtlas font;
	int font_state; /* 0 not tried, 1 loaded, -1 failed */
	AeronRenderTarget* rt;
	AeronDrawList2D* list;
	int w, h;
} s_text;

void TieXrArt_SetScreen(const TieXrArtScreen* screen) {
	s_art.have_screen = screen != NULL;
	if (screen)
		s_art.screen = *screen;
}

/* The logo in the frame's plane: above the frame, or (title) large where the
 * screen will be. `alpha` fades it (premultiplied). */
static void art_submit_logo_at(AeronScene3D* scene, float alpha, bool title) {
	if (!s_art.tex[ART_TEX_LOGO] || alpha <= 0.0f || !s_art.have_screen)
		return;
	const TieXrArtScreen* sc = &s_art.screen;
	const float f = sc->frame_scale, k = sc->units;
	const float lw = (title ? 0.9f : 0.6f) * sc->pic_w * f * art_env("XWA_XR_LOGO_SCALE", 1.0f);
	const float lh = lw * (float)s_art.tex_h[ART_TEX_LOGO] / (float)s_art.tex_w[ART_TEX_LOGO];
	const float up =
		title ? sc->lift * f : (sc->lift + 0.5f * sc->pic_h + sc->frame_top) * f + 0.05f + 0.5f * lh;
	const float center[3] = { s_art.view_pos[0], s_art.view_pos[1] + (sc->frame_dist + 0.02f) * k,
							  s_art.view_pos[2] + up * k };
	const float ax[3] = { 0.5f * lw * k, 0.0f, 0.0f };
	const float ay[3] = { 0.0f, 0.0f, 0.5f * lh * k };
	float c[4][3];
	art_rect(center, ax, ay, c);
	const float tint[4] = { alpha, alpha, alpha, alpha };
	art_quad(scene, ART_TEX_LOGO, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, tint);
}

/* A controller button's letter in a ring, centred at (cx, cy), `d` across. */
static void art_draw_key_ring(AeronDrawList2D* list, AeronTextStyle* style, const char* key, float cx, float cy,
							  float d) {
	AeronDrawList2DRRect r;
	memset(&r, 0, sizeof r);
	r.dst_x = cx - 0.5f * d;
	r.dst_y = cy - 0.5f * d;
	r.dst_w = d;
	r.dst_h = d;
	r.radius_px = 0.5f * d;
	r.border_px = fmaxf(2.0f, 0.075f * d);
	r.soft_px = 1.5f;
	const float a = style->tint[3];
	for (int i = 0; i < 3; ++i)
		r.border[i] = style->tint[i] * a;
	r.border[3] = a;
	AeronDrawList_AddRRect(list, &r);
	const float w = AeronText_MeasureWidth(style, key, -1);
	const float h = AeronText_LineHeight(style);
	AeronText_Draw(list, style, cx - 0.5f * w, cy - 0.5f * h, AERON_TEXT_LEFT, key, -1, NULL);
}

/* "Press (A) to Continue" into a texture of its own (once). */
static void art_title_text(AeronCommandBuffer* cmd) {
	if (s_text.rt || s_text.font_state < 0 || !cmd)
		return;
	if (s_text.font_state == 0) {
		s_text.font_state = -1;
		if (AeronFontAtlas_LoadVfs(&s_text.font, cmd, Aeron_GetVfs(), AERON_VFS_ROOT_RESOURCE, "assets/fonts/menu",
								   16u * 1024u * 1024u))
			s_text.font_state = 1;
		else
			Aeron_LogWarn("tie.vr", "menu scene: no font for the title's prompt");
		return; /* drawn next time */
	}
	s_text.w = 1024;
	s_text.h = 128;
	s_text.rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width = s_text.w, .height = s_text.h, .format = AERON_TEXTURE_FORMAT_RGBA8_UNORM });
	s_text.list = AeronDrawList_Create(64);
	if (!s_text.rt || !s_text.list) {
		s_text.font_state = -1;
		return;
	}
	static const char* const before = "Press";
	static const char* const key = "A";
	static const char* const after = "to Continue";
	AeronTextStyle style;
	memset(&style, 0, sizeof style);
	style.font = &s_text.font;
	style.scale = 1.0f;
	const float tall = AeronText_LineHeight(&style);
	const float space = 0.32f * tall, ring = 1.05f * tall;
	const float wide = AeronText_MeasureWidth(&style, before, -1) + space + ring + space +
					   AeronText_MeasureWidth(&style, after, -1);
	/* As wide as the texture allows, the line no taller than 70 % of it. */
	style.scale = fminf(wide > 0.0f ? 0.92f * (float)s_text.w / wide : 1.0f,
						tall > 0.0f ? 0.7f * (float)s_text.h / tall : 1.0f);
	style.tint[0] = style.tint[1] = style.tint[2] = style.tint[3] = 1.0f;
	style.filter = AERON_BLIT2D_FILTER_LINEAR;
	AeronDrawList_Begin(s_text.list, s_text.rt, s_text.w, s_text.h, AERON_DRAWLIST2D_CLEAR, NULL);
	const float k = style.scale;
	const float y = 0.5f * ((float)s_text.h - tall * k);
	float x = 0.5f * ((float)s_text.w - wide * k);
	AeronText_Draw(s_text.list, &style, x, y, AERON_TEXT_LEFT, before, -1, NULL);
	x += AeronText_MeasureWidth(&style, before, -1) + space * k;
	art_draw_key_ring(s_text.list, &style, key, x + 0.5f * ring * k, 0.5f * (float)s_text.h, ring * k);
	x += (ring + space) * k;
	AeronText_Draw(s_text.list, &style, x, y, AERON_TEXT_LEFT, after, -1, NULL);
	AeronDrawList_Render(s_text.list, cmd);
}

/* The logo large where the screen will be, the prompt under it, gently
 * pulsing; both fade out as the screen comes in. */
static void art_submit_title(AeronScene3D* scene) {
	if (!art_title_shown() || !s_art.have_screen)
		return;
	const float a = art_smooth(s_art.shown_t / art_env("XWA_XR_TITLE_FADE_SECONDS", 1.2f)) *
					(1.0f - TieXrArt_ScreenReveal());
	art_submit_logo_at(scene, a, true);
	if (!s_text.rt || a <= 0.0f)
		return;
	const TieXrArtScreen* sc = &s_art.screen;
	const float f = sc->frame_scale, k = sc->units;
	const float logo_h = 0.9f * sc->pic_w * f * art_env("XWA_XR_LOGO_SCALE", 1.0f) *
						 (float)s_art.tex_h[ART_TEX_LOGO] / (float)s_art.tex_w[ART_TEX_LOGO];
	const float tw = 0.55f * sc->pic_w * f, th = tw * (float)s_text.h / (float)s_text.w;
	const float up = sc->lift * f - 0.5f * logo_h - 0.03f - 0.5f * th;
	const float center[3] = { s_art.view_pos[0], s_art.view_pos[1] + (sc->frame_dist + 0.02f) * k,
							  s_art.view_pos[2] + up * k };
	const float ax[3] = { 0.5f * tw * k, 0.0f, 0.0f };
	const float ay[3] = { 0.0f, 0.0f, 0.5f * th * k };
	float c[4][3];
	art_rect(center, ax, ay, c);
	const float pulse = a * (0.75f + 0.25f * (0.5f + 0.5f * sinf(2.0f * ART_PI * 0.6f * s_art.shown_t)));
	const float tint[4] = { 1.15f, 1.2f, 1.3f, pulse };
	art_quad_tex(scene, Aeron_RenderTargetGetTexture(s_text.rt), AERON_SCENE_BILLBOARD_STAGE_OVERLAY,
				 AERON_SCENE_BILLBOARD_BLEND_PMA, c, tint);
}

void TieXrArt_Submit(AeronScene3D* scene, bool ships, float fade) {
	if (!scene)
		return;
	s_fade = fade;
	TieXrArt_ViewPos(s_art.view_pos);
	if (ships) {
		for (int i = 0; i < ART_SHIPS; ++i) {
			const struct AeronSceneMesh* mesh = s_art.models[k_ships[i].model].mesh;
			if (!mesh || s_art.hidden[i])
				continue;
			float m[16];
			if (s_art.posed) {
				memcpy(m, s_art.transform[i], sizeof m);
			} else {
				float pos[3], n[3], r[3], u[3];
				art_ship_pos(i, pos);
				art_ship_basis(i, n, r, u);
				art_pose_matrix(n, u, pos, m);
			}
			/* Centred on its bounds as XWA's OPTs are (TIE Fighter's own sit
			 * off their origin, the Interceptor 2 m toward its nose). */
			const float* b = s_art.models[k_ships[i].model].bounds;
			const float c[3] = { 0.5f * (b[0] + b[3]), 0.5f * (b[1] + b[4]), 0.5f * (b[2] + b[5]) };
			for (int row = 0; row < 3; ++row)
				m[row * 4 + 3] -= m[row * 4 + 0] * c[0] + m[row * 4 + 1] * c[1] + m[row * 4 + 2] * c[2];
			AeronSceneMeshInstance in;
			memset(&in, 0, sizeof in);
			in.mesh = mesh;
			memcpy(in.transform, m, sizeof in.transform);
			memcpy(in.prev_transform, m, sizeof in.prev_transform);
			in.zero_velocity = 1;
			in.no_local_lights = 1;
			if (mesh->all_materials_single_sided)
				in.cull_mode = AERON_CULL_BACK;
			in.shadow_flags = AERON_SCENE_INSTANCE_NO_CAST_SHADOW | AERON_SCENE_INSTANCE_NO_RECEIVE_SHADOW;
			AeronScene_AddMeshInstance(scene, &in);
		}
	}
	art_submit_sprites(scene, ships);
	/* The logo above the frame: at the cover's moment (in with the screen),
	 * and throughout when the screen is up from the start; the title. */
	if (!ships || s_art.anim_t >= ART_ANIM_SECONDS || TieXrArt_ScreenReveal() >= 1.0f)
		art_submit_logo_at(scene, TieXrArt_ScreenReveal(), false);
	art_submit_title(scene);
}

void TieXrArt_Lighting(TieXrArtLighting* out) {
	/* The TIE Fighter cover: a cool blue-white light from the upper right, the
	 * explosion's orange glow from behind and below the craft, a violet
	 * ambient. */
	const float sun = art_env("XWA_XR_ENDOR_SUN", 1.6f);
	memset(out, 0, sizeof *out);
	const float key_dir[3] = { 0.45f, 0.25f, 0.85f };
	const float key[3] = { 0.80f * sun, 0.85f * sun, 1.0f * sun };
	const float fill_dir[3] = { 0.0f, 0.9f, -0.45f };
	const float fill[3] = { 1.0f * 0.9f, 0.55f * 0.9f, 0.2f * 0.9f };
	const float sides[3] = { 0.05f, 0.04f, 0.08f }, top[3] = { 0.05f, 0.04f, 0.09f },
				bottom[3] = { 0.05f, 0.03f, 0.07f };
	memcpy(out->key_dir, key_dir, sizeof key_dir);
	art_norm(out->key_dir);
	memcpy(out->key_color, key, sizeof key);
	memcpy(out->fill_dir, fill_dir, sizeof fill_dir);
	art_norm(out->fill_dir);
	memcpy(out->fill_color, fill, sizeof fill);
	memcpy(out->ambient_sides, sides, sizeof sides);
	memcpy(out->ambient_top, top, sizeof top);
	memcpy(out->ambient_bottom, bottom, sizeof bottom);
}

/* ---- loading ---------------------------------------------------------------- */

bool TieXrArt_Prepare(AeronCommandBuffer* cmd, const TieFlightAssetSource* assets, bool ships) {
	if (!cmd)
		return false;
	if (ships) {
		for (int m = 0; m < ART_MODEL_COUNT; ++m) {
			if (s_art.models[m].tried)
				continue;
			s_art.models[m].tried = true;
			if (assets)
				s_art.models[m].mesh =
					TieFlightXrEnv_LoadModel(cmd, assets, k_art_species[m], s_art.models[m].bounds);
			return false;
		}
	}
	if (!s_art.generated_tried) {
		s_art.generated_tried = true;
		art_make_beam(cmd);
		art_make_glow(cmd);
		art_make_nebula(cmd);
		return false;
	}
	if (!s_art.vader_tried) {
		s_art.vader_tried = true;
		art_load_rgba(cmd, ART_TEX_VADER, "vader", "XWA_XR_VADER");
		return false;
	}
	if (!s_art.logo_tried) {
		s_art.logo_tried = true;
		art_load_rgba(cmd, ART_TEX_LOGO, "logo", "XWA_XR_LOGO");
		return false;
	}
	if (s_art.tex[ART_TEX_LOGO] && art_title_active() && !s_text.rt && s_text.font_state >= 0) {
		art_title_text(cmd); /* the font, then the prompt */
		return false;
	}
	if (!s_art.sprites_tried && assets) {
		s_art.sprites_tried = true;
		art_load_sprites(cmd, assets);
		return false;
	}
	if (!s_art.sounds_tried && assets) {
		s_art.sounds_tried = true;
		art_load_sounds(assets);
		return false;
	}
	if (ships && !s_art.logged) {
		s_art.logged = true;
		static const char* const names[ART_MODEL_COUNT] = { "T/I", "T/F", "T/B", "X-W", "A-W" };
		for (int m = 0; m < ART_MODEL_COUNT; ++m) {
			const float* b = s_art.models[m].bounds;
			Aeron_LogInfo("tie.vr", "menu scene (TIE Fighter): %s %s, %.1f long, %.1f wide, %.1f high (m)",
						  names[m], s_art.models[m].mesh ? "loaded" : "missing", (double)(b[4] - b[1]),
						  (double)(b[3] - b[0]), (double)(b[5] - b[2]));
		}
		Aeron_LogInfo("tie.vr",
					  "menu scene (TIE Fighter): logo %s, helmet %s, explosion %s, fire %s, sound level %.2f",
					  s_art.tex[ART_TEX_LOGO] ? "yes" : "no", s_art.tex[ART_TEX_VADER] ? "yes" : "no", s_art.tex[ART_TEX_BLAST] ? "yes" : "no",
					  s_art.tex[ART_TEX_FIRE] ? "yes" : "no", (double)es_master());
	}
	return true;
}

void TieXrArt_Restart(void) {
	es_stop();
	s_art.anim_t = 0.0f;
	s_art.anim_paths = 0;
	s_art.posed = false;
	s_art.fixed = false;
	s_art.hunter = -1;
	memset(s_art.bolts, 0, sizeof s_art.bolts);
	memset(s_art.hidden, 0, sizeof s_art.hidden);
	memset(s_art.runner, 0, sizeof s_art.runner);
	memset(s_art.anim_fl, 0, sizeof s_art.anim_fl);
}

bool TieXrArt_ReleaseShips(void) {
	bool released = s_art.anim_paths || s_art.posed;
	TieXrArt_Restart();
	for (int m = 0; m < ART_MODEL_COUNT; ++m) {
		released |= s_art.models[m].tried;
		if (s_art.models[m].mesh)
			AeronScene_MeshDestroy(s_art.models[m].mesh);
		s_art.models[m].mesh = NULL;
		s_art.models[m].tried = false;
	}
	s_art.logged = false;
	return released;
}

void TieXrArt_Shutdown(void) {
	(void)TieXrArt_ReleaseShips();
	for (int i = 0; i < ART_TEX_COUNT; ++i)
		if (s_art.tex[i])
			Aeron_DestroyTexture(s_art.tex[i]);
	for (int i = 0; i < ES_COUNT; ++i)
		if (s_art.clip[i])
			Aeron_AudioClipDestroy(s_art.clip[i]);
	if (s_text.list)
		AeronDrawList_Destroy(s_text.list);
	if (s_text.rt)
		Aeron_DestroyRenderTarget(s_text.rt);
	if (s_text.font_state > 0)
		AeronFontAtlas_Release(&s_text.font);
	memset(&s_text, 0, sizeof s_text);
	const bool title_done = s_art.title_done;
	memset(&s_art, 0, sizeof s_art);
	s_art.hunter = -1;
	s_art.title_done = title_done;
}

/* 3D cockpits for the player's TIE craft: X-Wing Alliance's cockpit OPTs when
 * the player copied them over, else OpenTIE-VR's own generated cockpit shell
 * (resources/cockpit/opentie_cockpit_fighter.glb, tools/cockpit_shell). See
 * xwa_cockpit.h.
 *
 * Coordinates: cockpit OPTs are authored in the craft's own OPT frame (X right,
 * -Y forward, Z up; XWA draws them at the craft origin with the eye at the
 * craft's cockpit POV). The meshes keep that frame in metres, so the instance
 * transform is the ordinary ship transform (TieRenderMath_Mat4FromQuaternion-
 * Translation, which applies the -Y = forward reflection) of the player
 * craft, translated so the table's eye point lands on the flight camera. The
 * own shell is a glTF whose axes Aeron's loader converts to the same frame
 * (aeron x, y, z = -gltf x, -gltf z, gltf y); its eye point is the origin. */

#include "tie_remaster/flight/xwa_cockpit.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#include "aeron/asset/flight_model.h"
#include "aeron/asset/opt_model.h"
#include "aeron/log.h"
#include "aeron/scene/billboard.h"
#include "aeron/scene/mesh.h"
#include "aeron/scene/scene3d.h"
#include "aeron/scene/world.h"
#include "aeron/vfs.h"
#include "cgltf.h"
#include "tie_remaster/flight/cockpit/renderer.h"
#include "tie_remaster/flight/render_math.h"
#include "tie_runtime/flight_assets/source.h"
#include "tie_runtime/snapshot/snapshot.h"
#include "tie_runtime/species_id.h"

#define XWA_COCKPIT_NAME_MAX 32

/* OpenTIE-VR's own cockpit shell: its name in the table below (and in the
 * TIE_XR_COCKPIT_*_<NAME> / XWA_XR_HUD_POSE_<NAME> keys) and its file in the
 * app resources (packaged into the APK with the rest of resources/). */
#define TIE_OWN_COCKPIT_NAME "OPENTIE_FIGHTER"
#define TIE_OWN_COCKPIT_RESOURCE "cockpit/opentie_cockpit_fighter.glb"
#define TIE_OWN_COCKPIT_MAX_BYTES (16u * 1024u * 1024u)

/* One instrument panel on a cockpit model: centre in model OPT metres, the
 * panel plane's tilt and turn, and its height (width follows the source
 * rectangle's aspect). pitch 0 = upright facing the pilot, 90 = lying flat
 * with its top edge forward; yaw turns it about the up axis (positive turns
 * its face toward +X, the pilot's right); roll about its own normal. */
typedef struct TieXwaPanelPose {
	bool shown;
	float center[3];
	float pitch_deg, yaw_deg, roll_deg;
	float height_m;
} TieXwaPanelPose;

typedef struct TieXwaCockpitSpec {
	const char* name;
	/* Seat eye point in the model, OPT metres (x right, y back, z up). */
	float eye[3];
	float scale;
	TieXwaPanelPose panels[TIE_COCKPIT_PANEL_COUNT];
} TieXwaCockpitSpec;

/* Eye points come from the model geometry (XWA keeps the POV in the craft
 * data, not in the cockpit OPT): TIECOCKPIT is a 1.46 m ball centred on the
 * origin whose round front window is centred at eye height; the seat back is
 * at y +0.44. TIEBOMBERCOCKPIT is the same pod 1.845 m to the craft's right.
 * Panels: the sensors lie on the dashboard shelf (81 degrees back, 0.6 m below
 * the eye) over its painted sensor rings, the gauges on the shelf's triangle
 * plates nearer the pilot, the CMD upright on the front of the centre console. */
static const TieXwaCockpitSpec k_cockpits[] = {
	{
		"TIECOCKPIT",
		{ 0.0f, 0.15f, 0.0f },
		1.0f,
		{
			[TIE_COCKPIT_PANEL_SENSOR_FWD] = { true, { -0.234f, -0.747f, -0.612f }, 81.0f, 0.0f, 0.0f, 0.16f },
			[TIE_COCKPIT_PANEL_SENSOR_REAR] = { true, { 0.231f, -0.747f, -0.612f }, 81.0f, 0.0f, 0.0f, 0.16f },
			[TIE_COCKPIT_PANEL_CMD] = { true, { 0.0f, -0.60f, -0.525f }, 0.0f, 0.0f, 0.0f, 0.17f },
			[TIE_COCKPIT_PANEL_GAUGES_L] = { true, { -0.40f, -0.52f, -0.652f }, 81.0f, 0.0f, 0.0f, 0.20f },
			[TIE_COCKPIT_PANEL_GAUGES_R] = { true, { 0.40f, -0.52f, -0.652f }, 81.0f, 0.0f, 0.0f, 0.20f },
		},
	},
	{
		"TIEBOMBERCOCKPIT",
		{ 1.845f, 0.16f, 0.0f },
		1.0f,
		{
			[TIE_COCKPIT_PANEL_SENSOR_FWD] = { true, { 1.611f, -0.737f, -0.612f }, 81.0f, 0.0f, 0.0f, 0.16f },
			[TIE_COCKPIT_PANEL_SENSOR_REAR] = { true, { 2.076f, -0.737f, -0.612f }, 81.0f, 0.0f, 0.0f, 0.16f },
			[TIE_COCKPIT_PANEL_CMD] = { true, { 1.845f, -0.59f, -0.525f }, 0.0f, 0.0f, 0.0f, 0.17f },
			[TIE_COCKPIT_PANEL_GAUGES_L] = { true, { 1.445f, -0.51f, -0.652f }, 81.0f, 0.0f, 0.0f, 0.20f },
			[TIE_COCKPIT_PANEL_GAUGES_R] = { true, { 2.245f, -0.51f, -0.652f }, 81.0f, 0.0f, 0.0f, 0.20f },
		},
	},
	{
		"COMBATSIMCOCKPIT",
		{ 0.0f, 0.20f, 0.20f },
		1.0f,
		{
			[TIE_COCKPIT_PANEL_SENSOR_FWD] = { true, { -0.28f, -0.60f, -0.28f }, 10.0f, 15.0f, 0.0f, 0.15f },
			[TIE_COCKPIT_PANEL_SENSOR_REAR] = { true, { 0.28f, -0.60f, -0.28f }, 10.0f, -15.0f, 0.0f, 0.15f },
			[TIE_COCKPIT_PANEL_CMD] = { true, { 0.02f, -0.565f, -0.30f }, 0.0f, 0.0f, 0.0f, 0.16f },
			[TIE_COCKPIT_PANEL_GAUGES_L] = { true, { -0.28f, -0.61f, -0.43f }, 25.0f, 15.0f, 0.0f, 0.13f },
			[TIE_COCKPIT_PANEL_GAUGES_R] = { true, { 0.28f, -0.61f, -0.43f }, 25.0f, -15.0f, 0.0f, 0.13f },
		},
	},
	/* OpenTIE-VR's own shell (tools/cockpit_shell/make_cockpit.py --spec):
	 * the eye at the origin, the sensors either side of the CMD on the sloped
	 * dashboard face, the gauges on pods angled in from the side consoles. */
	{
		TIE_OWN_COCKPIT_NAME,
		{ 0.0f, 0.0f, 0.0f },
		1.0f,
		{
			[TIE_COCKPIT_PANEL_SENSOR_FWD] = { true, { -0.255f, -0.738f, -0.421f }, 40.9f, 0.0f, 0.0f, 0.16f },
			[TIE_COCKPIT_PANEL_SENSOR_REAR] = { true, { 0.255f, -0.738f, -0.421f }, 40.9f, 0.0f, 0.0f, 0.16f },
			[TIE_COCKPIT_PANEL_CMD] = { true, { 0.000f, -0.730f, -0.430f }, 40.9f, 0.0f, 0.0f, 0.17f },
			[TIE_COCKPIT_PANEL_GAUGES_L] = { true, { -0.445f, -0.600f, -0.470f }, 42.0f, 30.0f, 0.0f, 0.22f },
			[TIE_COCKPIT_PANEL_GAUGES_R] = { true, { 0.445f, -0.600f, -0.470f }, 42.0f, -30.0f, 0.0f, 0.22f },
		},
	},
};

static const char* const k_panel_ids[TIE_COCKPIT_PANEL_COUNT] = {
	[TIE_COCKPIT_PANEL_SENSOR_FWD] = "sensor_fwd", [TIE_COCKPIT_PANEL_SENSOR_REAR] = "sensor_rear",
	[TIE_COCKPIT_PANEL_CMD] = "cmd",			   [TIE_COCKPIT_PANEL_GAUGES_L] = "gauges_l",
	[TIE_COCKPIT_PANEL_GAUGES_R] = "gauges_r",
};

typedef struct TieXwaCockpitState {
	AeronSceneMesh* mesh;
	/* Loaded model and what it was chosen for. */
	char name[XWA_COCKPIT_NAME_MAX];
	const TieXwaCockpitSpec* spec;
	uint32_t mission_gen;
	uint8_t species;
	/* Last failed choice (species, generation), to not retry every frame. */
	bool failed;
	uint8_t failed_species;
	uint32_t failed_gen;
	/* Live values (spec + env overrides). */
	float eye[3];
	float scale;
	TieXwaPanelPose panels[TIE_COCKPIT_PANEL_COUNT];
	char panel_env[512];
} TieXwaCockpitState;

static TieXwaCockpitState s_xwa;

typedef enum TieXwaCockpitMode {
	TIE_XWA_COCKPIT_MODE_OFF, /* TIE's 2D cockpit art */
	TIE_XWA_COCKPIT_MODE_XWA, /* XWA's cockpit OPTs, the own shell without them */
	TIE_XWA_COCKPIT_MODE_OWN, /* always the own shell */
} TieXwaCockpitMode;

static TieXwaCockpitMode TieXwaCockpit_Mode(void) {
	const char* mode = SDL_getenv("TIE_XR_COCKPIT");
	if (mode && (SDL_strcasecmp(mode, "2d") == 0 || SDL_strcmp(mode, "0") == 0 || SDL_strcasecmp(mode, "off") == 0))
		return TIE_XWA_COCKPIT_MODE_OFF;
	if (mode && SDL_strcasecmp(mode, "own") == 0)
		return TIE_XWA_COCKPIT_MODE_OWN;
	/* Unset: 3D when the app named an XWA cockpit folder (the VR app always
	 * does, even when it is empty); a desktop run without one keeps TIE's 2D
	 * cockpit as before. */
	if (!(mode && mode[0])) {
		const char* dir = SDL_getenv("TIE_XWA_COCKPITS");
		if (!(dir && dir[0]))
			return TIE_XWA_COCKPIT_MODE_OFF;
	}
	return TIE_XWA_COCKPIT_MODE_XWA;
}

static bool TieXwaCockpit_Enabled(void) {
	return TieXwaCockpit_Mode() != TIE_XWA_COCKPIT_MODE_OFF;
}

static const char* TieXwaCockpit_NameForSpecies(uint8_t species) {
	switch (species) {
		case TIE_SPECIES_TIE_BOMBER:
			return "TIEBOMBERCOCKPIT";
		case TIE_SPECIES_ASSAULT_GUNBOAT:
		case TIE_SPECIES_MISSILE_BOAT:
			return "COMBATSIMCOCKPIT";
		case TIE_SPECIES_TIE_FIGHTER:
		case TIE_SPECIES_TIE_INTERCEPTOR:
		case TIE_SPECIES_TIE_ADVANCED:
		case TIE_SPECIES_TIE_DEFENDER:
		default:
			return "TIECOCKPIT";
	}
}

static const TieXwaCockpitSpec* TieXwaCockpit_Spec(const char* name) {
	for (size_t i = 0; i < sizeof k_cockpits / sizeof k_cockpits[0]; ++i)
		if (SDL_strcasecmp(k_cockpits[i].name, name) == 0)
			return &k_cockpits[i];
	return &k_cockpits[0];
}

/* The camera's own craft in cockpit views (its hull is hidden there). */
static const TieFlightObjectState* TieXwaCockpit_PlayerFlight(const TieSnapshot* snap) {
	const uint16_t slot = snap->camera.target_obj_slot;
	if (slot == 0xFFFFu)
		return NULL;
	for (uint16_t i = 0; i < snap->flight_count; ++i)
		if (snap->flights[i].slot == slot)
			return &snap->flights[i];
	return NULL;
}

/* Cockpit view (not external, zoom or replay) with the panel shown: TIE hides
 * its panel in views 17 (title), 18 (no panel) and 19 (forward HUD only). */
static bool TieXwaCockpit_ViewShowsCockpit(const TieSnapshot* snap) {
	if (!snap || snap->scene_kind != TIE_SCENE_FLIGHT || snap->replay_mode == 2)
		return false;
	if (snap->camera.zoom_active)
		return false;
	const uint8_t view = snap->camera.pilotview;
	if (view == 17 || view == 18 || view == 19)
		return false;
	const TieFlightObjectState* fl = TieXwaCockpit_PlayerFlight(snap);
	return fl && fl->genus != TIE_GENUS_EXPLOSION;
}

static void* TieXwaCockpit_ReadFile(const char* dir, const char* name, size_t* size, char* path, size_t path_cap) {
	char lower[XWA_COCKPIT_NAME_MAX];
	size_t n = 0;
	for (; name[n] && n + 1 < sizeof lower; ++n)
		lower[n] = (char)tolower((unsigned char)name[n]);
	lower[n] = '\0';
	const char* const forms[][2] = { { name, ".OPT" }, { name, ".opt" }, { lower, ".opt" } };
	for (size_t i = 0; i < sizeof forms / sizeof forms[0]; ++i) {
		snprintf(path, path_cap, "%s/%s%s", dir, forms[i][0], forms[i][1]);
		void* data = SDL_LoadFile(path, size);
		if (data)
			return data;
	}
	return NULL;
}

static float TieXwaCockpit_EnvFloat(const char* key, float fallback) {
	const char* v = SDL_getenv(key);
	if (!v || !v[0])
		return fallback;
	char* end = NULL;
	const double d = SDL_strtod(v, &end);
	return end != v ? (float)d : fallback;
}

/* id=x,y,z,pitch,yaw,roll,height;... (or id=off). */
static void TieXwaCockpit_ApplyPanelEnv(const char* text) {
	while (text && *text) {
		const char* end = strchr(text, ';');
		const size_t len = end ? (size_t)(end - text) : strlen(text);
		char item[128];
		if (len > 0 && len < sizeof item) {
			memcpy(item, text, len);
			item[len] = '\0';
			char* eq = strchr(item, '=');
			if (eq) {
				*eq = '\0';
				for (int p = 0; p < TIE_COCKPIT_PANEL_COUNT; ++p) {
					if (SDL_strcasecmp(item, k_panel_ids[p]) != 0)
						continue;
					TieXwaPanelPose* pose = &s_xwa.panels[p];
					if (SDL_strcasecmp(eq + 1, "off") == 0) {
						pose->shown = false;
						break;
					}
					float v[7];
					if (sscanf(eq + 1, "%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) == 7) {
						*pose = (TieXwaPanelPose) { true, { v[0], v[1], v[2] }, v[3], v[4], v[5], v[6] };
					}
					break;
				}
			}
		}
		text = end ? end + 1 : NULL;
	}
}

/* Spec values with env overrides; cheap, refreshed every frame so tuning
 * through the environment (or the settings page) is live. */
static void TieXwaCockpit_RefreshTuning(void) {
	const TieXwaCockpitSpec* spec = s_xwa.spec;
	if (!spec)
		return;
	char key[96];
	memcpy(s_xwa.eye, spec->eye, sizeof s_xwa.eye);
	snprintf(key, sizeof key, "TIE_XR_COCKPIT_EYE_%s", s_xwa.name);
	const char* eye = SDL_getenv(key);
	if (eye) {
		float v[3];
		if (sscanf(eye, "%f,%f,%f", &v[0], &v[1], &v[2]) == 3)
			memcpy(s_xwa.eye, v, sizeof v);
	}
	snprintf(key, sizeof key, "TIE_XR_COCKPIT_SCALE_%s", s_xwa.name);
	s_xwa.scale = TieXwaCockpit_EnvFloat(key, spec->scale);
	if (!(s_xwa.scale > 0.1f && s_xwa.scale < 10.0f))
		s_xwa.scale = 1.0f;
	memcpy(s_xwa.panels, spec->panels, sizeof s_xwa.panels);
	snprintf(key, sizeof key, "TIE_XR_PANEL_%s", s_xwa.name);
	TieXwaCockpit_ApplyPanelEnv(SDL_getenv(key));
}

void TieXwaCockpit_Release(void) {
	if (s_xwa.mesh)
		AeronScene_MeshDestroy(s_xwa.mesh);
	memset(&s_xwa, 0, sizeof s_xwa);
}

/* Scene mesh from a built model; frees the model. */
static bool TieXwaCockpit_Adopt(AeronCommandBuffer* cmd, AeronFlightModel* model, const char* name, const char* path) {
	AeronSceneMeshCreateStatus status = AERON_SCENE_MESH_CREATE_SUCCESS;
	char label[64];
	snprintf(label, sizeof label, "xwa.cockpit.%s", name);
	AeronSceneMesh* mesh = AeronScene_MeshCreate(cmd, model, label, &status);
	Aeron_FlightModelFree(model);
	if (!mesh) {
		Aeron_LogWarn("tie.xwa_cockpit", "%s: scene mesh creation failed (%d)", path, (int)status);
		return false;
	}
	s_xwa.mesh = mesh;
	snprintf(s_xwa.name, sizeof s_xwa.name, "%s", name);
	s_xwa.spec = TieXwaCockpit_Spec(name);
	Aeron_LogInfo("tie.xwa_cockpit", "loaded %s: %u vertices, %u primitives, bounds (%.2f %.2f %.2f)..(%.2f %.2f %.2f) m",
				  path, mesh->vertex_count, mesh->total_prim_count, mesh->bound_min[0], mesh->bound_min[1],
				  mesh->bound_min[2], mesh->bound_max[0], mesh->bound_max[1], mesh->bound_max[2]);
	return true;
}

static bool TieXwaCockpit_Load(AeronCommandBuffer* cmd, const TieFlightAssetSource* assets, const char* name) {
	const char* dir = SDL_getenv("TIE_XWA_COCKPITS");
	if (!dir || !dir[0])
		return false;
	char path[1024];
	size_t size = 0;
	void* bytes = TieXwaCockpit_ReadFile(dir, name, &size, path, sizeof path);
	if (!bytes) {
		Aeron_LogWarn("tie.xwa_cockpit", "%s/%s.OPT not found", dir, name);
		return false;
	}
	AeronFlightModel model;
	memset(&model, 0, sizeof model);
	AeronOptModelError err = { 0 };
	const bool built = Aeron_OptModelBuildMemory(bytes, size, name,
												 &(AeronOptModelBuildOptions) {
													 .smooth_angle_degrees = assets ? assets->smooth_angle_degrees : 0.0f,
													 .emissive_strength = 1.5f,
													 .emissive = true,
												 },
												 &model, &err);
	SDL_free(bytes);
	if (!built) {
		Aeron_LogWarn("tie.xwa_cockpit", "%s: OPT conversion failed: %s", path, err.message);
		return false;
	}
	return TieXwaCockpit_Adopt(cmd, &model, name, path);
}

/* The own cockpit shell: TIE_OWN_COCKPIT_GLB=<path> (testing), else the app
 * resource (next to the executable on desktop, extracted from the APK on the
 * Quest). The .glb is cooked (KTX2 channel atlases), see tools/cockpit_shell. */
static bool TieXwaCockpit_LoadOwn(AeronCommandBuffer* cmd, const TieFlightAssetSource* assets) {
	const char* override = SDL_getenv("TIE_OWN_COCKPIT_GLB");
	char path[1024];
	void* bytes = NULL;
	size_t size = 0;
	bool sdl_owned = false;
	if (override && override[0]) {
		snprintf(path, sizeof path, "%s", override);
		bytes = SDL_LoadFile(path, &size);
		sdl_owned = true;
	} else {
		snprintf(path, sizeof path, "resources/%s", TIE_OWN_COCKPIT_RESOURCE);
		uint8_t* data = NULL;
		if (assets && assets->vfs &&
			AeronVfs_ReadAll(assets->vfs, AERON_VFS_ROOT_RESOURCE, TIE_OWN_COCKPIT_RESOURCE, TIE_OWN_COCKPIT_MAX_BYTES,
							 &data, &size))
			bytes = data;
	}
	if (!bytes) {
		Aeron_LogWarn("tie.xwa_cockpit", "own cockpit %s not found", path);
		return false;
	}
	/* A binary glTF's buffer is its BIN chunk, which cgltf attaches in
	 * cgltf_load_buffers (Aeron_FlightModelBuildMemory expects it attached). */
	AeronFlightModel model;
	memset(&model, 0, sizeof model);
	cgltf_options options;
	memset(&options, 0, sizeof options);
	cgltf_data* data = NULL;
	bool built = false;
	if (cgltf_parse(&options, bytes, size, &data) == cgltf_result_success &&
		cgltf_load_buffers(&options, data, NULL) == cgltf_result_success)
		built = Aeron_FlightModelBuildData(data, path, &model);
	if (data)
		cgltf_free(data);
	if (sdl_owned)
		SDL_free(bytes);
	else
		free(bytes);
	if (!built) {
		Aeron_LogWarn("tie.xwa_cockpit", "%s: not a usable cockpit glTF", path);
		return false;
	}
	return TieXwaCockpit_Adopt(cmd, &model, TIE_OWN_COCKPIT_NAME, path);
}

void TieXwaCockpit_Prepare(AeronCommandBuffer* cmd, const TieFlightAssetSource* assets, const TieSnapshot* snap) {
	if (!cmd || !snap || snap->scene_kind != TIE_SCENE_FLIGHT)
		return;
	if (!TieXwaCockpit_Enabled()) {
		if (s_xwa.mesh)
			TieXwaCockpit_Release();
		return;
	}
	/* TIE95 renders with the classic palette path, which has no scene meshes. */
	if (!assets || TieFlightAssetSource_IsTie95(assets))
		return;
	const TieFlightObjectState* fl = TieXwaCockpit_ViewShowsCockpit(snap) ? TieXwaCockpit_PlayerFlight(snap) : NULL;
	if (!fl)
		return;
	const uint8_t species = fl->ship_idx;
	const uint32_t gen = snap->mission_load_generation;
	if (s_xwa.mesh && s_xwa.species == species && s_xwa.mission_gen == gen)
		return;
	if (s_xwa.failed && s_xwa.failed_species == species && s_xwa.failed_gen == gen)
		return;

	/* TIE_XR_COCKPIT_FILE=<NAME> forces one cockpit for every craft. */
	const char* forced = SDL_getenv("TIE_XR_COCKPIT_FILE");
	const bool own_only = TieXwaCockpit_Mode() == TIE_XWA_COCKPIT_MODE_OWN;
	const char* want = own_only					? TIE_OWN_COCKPIT_NAME
					   : forced && forced[0] ? forced
											 : TieXwaCockpit_NameForSpecies(species);
	if (s_xwa.mesh && strcmp(s_xwa.name, want) == 0) {
		/* Same model for the new mission or craft: keep it. */
		s_xwa.species = species;
		s_xwa.mission_gen = gen;
		return;
	}
	TieXwaCockpit_Release();
	bool ok = false;
	if (strcmp(want, TIE_OWN_COCKPIT_NAME) != 0) {
		ok = TieXwaCockpit_Load(cmd, assets, want);
		if (!ok && strcmp(want, "TIECOCKPIT") != 0)
			ok = TieXwaCockpit_Load(cmd, assets, "TIECOCKPIT");
	}
	/* No X-Wing Alliance cockpit (not copied over): the own shell. */
	if (!ok)
		ok = TieXwaCockpit_LoadOwn(cmd, assets);
	if (!ok) {
		s_xwa.failed = true;
		s_xwa.failed_species = species;
		s_xwa.failed_gen = gen;
		Aeron_LogWarn("tie.xwa_cockpit", "no 3D cockpit for species %u; keeping the 2D cockpit", (unsigned)species);
		return;
	}
	s_xwa.species = species;
	s_xwa.mission_gen = gen;
}

bool TieXwaCockpit_PanelView(const TieSnapshot* snap) {
	return TieXwaCockpit_ViewShowsCockpit(snap);
}

const char* TieXwaCockpit_ModelName(void) {
	return s_xwa.mesh ? s_xwa.name : "";
}

bool TieXwaCockpit_Drawn(const TieSnapshot* snap) {
	return s_xwa.mesh && TieXwaCockpit_Enabled() && TieXwaCockpit_ViewShowsCockpit(snap) &&
		   snap->mission_load_generation == s_xwa.mission_gen;
}

/* Model point (OPT metres) to scene-local position through the instance. */
static void TieXwaCockpit_ModelToLocal(const float m[16], const float p[3], float out[3]) {
	for (int r = 0; r < 3; ++r)
		out[r] = m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
}

static void TieXwaCockpit_SubmitPanels(AeronScene3D* scene, const float m[16],
									   const TieCockpitRenderer* instruments) {
	/* VR cockpit HUD: the instruments float in front of the cockpit instead
	 * (TIE_XR_HUD_LAYOUT=dashboard keeps them on the dashboard). */
	if (!instruments || TieCockpitRenderer_XrPanels(instruments))
		return;
	TieCockpitPanelSource src[TIE_COCKPIT_PANEL_COUNT];
	AeronTexture* atlas = TieCockpitRenderer_InstrumentAtlas(instruments, src);
	if (!atlas)
		return;
	const float deg = 3.14159265f / 180.0f;
	for (int p = 0; p < TIE_COCKPIT_PANEL_COUNT; ++p) {
		const TieXwaPanelPose* pose = &s_xwa.panels[p];
		if (!pose->shown || !src[p].valid || pose->height_m <= 0.0f)
			continue;
		/* Panel frame in the model: right, down and normal (toward pilot). */
		const float sp = sinf(pose->pitch_deg * deg), cp = cosf(pose->pitch_deg * deg);
		float right[3] = { 1.0f, 0.0f, 0.0f };
		float down[3] = { 0.0f, sp, -cp };
		const float sr = sinf(pose->roll_deg * deg), cr = cosf(pose->roll_deg * deg);
		float normal[3] = { 0.0f, cp, sp };
		float r2[3], d2[3];
		for (int k = 0; k < 3; ++k) {
			r2[k] = right[k] * cr - down[k] * sr;
			d2[k] = right[k] * sr + down[k] * cr;
		}
		const float sy = sinf(pose->yaw_deg * deg), cy = cosf(pose->yaw_deg * deg);
		/* Yaw about +Z: the face (+Y toward the pilot) turns toward +X. */
		float* vecs[3] = { r2, d2, normal };
		for (int v = 0; v < 3; ++v) {
			const float x = vecs[v][0], y = vecs[v][1];
			vecs[v][0] = x * cy + y * sy;
			vecs[v][1] = -x * sy + y * cy;
		}
		const float hh = pose->height_m * 0.5f;
		const float hw = hh * (src[p].aspect > 0.0f ? src[p].aspect : 1.0f);
		/* 3 mm toward the pilot so the panel wins the depth test against the
		 * surface it lies on. */
		float c[3];
		for (int k = 0; k < 3; ++k)
			c[k] = pose->center[k] + normal[k] * 0.003f;
		const float sx[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
		const float sdn[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
		AeronSceneBillboardDesc d;
		memset(&d, 0, sizeof d);
		d.texture = atlas;
		d.blend = AERON_SCENE_BILLBOARD_BLEND_ALPHA;
		d.stage = AERON_SCENE_BILLBOARD_STAGE_OVERLAY;
		const float u[4] = { src[p].uv[0], src[p].uv[2], src[p].uv[2], src[p].uv[0] };
		const float vv[4] = { src[p].uv[1], src[p].uv[1], src[p].uv[3], src[p].uv[3] };
		for (int corner = 0; corner < 4; ++corner) {
			float pm[3];
			for (int k = 0; k < 3; ++k)
				pm[k] = c[k] + r2[k] * hw * sx[corner] + d2[k] * hh * sdn[corner];
			TieXwaCockpit_ModelToLocal(m, pm, d.corners[corner]);
			d.uv[corner][0] = u[corner];
			d.uv[corner][1] = vv[corner];
			d.colors[corner][0] = d.colors[corner][1] = d.colors[corner][2] = d.colors[corner][3] = 1.0f;
		}
		AeronScene_AddBillboard(scene, &d);
	}
}

void TieXwaCockpit_Submit(AeronScene3D* scene, const TieSnapshot* snap, const TieCockpitRenderer* instruments) {
	if (!scene || !TieXwaCockpit_Drawn(snap))
		return;
	const TieFlightObjectState* fl = TieXwaCockpit_PlayerFlight(snap);
	if (!fl)
		return;
	TieXwaCockpit_RefreshTuning();

	/* Eye point in scene-local units: the flight camera (the scene origin). */
	float eye_local[3];
	AeronWorld_LocalI32(snap->camera.world_pos, snap->camera.world_pos, eye_local);
	const float s = AERON_OPT_UNITS_PER_METER * s_xwa.scale;
	float r3[9];
	TieRenderMath_QuaternionToRenderBasis(fl->ori, r3);
	float t[3];
	for (int r = 0; r < 3; ++r)
		t[r] = eye_local[r] - s * (r3[r * 3 + 0] * s_xwa.eye[0] + r3[r * 3 + 1] * s_xwa.eye[1] +
								   r3[r * 3 + 2] * s_xwa.eye[2]);

	AeronSceneMeshInstance in;
	memset(&in, 0, sizeof in);
	in.mesh = s_xwa.mesh;
	TieRenderMath_Mat4FromQuaternionTranslation(in.transform, fl->ori, t, s);
	memcpy(in.prev_transform, in.transform, sizeof in.prev_transform);
	in.zero_velocity = 1;
	in.no_local_lights = 1;
	in.cull_mode = AERON_CULL_BACK;
	in.shadow_flags = AERON_SCENE_INSTANCE_NO_CAST_SHADOW;
	in.base_color_emissive_strength = TieXwaCockpit_EnvFloat("TIE_XR_COCKPIT_EMISSIVE", 0.0f);

	/* Test aid for the flat view: TIE_XR_COCKPIT_DEBUG_PITCH=deg turns the
	 * cockpit about the eye as if the pilot looked down that far (positive
	 * brings the dashboard up into a forward view); _DEBUG_YAW likewise. */
	const float dbg_pitch = TieXwaCockpit_EnvFloat("TIE_XR_COCKPIT_DEBUG_PITCH", 0.0f);
	const float dbg_yaw = TieXwaCockpit_EnvFloat("TIE_XR_COCKPIT_DEBUG_YAW", 0.0f);
	if (dbg_pitch != 0.0f || dbg_yaw != 0.0f) {
		for (int pass = 0; pass < 2; ++pass) {
			const float ang = (pass == 0 ? dbg_yaw : dbg_pitch) * 3.14159265f / 180.0f;
			if (ang == 0.0f)
				continue;
			/* Axis: craft up (render basis column 2) for yaw, craft right
			 * (column 0) for pitch. */
			const int col = pass == 0 ? 2 : 0;
			float a[3] = { r3[0 * 3 + col], r3[1 * 3 + col], r3[2 * 3 + col] };
			const float len = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
			for (int k = 0; k < 3; ++k)
				a[k] /= len;
			const float c = cosf(ang), sn = sinf(ang), t1 = 1.0f - c;
			const float R[9] = {
				c + a[0] * a[0] * t1,		 a[0] * a[1] * t1 - a[2] * sn, a[0] * a[2] * t1 + a[1] * sn,
				a[1] * a[0] * t1 + a[2] * sn, c + a[1] * a[1] * t1,		   a[1] * a[2] * t1 - a[0] * sn,
				a[2] * a[0] * t1 - a[1] * sn, a[2] * a[1] * t1 + a[0] * sn, c + a[2] * a[2] * t1,
			};
			float out[12];
			for (int r = 0; r < 3; ++r)
				for (int k = 0; k < 4; ++k) {
					out[r * 4 + k] = R[r * 3 + 0] * (in.transform[0 * 4 + k] - (k == 3 ? eye_local[0] : 0.0f)) +
									 R[r * 3 + 1] * (in.transform[1 * 4 + k] - (k == 3 ? eye_local[1] : 0.0f)) +
									 R[r * 3 + 2] * (in.transform[2 * 4 + k] - (k == 3 ? eye_local[2] : 0.0f)) +
									 (k == 3 ? eye_local[r] : 0.0f);
				}
			memcpy(in.transform, out, sizeof out);
		}
		memcpy(in.prev_transform, in.transform, sizeof in.prev_transform);
	}
	AeronScene_AddMeshInstance(scene, &in);

	TieXwaCockpit_SubmitPanels(scene, in.transform, instruments);
}

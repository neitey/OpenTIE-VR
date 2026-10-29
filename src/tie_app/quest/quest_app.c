/* Standalone headset (Android) startup for OpenTIE-VR. Ported from
 * OpenXWA-VR's xwa_app/main.c Android block. */

#include "tie_app/quest/quest_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "aeron/aeron.h"
#include "aeron/log.h"

#if defined(SDL_PLATFORM_ANDROID)
#include <SDL3/SDL_system.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "aeron/vfs.h"
#include "tie_app/quest/crash_report.h"
#include "tie_app/quest/error_screen.h"
#include "tie_app/quest/xr_tier.h"
#include "tie_remaster/flight/xr_env.h"

static char s_external[1024];
static char s_tie95[1100];
static char s_tie98[1100];
static char s_cfg_path[1100];

static const char k_settings_template[] =
	"# OpenTIE-VR headset settings. Remove the leading '#' to change a value.\n"
	"# (The VR settings keep their OpenXWA-VR names: the two share the VR code.)\n"
	"# Headset class, picked from the GPU (low: Quest 2, Pico 4; mid: Quest 3,\n"
	"# Pico 4 Ultra; high: Steam Frame); it sets the defaults below. Force it with:\n"
	"#XWA_XR_TIER=mid\n"
	"# Per-eye render resolution (lower = faster):\n"
	"#AERON_XR_RESOLUTION_SCALE=1.1\n"
	"# Per-eye pixel budget (low 2300000, mid 3700000, high 4600000):\n"
	"#AERON_XR_EYE_PIXELS=3700000\n"
	"# Display refresh rate in Hz (0 = the headset's setting):\n"
	"#AERON_XR_REFRESH_RATE=0\n"
	"# Fixed foveated rendering of the 3D view: 0 (off), low, medium, high:\n"
	"#XWA_XR_FOVEATION=low\n"
	"# GPU/CPU clock floor requested from the headset: 0, low, high, boost:\n"
	"#AERON_XR_PERF_LEVEL=high\n"
	"# 0 = keep the game running while the headset is off / menu open:\n"
	"#XWA_XR_AUTO_PAUSE=1\n"
	"# 0 = everything on the flat virtual screen (no 3D cockpit):\n"
	"#XWA_XR_STEREO=1\n"
	"# Lower = cockpit and ships feel bigger (try 0.8):\n"
	"#XWA_XR_WORLD_SCALE=1.0\n"
	"# Seat position in the 3D cockpits, metres further back from the window\n"
	"# (-0.2 closer .. 0.2 further; VR settings \"Seat Position\"):\n"
	"#XWA_XR_SEAT_POSITION=0\n"
	"# Stereo depth of the world outside the cockpit (1 = real):\n"
	"#XWA_XR_DEPTH_BOOST=1.0\n"
	"# Cockpit: xwa (X-Wing Alliance's 3D TIE cockpits, if the launcher copied\n"
	"# them over), revival (X-Wing Revival's 3D TIE cockpits, included), own\n"
	"# (OpenTIE-VR's own simple 3D pod) or 2d (TIE Fighter's own cockpit art on\n"
	"# a panel in front of you). Empty = xwa when its files are there, else\n"
	"# revival; a cockpit that cannot load falls back xwa -> revival -> own -> 2d:\n"
	"#TIE_XR_COCKPIT=\n"
	"# Flight HUD: cockpit (instrument panels floating around the canopy; put a\n"
	"# hand on one and squeeze the grip to move it) or helmet (2D cockpit:\n"
	"# head-locked; 3D cockpit: instruments on the dashboard):\n"
	"#XWA_XR_HUD_LAYOUT=cockpit\n"
	"# Where you put the panels, per cockpit (empty = built-in layout):\n"
	"#XWA_XR_HUD_POSE_TIECOCKPIT=\n"
	"# Screen distance and width in metres:\n"
	"#AERON_XR_SCREEN_DISTANCE=3.2\n"
	"#AERON_XR_SCREEN_WIDTH=2.1\n"
	"# Left thumbstick: degrees either side of straight forward/back that\n"
	"# move the throttle; everywhere else rolls:\n"
	"#AERON_XR_THROTTLE_ZONE=30\n"
	"# Thumbstick response in flight: 1 = direct (linear), 2 = precise (default;\n"
	"# small movements turn gently), up to 4:\n"
	"#TIE_XR_STICK_CURVE=2.0\n"
	"# 1 = right thumbstick back pushes the nose down (inverted pitch):\n"
	"#AERON_XR_INVERT_PITCH=0\n"
	"# 1 = the grabbed virtual flight stick pulled back pushes the nose down:\n"
	"#AERON_XR_INVERT_VSTICK_PITCH=0\n"
	"# 1 = left-handed controls (the controllers trade roles):\n"
	"#AERON_XR_LEFT_HANDED=0\n"
	"# 1 = the thumbsticks swap what they do in flight (left stick: pitch and\n"
	"# turn; right stick: roll and throttle); every button stays where it is:\n"
	"#XWA_XR_SWAP_STICKS=0\n"
	"# Your hands in flight: controllers, gloves or off:\n"
	"#XWA_XR_HANDS=controllers\n"
	"# 0 = no button labels when you look at a controller:\n"
	"#XWA_XR_CONTROLLER_LABELS=1\n"
	"# 1 = a flight stick right of the seat: squeeze the right grip on it to fly:\n"
	"#XWA_XR_FLIGHT_STICK=0\n"
	"# Its spot (x,y,z in metres from your head; set by moving it with the left grip):\n"
	"#XWA_XR_STICK_POS=0.12,-0.70,-0.32\n"
	"# Glove size (1 = the model's own, large):\n"
	"#XWA_XR_GLOVE_SCALE=0.85\n"
	"# Performance A/B switches (defaults shown; see the log's tie.perf and\n"
	"# vkshim lines every 5 / 10 s):\n"
	"# Instrument panel sharpness: the instrument atlas size (default 1024x768;\n"
	"# 1536x1152 is sharper and costs GPU time). Redraw at most every Nth frame\n"
	"# (0 = every frame):\n"
	"#TIE_XR_ATLAS_SIZE=1536x1152\n"
	"#TIE_XR_ATLAS_EVERY=2\n"
	"# CMD target picture: at most every Nth frame (0 = every frame):\n"
	"#TIE_XR_PIP_EVERY=2\n"
	"# 1 = laser and explosion lights (costly on the headset GPU):\n"
	"#TIE_XR_POINT_LIGHTS=0\n"
	"# 1 = the eyes render straight into the headset images (no HDR target,\n"
	"# tonemap pass or bloom; saves ~3 ms GPU per frame):\n"
	"#XWA_XR_DIRECT_OUTPUT=0\n"
	"# 0 = classic draw order (sky first; more overdraw, same image):\n"
	"#XWA_XR_DRAW_ORDER=1\n"
	"# Bloom in VR: 1 one for both eyes, eye = per eye, 0 = none:\n"
	"#XWA_XR_BLOOM=1\n"
	"# 0 = no per-pass GPU timing in the log; no CPU frame breakdown:\n"
	"#XWA_XR_GPU_TIMING=1\n"
	"#TIE_XR_PROFILE=1\n";

/* (A second literal: one string may not exceed 4095 characters.) */
static const char k_settings_template_scenes[] =
	"# Menu scene: tiefighter (default), defender, balance, xwing, xvt, random,\n"
	"# 1 (Imperial fleet), 0 (stars only) or off:\n"
	"#XWA_XR_ENVIRONMENT=tiefighter\n"
	"# 0 = the opening crawl on the screen instead of as a 3D scene around you:\n"
	"#XWA_XR_INTRO_CRAWL=1\n";

static void quest_load_settings(void) {
	char path[1100];
	char line[2048];
	if (!s_external[0])
		return;
	if (!SDL_getenv("AERON_LOG_FILE")) {
		char prev[1100];
		snprintf(path, sizeof path, "%s/opentie.log", s_external);
		snprintf(prev, sizeof prev, "%s/opentie-prev.log", s_external);
		remove(prev);
		rename(path, prev);
		SDL_setenv_unsafe("AERON_LOG_FILE", path, 1);
	}
	snprintf(path, sizeof path, "%s/opentie-crash.txt", s_external);
	TieCrashReport_Install(path);
	snprintf(s_cfg_path, sizeof s_cfg_path, "%s/opentie-quest.cfg", s_external);
	FILE* f = fopen(s_cfg_path, "r");
	if (!f) {
		f = fopen(s_cfg_path, "w");
		if (f) {
			fputs(k_settings_template, f);
			fputs(k_settings_template_scenes, f);
			fclose(f);
		}
		return;
	}
	while (fgets(line, sizeof line, f)) {
		char* key = line;
		while (*key == ' ' || *key == '\t')
			key++;
		if (*key == '#' || *key == '\0' || *key == '\n' || *key == '\r')
			continue;
		char* eq = strchr(key, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char* end = eq + 1 + strcspn(eq + 1, "\r\n");
		*end = '\0';
		SDL_setenv_unsafe(key, eq + 1, 1);
	}
	fclose(f);
}

/* Foveation shim (packaging/quest/vkshim, shared with OpenXWA-VR). */
static char g_shim_guard[1100];
static int (*g_shim_take_log)(char* out, int size);
static void (*g_shim_frame_mark)(void);

static void quest_xr_defaults(void) {
	const char* profile = SDL_getenv("XWA_QUEST_PROFILE");
	if (profile && profile[0] == '0')
		return;
	TieXrTier_ApplyDefaults();
	if (!SDL_getenv("AERON_XR_RESOLUTION_SCALE"))
		SDL_setenv_unsafe("AERON_XR_RESOLUTION_SCALE", "1.1", 1);
	if (!SDL_getenv("AERON_XR_SCREEN_DISTANCE"))
		SDL_setenv_unsafe("AERON_XR_SCREEN_DISTANCE", "3.2", 1);
	if (!SDL_getenv("AERON_XR_SCREEN_PIXELS")) {
		/* TIE Fighter's menus are 4:3 (320x200 / 640x480 stretched). */
		SDL_setenv_unsafe("AERON_XR_SCREEN_PIXELS", "1920x1440", 1);
	}
	if (!SDL_getenv("XWA_XR_SHADOWS"))
		SDL_setenv_unsafe("XWA_XR_SHADOWS", "0", 1);
	/* Laser and explosion point lights are what saturates the headset's GPU
	 * in a dogfight (each is shaded per pixel, per eye; OpenXWA-VR turns them
	 * off too). TIE_XR_POINT_LIGHTS=1 brings them back. */
	if (!SDL_getenv("TIE_XR_POINT_LIGHTS") && !SDL_getenv("XWA_POINT_LIGHTS"))
		SDL_setenv_unsafe("TIE_XR_POINT_LIGHTS", "0", 1);
	Aeron_LogInfo("tie.quest",
				  "Quest render profile: stereo shadows %s, point lights %s, shading %s; SSAO, motion blur, FSR "
				  "off in stereo; MSAA from config.yaml (render.msaa_samples, 2)",
				  SDL_getenv("XWA_XR_SHADOWS"),
				  SDL_getenv("TIE_XR_POINT_LIGHTS") ? SDL_getenv("TIE_XR_POINT_LIGHTS") : SDL_getenv("XWA_POINT_LIGHTS"),
				  SDL_getenv("AERON_PBR_SHADER"));
	if (!SDL_getenv("AERON_PBR_SHADER")) {
		const char* shading = SDL_getenv("XWA_XR_SHADING");
		SDL_setenv_unsafe("AERON_PBR_SHADER", shading && strcmp(shading, "full") == 0 ? "full" : "lite", 1);
	}
	if (!SDL_getenv("AERON_ASTC_CACHE_DIR") && s_external[0]) {
		char dir[1100];
		snprintf(dir, sizeof dir, "%s/astc-cache", s_external);
		SDL_setenv_unsafe("AERON_ASTC_CACHE_DIR", dir, 1);
	}
	const char* fov = SDL_getenv("XWA_XR_FOVEATION");
	FILE* guard = NULL;
	if (s_external[0]) {
		snprintf(g_shim_guard, sizeof g_shim_guard, "%s/opentie-foveation-start", s_external);
		guard = fopen(g_shim_guard, "r");
	}
	if (guard) {
		/* The last start with the shim never got going: run without it. */
		fclose(guard);
		remove(g_shim_guard);
		g_shim_guard[0] = '\0';
		SDL_setenv_unsafe("AERON_TEXTURE_ASTC", "0", 1);
		SDL_setenv_unsafe("XWA_XR_FOVEATION", "0", 1);
		Aeron_LogWarn("tie.quest", "foveation: the last start with it did not get going, so it is off for this run");
		return;
	}
	/* With foveation off the shim still times the GPU (XWA_XR_GPU_TIMING=0
	 * turns that off too, and then it is not loaded at all). */
	const char* timing = SDL_getenv("XWA_XR_GPU_TIMING");
	if (fov && (fov[0] == '0' || SDL_strcasecmp(fov, "off") == 0) && timing && timing[0] == '0') {
		g_shim_guard[0] = '\0';
		SDL_setenv_unsafe("AERON_TEXTURE_ASTC", "0", 1);
		return;
	}
	void* shim = dlopen("libopenxwa_vkshim.so", RTLD_NOW | RTLD_LOCAL);
	if (shim && dlsym(shim, "vkGetInstanceProcAddr")) {
		SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, "libopenxwa_vkshim.so");
		*(void**)&g_shim_take_log = dlsym(shim, "OpenXWA_VkShim_TakeLog");
		*(void**)&g_shim_frame_mark = dlsym(shim, "OpenXWA_VkShim_FrameMark");
		if (g_shim_guard[0] && (guard = fopen(g_shim_guard, "w")) != NULL)
			fclose(guard);
		if (!SDL_getenv("AERON_TEXTURE_ASTC"))
			SDL_setenv_unsafe("AERON_TEXTURE_ASTC", "1", 1);
		Aeron_LogInfo("tie.quest", "Vulkan through the foveation shim (XWA_XR_FOVEATION=%s)",
					  fov && fov[0] ? fov : "low");
	} else {
		g_shim_guard[0] = '\0';
		SDL_setenv_unsafe("AERON_TEXTURE_ASTC", "0", 1);
		Aeron_LogWarn("tie.quest", "foveation shim not found: %s", dlerror());
	}
}

/* A folder made over adb before the app ever ran belongs to the shell user
 * and the app cannot read it: move it aside (the PC launcher then copies
 * the files again, as the app). */
static void quest_own_folder(const char* path) {
	struct stat st;
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid != getuid() && access(path, R_OK | X_OK) != 0) {
		char aside[1200];
		snprintf(aside, sizeof aside, "%s-unreadable-%ld", path, (long)time(NULL));
		if (rename(path, aside) == 0)
			Aeron_LogWarn("tie.quest", "%s belonged to uid %u, not to the app: moved to %s", path,
						  (unsigned)st.st_uid, aside);
		else
			Aeron_LogError("tie.quest", "%s belongs to uid %u and cannot be read or moved (%s)", path,
						   (unsigned)st.st_uid, strerror(errno));
	}
	mkdir(path, 0775);
}

static int quest_dir_has_files(const char* path) {
	DIR* d = opendir(path);
	if (!d)
		return 0;
	int found = 0;
	struct dirent* e;
	while (!found && (e = readdir(d)) != NULL)
		found = e->d_name[0] != '.';
	closedir(d);
	return found;
}

static void quest_game_folders(TieLaunchOptions* launch) {
	char path[1100];
	if (!s_external[0])
		return;
	snprintf(s_tie95, sizeof s_tie95, "%s/TIE95", s_external);
	snprintf(s_tie98, sizeof s_tie98, "%s/TIE98", s_external);
	quest_own_folder(s_tie95);
	quest_own_folder(s_tie98);
	snprintf(path, sizeof path, "%s/XWA", s_external);
	quest_own_folder(path);
	snprintf(path, sizeof path, "%s/XWA/FlightModels", s_external);
	mkdir(path, 0775);
	if (!SDL_getenv("TIE_XWA_COCKPITS"))
		SDL_setenv_unsafe("TIE_XWA_COCKPITS", path, 1);
	/* The Collector's CD edition needs its CD image (game.fil on Steam, about
	 * 600 MB): a TIE95 folder without it (copy cut short) would stop the
	 * game at start, so it is left out and TIE98 is used alone. */
	if (!launch->tie95_data && quest_dir_has_files(s_tie95)) {
		/* game.ins (the cue sheet) only points at the image: the image itself
		 * (game.fil on Steam, game.gog on GOG) must be there in full. */
		static const char* const images[] = { "game.fil", "GAME.FIL", "game.gog", "GAME.GOG" };
		int found = 0;
		for (size_t i = 0; i < sizeof images / sizeof images[0] && !found; ++i) {
			struct stat st;
			snprintf(path, sizeof path, "%s/%s", s_tie95, images[i]);
			found = stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 100l * 1024 * 1024;
		}
		if (found)
			launch->tie95_data = s_tie95;
		else
			Aeron_LogWarn("tie.quest", "%s has no CD image (game.fil): the 1995 edition is left out", s_tie95);
	}
	if (!launch->tie98_data && quest_dir_has_files(s_tie98))
		launch->tie98_data = s_tie98;
	Aeron_LogInfo("tie.quest", "game folders: TIE95 %s, TIE98 %s, cockpits %s",
				  launch->tie95_data ? launch->tie95_data : "(empty)",
				  launch->tie98_data ? launch->tie98_data : "(empty)", SDL_getenv("TIE_XWA_COCKPITS"));
}

/* Pilots the PC launcher copied (.tfr files in files/pilot-import) move into
 * the app's pilot folder; a pilot already on the headset is kept. */
static void quest_import_pilots(void) {
	char dir[1100];
	if (!s_external[0])
		return;
	snprintf(dir, sizeof dir, "%s/pilot-import", s_external);
	DIR* d = opendir(dir);
	if (!d)
		return;
	AeronVfs* vfs = Aeron_GetVfs();
	struct dirent* e;
	while ((e = readdir(d)) != NULL) {
		const char* name = e->d_name;
		const size_t len = strlen(name);
		if (len < 5 || len > 64 || SDL_strcasecmp(name + len - 4, ".tfr") != 0 || strchr(name, '/'))
			continue;
		char src[1200];
		snprintf(src, sizeof src, "%s/%s", dir, name);
		if (AeronVfs_Exists(vfs, AERON_VFS_ROOT_USER, name)) {
			Aeron_LogInfo("tie.quest", "pilot import: %s is already on the headset, kept that one", name);
			remove(src);
			continue;
		}
		size_t size = 0;
		void* data = SDL_LoadFile(src, &size);
		if (!data)
			continue;
		if (AeronVfs_WriteAllAtomic(vfs, AERON_VFS_ROOT_USER, name, data, size)) {
			Aeron_LogInfo("tie.quest", "pilot import: %s (%zu bytes)", name, size);
			remove(src);
		}
		SDL_free(data);
	}
	closedir(d);
}

/* The app's resources (fonts, config.yaml, model catalog) are read with
 * plain fopen by parts of the engine, which cannot see inside the APK: copy
 * them out to internal storage once per APK (assets/resources.lst, written
 * by the packaging, lists them) and point the resource root there. */
static char s_resource_root[1100];

static int quest_mkdirs(char* path) {
	for (char* p = path + 1; *p; ++p) {
		if (*p == '/') {
			*p = '\0';
			mkdir(path, 0775);
			*p = '/';
		}
	}
	return mkdir(path, 0775) == 0 || errno == EEXIST;
}

static void quest_extract_resources(TieLaunchOptions* launch) {
	const char* internal = SDL_GetAndroidInternalStoragePath();
	size_t list_size = 0;
	char* list = (char*)SDL_LoadFile("resources.lst", &list_size);
	if (!internal || !internal[0] || !list) {
		SDL_free(list);
		return;
	}
	char base[1100];
	char stamp_path[1200];
	snprintf(base, sizeof base, "%s/apk-res", internal);
	snprintf(stamp_path, sizeof stamp_path, "%s/.stamp", base);
	/* The list itself is the stamp: a new APK with other files re-extracts. */
	size_t stamp_size = 0;
	char* stamp = (char*)SDL_LoadFile(stamp_path, &stamp_size);
	const int current = stamp && stamp_size == list_size && memcmp(stamp, list, list_size) == 0;
	SDL_free(stamp);
	int ok = 1;
	if (!current) {
		int count = 0;
		char* save = NULL;
		for (char* line = strtok_r(list, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
			if (!line[0] || line[0] == '#' || strstr(line, ".."))
				continue;
			size_t size = 0;
			void* data = SDL_LoadFile(line, &size);
			char out[1400];
			snprintf(out, sizeof out, "%s/%s", base, line);
			char* slash = strrchr(out, '/');
			*slash = '\0';
			quest_mkdirs(out);
			*slash = '/';
			FILE* f = data ? fopen(out, "wb") : NULL;
			if (!f || fwrite(data, 1, size, f) != size) {
				Aeron_LogError("tie.quest", "could not extract %s", line);
				ok = 0;
			} else {
				count++;
			}
			if (f)
				fclose(f);
			SDL_free(data);
		}
		SDL_free(list);
		list = (char*)SDL_LoadFile("resources.lst", &list_size);
		if (ok && list) {
			FILE* f = fopen(stamp_path, "wb");
			if (f) {
				fwrite(list, 1, list_size, f);
				fclose(f);
			}
		}
		Aeron_LogInfo("tie.quest", "extracted %d app resource files to %s", count, base);
	}
	SDL_free(list);
	snprintf(s_resource_root, sizeof s_resource_root, "%s/resources", base);
	if (!launch->resource_root)
		launch->resource_root = s_resource_root;
}

/* The bundled SoundFont (packaging: assets/soundfonts/GeneralUser-GS.sf2, about
 * 32 MB) is copied next to the extracted resources, where FluidSynth can open
 * it (midi_resources.c: <resources>/soundfonts). Copied in pieces, again only
 * when the size differs (a new APK with another version). files/soundfonts is
 * made for the user's own .sf2, which then wins (the PC launcher copies one),
 * files/sc55 for SC-55 ROM dumps. */
#define TIE_QUEST_BUNDLED_SOUNDFONT "GeneralUser-GS.sf2"

static void quest_extract_soundfont(void) {
	char path[1300];
	if (s_external[0]) {
		snprintf(path, sizeof path, "%s/soundfonts", s_external);
		quest_own_folder(path);
		/* The user's SC-55 ROM dumps (Nuked SC-55; midi_resources.c). */
		snprintf(path, sizeof path, "%s/sc55", s_external);
		quest_own_folder(path);
	}
	if (!s_resource_root[0])
		return;
	SDL_IOStream* in = SDL_IOFromFile("soundfonts/" TIE_QUEST_BUNDLED_SOUNDFONT, "rb");
	if (!in)
		return; /* this APK has no SoundFont: OPL3 stays the default */
	const Sint64 size = SDL_GetIOSize(in);
	snprintf(path, sizeof path, "%s/soundfonts", s_resource_root);
	mkdir(path, 0775);
	snprintf(path, sizeof path, "%s/soundfonts/" TIE_QUEST_BUNDLED_SOUNDFONT, s_resource_root);
	struct stat st;
	if (size > 0 && stat(path, &st) == 0 && st.st_size == size) {
		SDL_CloseIO(in);
		return;
	}
	char temporary[1320];
	snprintf(temporary, sizeof temporary, "%s.part", path);
	FILE* out = fopen(temporary, "wb");
	int ok = out != NULL;
	static char buffer[1 << 16];
	Sint64 copied = 0;
	while (ok) {
		const size_t n = SDL_ReadIO(in, buffer, sizeof buffer);
		if (n == 0)
			break;
		ok = fwrite(buffer, 1, n, out) == n;
		copied += (Sint64)n;
	}
	SDL_CloseIO(in);
	if (out && fclose(out) != 0)
		ok = 0;
	if (ok && copied > 0 && (size <= 0 || copied == size) && rename(temporary, path) == 0) {
		Aeron_LogInfo("tie.quest", "bundled SoundFont copied to %s (%lld bytes)", path, (long long)copied);
	} else {
		Aeron_LogError("tie.quest", "could not copy the bundled SoundFont to %s", path);
		remove(temporary);
	}
}

int TieQuest_PreInit(TieLaunchOptions* launch) {
	const char* external = SDL_GetAndroidExternalStoragePath();
	if (external && external[0])
		snprintf(s_external, sizeof s_external, "%s", external);
	quest_load_settings();
	quest_extract_resources(launch);
	quest_extract_soundfont();
	quest_xr_defaults();
	quest_game_folders(launch);
	return 1;
}

int TieQuest_PostInit(TieLaunchOptions* launch) {
	/* The Vulkan device came up through the shim: the start proved itself. */
	if (g_shim_guard[0]) {
		remove(g_shim_guard);
		g_shim_guard[0] = '\0';
	}
	if (g_shim_take_log) {
		int (*seen)(void) = NULL;
		void* shim = dlopen("libopenxwa_vkshim.so", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
		if (shim)
			*(void**)&seen = dlsym(shim, "OpenXWA_VkShim_DeviceSeen");
		if (seen && !seen()) {
			SDL_setenv_unsafe("AERON_TEXTURE_ASTC", "0", 1);
			Aeron_LogWarn("tie.quest", "the OpenXR runtime created the Vulkan device without the shim: no "
									  "foveation, no ASTC textures");
		}
	}
	/* OS message boxes can't be seen in the headset. */
	Aeron_SetFatalErrorHandler(TieErrorScreen_FatalHandler);
	quest_import_pilots();
	/* No game copied yet: say how, and look again when asked. */
	while (launch && !launch->tie98_data && !launch->tie95_data) {
		char message[1600];
		snprintf(message, sizeof message,
				 "TIE Fighter's game files are not on the headset yet.\n\n"
				 "On your Windows PC, run OpenTIE-VR-Launcher.exe (in the OpenTIE-VR download) with the "
				 "headset connected by USB. It copies your TIE Fighter Special Edition (Steam or GOG) and, if "
				 "installed, X-Wing Alliance's three TIE cockpits to this folder:\n%s\n\n"
				 "Then choose Check Again.",
				 s_external);
		const int choice = TieErrorScreen_Run("OpenTIE-VR: game files needed", message, "Check Again", 0);
		if (choice != TIE_ERROR_SCREEN_RETRY)
			return 0;
		quest_game_folders(launch);
	}
	return 1;
}

void TieQuest_Frame(void) {
	static int frames;
	if (g_shim_frame_mark)
		g_shim_frame_mark();
	/* Y in the menus changed the scene: kept like the VR settings' choice. */
	const char* scene = TieFlightXrEnv_TakeSceneSwap();
	if (scene)
		TieQuest_SaveSetting("XWA_XR_ENVIRONMENT", scene);
	if (g_shim_take_log) {
		char text[4096];
		if (g_shim_take_log(text, (int)sizeof text) > 0) {
			for (char* line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
				if (line[0] == '!')
					Aeron_LogWarn("vkshim", "%s", line + 1);
				else
					Aeron_LogInfo("vkshim", "%s", line + 1);
			}
		}
	}
	if (g_shim_guard[0] && ++frames > 300) {
		remove(g_shim_guard);
		g_shim_guard[0] = '\0';
	}
}

void TieQuest_SaveSetting(const char* key, const char* value) {
	if (!s_cfg_path[0] || !key || !key[0])
		return;
	size_t size = 0;
	char* old = (char*)SDL_LoadFile(s_cfg_path, &size);
	FILE* f = fopen(s_cfg_path, "w");
	if (!f) {
		SDL_free(old);
		return;
	}
	const size_t klen = strlen(key);
	int written = 0;
	for (char* p = old; p && *p;) {
		char* nl = strchr(p, '\n');
		size_t n = nl ? (size_t)(nl - p + 1) : strlen(p);
		if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
			if (!written)
				fprintf(f, "%s=%s\n", key, value ? value : "");
			written = 1;
		} else {
			fwrite(p, 1, n, f);
		}
		p += n;
	}
	if (!written)
		fprintf(f, "%s=%s\n", key, value ? value : "");
	fclose(f);
	SDL_free(old);
}

const char* TieQuest_ExternalDir(void) {
	return s_external;
}

#else /* desktop */

#include "tie_app/quest/error_screen.h"

int TieQuest_PreInit(TieLaunchOptions* launch) {
	(void)launch;
	const char* xr = SDL_getenv("AERON_XR");
	return xr && xr[0] == '1';
}

int TieQuest_PostInit(TieLaunchOptions* launch) {
	(void)launch;
	/* Desktop test of the headset's message screen (no game files case). */
	const char* test = SDL_getenv("TIE_TEST_ERROR_SCREEN");
	if (test && test[0] == '1')
		return TieErrorScreen_Run("OpenTIE-VR: game files needed", "Test of the in-headset message screen.",
								  "Check Again", 0) == TIE_ERROR_SCREEN_RETRY;
	return 1;
}

void TieQuest_Frame(void) {}

void TieQuest_SaveSetting(const char* key, const char* value) {
	(void)key;
	(void)value;
}

const char* TieQuest_ExternalDir(void) {
	return "";
}

#endif

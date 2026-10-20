#include "tie_app/midi_resources.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "aeron/paths.h"

#include <imuse/midi_fluidsynth.h>
#include <imuse/midi_nuked_sc55.h>

/* First valid *.sf2 (by name, case-insensitive extension) directly in directory. */
static bool TieMidiResources_FirstSoundfontIn(const char* directory, char* out, size_t capacity) {
	if (!directory || !directory[0])
		return false;
	int count = 0;
	char** names = SDL_GlobDirectory(directory, "*.sf2", SDL_GLOB_CASEINSENSITIVE, &count);
	if (!names)
		return false;
	const char* best = NULL;
	for (int index = 0; index < count; ++index) {
		if (strchr(names[index], '/') || strchr(names[index], '\\'))
			continue;
		if (!best || SDL_strcasecmp(names[index], best) < 0) {
			char candidate[2048];
			const int length = snprintf(candidate, sizeof candidate, "%s/%s", directory, names[index]);
			if (length > 0 && (size_t)length < sizeof candidate && imuse_fluidsynth_soundfont_is_valid(candidate))
				best = names[index];
		}
	}
	bool found = false;
	if (best) {
		const int length = snprintf(out, capacity, "%s/%s", directory, best);
		found = length > 0 && (size_t)length < capacity;
	}
	SDL_free(names);
	return found;
}

static bool TieMidiResources_FindDefaultSoundfont(char* out, size_t capacity) {
	char directory[1024];
#if defined(SDL_PLATFORM_ANDROID)
	const char* external = SDL_GetAndroidExternalStoragePath();
	if (external && external[0]) {
		snprintf(directory, sizeof directory, "%s/soundfonts", external);
		if (TieMidiResources_FirstSoundfontIn(directory, out, capacity))
			return true;
	}
#endif
	const char* resources = Aeron_ResourceRoot();
	if (resources && resources[0]) {
		snprintf(directory, sizeof directory, "%s/soundfonts", resources);
		if (TieMidiResources_FirstSoundfontIn(directory, out, capacity))
			return true;
	}
	return false;
}

bool TieMidiResources_DefaultSoundfont(char* out, size_t capacity) {
	/* The settings page asks every frame: look at the folders at most every 2 s. */
	static char cached[1024];
	static bool cached_found;
	static Uint64 cached_at;
	if (!out || !capacity || !imuse_fluidsynth_backend_available())
		return false;
	const Uint64 now = SDL_GetTicks();
	if (!cached_at || now - cached_at > 2000) {
		cached_found = TieMidiResources_FindDefaultSoundfont(cached, sizeof cached);
		cached_at = now ? now : 1;
	}
	if (!cached_found || strlen(cached) >= capacity)
		return false;
	memcpy(out, cached, strlen(cached) + 1);
	return true;
}

const char* TieMidiResources_EffectiveSoundfont(const char* configured, char* buffer, size_t capacity) {
	if (configured && configured[0])
		return configured;
	return TieMidiResources_DefaultSoundfont(buffer, capacity) ? buffer : NULL;
}

bool TieMidiResources_Soundfontvalidate(const char* path, char* error, size_t error_capacity) {
	char default_path[1024];
	path = TieMidiResources_EffectiveSoundfont(path, default_path, sizeof default_path);
	if (!path || !path[0]) {
		if (error && error_capacity)
			snprintf(error, error_capacity, "Select a SoundFont file.");
		return false;
	}
	FILE* file = fopen(path, "rb");
	if (!file) {
		if (error && error_capacity)
			snprintf(error, error_capacity, "The SoundFont file is missing or unreadable:\n%s", path);
		return false;
	}
	fclose(file);
	if (!imuse_fluidsynth_soundfont_is_valid(path)) {
		if (error && error_capacity)
			snprintf(error, error_capacity, "The selected file is not a valid SoundFont:\n%s", path);
		return false;
	}
	if (error && error_capacity)
		error[0] = '\0';
	return true;
}

bool TieMidiResources_Sc55RomDirectoryvalidate(const char* path, char* error, size_t error_capacity) {
	ImuseNukedSc55Romset* romset = imuse_nuked_sc55_romset_load(path, error, error_capacity);
	if (!romset)
		return false;
	imuse_nuked_sc55_romset_release(romset);
	return true;
}

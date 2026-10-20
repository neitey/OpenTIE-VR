#ifndef TIE_APP_MIDI_RESOURCES_H
#define TIE_APP_MIDI_RESOURCES_H

#include <stdbool.h>
#include <stddef.h>

/* An empty path means the default SoundFont (TieMidiResources_DefaultSoundfont). */
bool TieMidiResources_Soundfontvalidate(const char* path, char* error, size_t error_capacity);
bool TieMidiResources_Sc55RomDirectoryvalidate(const char* path, char* error, size_t error_capacity);

/* The SoundFont used when audio.fluidsynth.soundfont_file is empty: the first
 * .sf2 (by name) in the user's soundfonts folder (Android: files/soundfonts,
 * where the PC launcher copies one), else the first in <resources>/soundfonts
 * (the Quest APK bundles GeneralUser GS there). False when there is none. */
bool TieMidiResources_DefaultSoundfont(char* out, size_t capacity);

/* configured when set, else the default SoundFont; NULL when neither exists. */
const char* TieMidiResources_EffectiveSoundfont(const char* configured, char* buffer, size_t capacity);

#endif /* TIE_APP_MIDI_RESOURCES_H */

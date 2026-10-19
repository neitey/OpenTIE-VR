#ifndef TIE_RUNTIME_RUNTIME_H
#define TIE_RUNTIME_RUNTIME_H

#include "tie_runtime/audio/imuse_session.h"
#include "tie_runtime/flight_assets/service.h"
#include "tie_runtime/runtime/profile_types.h"
#include "tie_runtime/storage/storage.h"

typedef struct TieRuntimeConfig {
	TieStorageConfig storage;
	TieFlightAssetsConfig flight_assets;
	TieFrontendProfileId frontend_profile;
	TieFlightProfile flight_profile;
	TieAudioConfig audio;
} TieRuntimeConfig;

bool TieRuntime_Init(const TieRuntimeConfig* config, char* error, size_t error_capacity);
void TieRuntime_Shutdown(void);
bool TieRuntime_IsActive(void);

/* Requests cooperative shutdown of both frontend and flight tasks. */
void TieRuntime_RequestExit(void);
bool TieRuntime_ShouldExit(void);
void TieRuntime_SetWindowActive(bool active);

/* One-shot request from recovered UI tasks to the application overlay. */
void TieRuntime_RequestSettingsMenu(void);
bool TieRuntime_ConsumeSettingsMenuRequest(void);
bool TieRuntime_SetMusicDuckingVolumePercent(int percent);

/* Advances one host-visible tick using the supplied synthetic-clock delta. */
void TieRuntime_Tick(int32_t delta_us);
/* Slow motion: the flight sim's clock advances `scale` (0.05..1) of each
 * tick's time; 1 = normal. Music and the frontend clocks are not scaled.
 * The VR quick wheel uses it (XWA_XR_WHEEL_SLOWMO). */
void TieRuntime_SetSimTimeScale(float scale);

#ifdef TIE_MODERN
/* UINT64_MAX means no task deadline precedes the presentation cadence. */
uint64_t TieRuntime_NextWakeDelayUs(void);
#endif
void TieRuntime_RequestFlightResourceRelease(void);
bool TieRuntime_FlightResourceReleaseRequested(void);
void TieRuntime_CompleteFlightResourceRelease(void);

#endif

#ifndef TIE_REMASTER_XR_PROF_H
#define TIE_REMASTER_XR_PROF_H

#include <stdint.h>

/* OpenTIE-VR: where a host frame's CPU time goes, logged once per 5 s while
 * the headset presents ("tie.perf: CPU per frame ..."), so a headset log
 * shows it next to the GPU timing of the Vulkan shim. TIE_XR_PROFILE=0 turns
 * it off; =1 also logs while not presenting (desktop). */
typedef enum TieXrProfSection {
	TIE_XR_PROF_WAIT = 0, /* Aeron_BeginFrame + Aeron_WaitForNextFrame (xrWaitFrame, pacing) */
	TIE_XR_PROF_INPUT,    /* input, VR controls, console actions, ... up to the sim tick */
	TIE_XR_PROF_SIM,      /* TieRuntime_Tick (the game) */
	TIE_XR_PROF_PREPARE,  /* flight PrepareFrame (scene submission, PIP) */
	TIE_XR_PROF_COCKPIT,  /* cockpit Prepare (instrument atlas, HUD lists) */
	TIE_XR_PROF_EYES,     /* recording both eyes (scene render + tonemap) */
	TIE_XR_PROF_HUD,      /* HUD targets + controller labels */
	TIE_XR_PROF_PANELS,   /* lap console, side panel, quick wheel */
	TIE_XR_PROF_SUBMIT,   /* flight command buffer submission */
	TIE_XR_PROF_ENV,      /* the menu scene (both eyes) */
	TIE_XR_PROF_LAYERS,   /* the rest of the game layers (2D scenes, cursor, ...) */
	TIE_XR_PROF_UI,       /* settings / keyboard / overlays */
	TIE_XR_PROF_PRESENT,  /* Aeron_Present (xrEndFrame, swapchain) */
	TIE_XR_PROF_COUNT
} TieXrProfSection;

typedef enum TieXrProfCounter {
	TIE_XR_PROF_N_STEREO = 0, /* flight frames rendered in stereo */
	TIE_XR_PROF_N_ATLAS,      /* instrument atlas redraws */
	TIE_XR_PROF_N_PIP,        /* CMD PIP scene renders */
	TIE_XR_PROF_N_ENV,        /* menu scene frames */
	TIE_XR_PROF_N_COUNT
} TieXrProfCounter;

/* Nanoseconds (SDL_GetTicksNS); 0 when profiling is off. */
uint64_t TieXrProf_Now(void);
/* Adds now - t0 to the section (no-op for t0 == 0). Returns now. */
uint64_t TieXrProf_Add(TieXrProfSection section, uint64_t t0);
/* For a section that contains others: TieXrProf_Mark() before, then
 * AddOuter adds only the time the inner sections did not take. */
uint64_t TieXrProf_Mark(void);
uint64_t TieXrProf_AddOuter(TieXrProfSection section, uint64_t t0, uint64_t mark);
void TieXrProf_Count(TieXrProfCounter counter);
/* Once per host frame, after Aeron_Present. */
void TieXrProf_FrameEnd(void);

#endif

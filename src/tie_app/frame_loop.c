#include "tie_app/frame_loop.h"

#include "tie_runtime/input/controller_mapping.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aeron/aeron.h"
#include "aeron/compat/host.h"
#include "tie_app/hotkeys.h"
#include "tie_app/quest/quest_app.h"
#include "tie_app/test/test_harness.h"
#include <SDL3/SDL_stdinc.h>
#include "tie_app/settings/flight_options.h"
#include "tie_app/settings/settings.h"
#include "tie_remaster/remaster.h"
#include "tie_remaster/vr/vr_cockpit_hud.h"
#include "tie_remaster/vr/vr_hands.h"
#include "tie_runtime/display/classic_display.h"
#include "tie_runtime/display/classic_framebuffer.h"
#include "tie_runtime/flight_assets/service.h"
#include "tie_runtime/input/input.h"
#include "tie_runtime/input/vr_controls.h"
#include "tie_runtime/presentation/classic_layer.h"
#include "tie_runtime/presentation/flight_marker.h"
#include "tie_runtime/presentation/presentation.h"
#include "tie_runtime/runtime/profile.h"
#include "tie_runtime/runtime/runtime.h"
#include "tie_runtime/snapshot/snapshot.h"
#include "tie_app/quest/voice_commands.h"
#include "tie_app/quest/vr_voice.h"
#include "tie_remaster/flight/vr_console.h"
#include "tie_remaster/xr_prof.h"
#include "tie_runtime/input/vr_console_game.h"
#include "tie_app/vr_keyboard.h"

static uint64_t TieFrameLoop_PresentationIntervalUs(void) {
	const double rate = Aeron_PresentationRate();
	return (uint64_t)(1000000.0 / rate + 0.5);
}

static const TieFramebuffer* TieFrameLoop_Tie98AspectCorrectedFramebuffer(bool enabled,
																		  TieClassicOutputKind output_kind,
																		  const TieSnapshot* snapshot) {
	if (!enabled || output_kind != TIE_CLASSIC_OUTPUT_DX5_SURFACE || !snapshot ||
		snapshot->frontend_profile_id != TIE_FRONTEND_PROFILE_TIE98)
		return NULL;
	return TieClassicFramebuffer_PresentedVga();
}

static bool TieFrameLoop_UpdateFocus(const AeronInputSnapshot* input, bool* ever_had_focus) {
	if (input && input->has_focus)
		*ever_had_focus = true;
	bool focus_paused = input && !input->has_focus && *ever_had_focus;
	/* Headset taken off or its system menu open (XWA_XR_AUTO_PAUSE=0 keeps
	 * the game running). */
	if (Aeron_XrIsUnfocused()) {
		const char* auto_pause = SDL_getenv("XWA_XR_AUTO_PAUSE");
		if (!(auto_pause && auto_pause[0] == '0'))
			focus_paused = true;
	}
	TieRuntime_SetWindowActive(!focus_paused);
	return focus_paused;
}

static void TieFrameLoop_UpdatePresentation(const AeronInputSnapshot* input, int32_t delta_us) {
	const TiePresentationChange change = TiePresentation_BeginFrame(input, delta_us);
	if (!(change & TIE_PRESENTATION_CHANGE_LAYOUT))
		return;
	const TiePresentationLayout* presentation = TiePresentation_Layout();
	if (presentation)
		TieInput_SetClassicLayout(&presentation->frame, &presentation->classic);
}

static bool TieFrameLoop_SynchronizeFlightProfile(uint32_t* generation) {
	if (TieRuntime_FlightResourceReleaseRequested()) {
		TieRemaster_ReleaseFlightResources();
		TieRuntime_CompleteFlightResourceRelease();
	}
	const uint32_t current = TieFlightAssets_ProfileGeneration();
	if (current == *generation)
		return true;
	const TieFlightAssetSource* source = TieFlightAssets_CurrentSource();
	if (!source || !TiePresentation_SetModernAspect(source->presentation_aspect)) {
		Aeron_RequestFatalRendererError("flight profile presentation update");
		return false;
	}
	if (!TieRemaster_SetFlightSource(source)) {
		Aeron_RequestFatalRendererError("flight profile renderer rebind");
		return false;
	}
	*generation = current;
	return true;
}

static void TieFrameLoop_SubmitGameLayers(TieClassicOutputKind* previous_output_kind,
										  const TieSnapshot* snapshot,
										  const TieAppLiveFlightOptions* flight_options, int32_t delta_us,
										  bool paused) {
	const bool suppress_classic = TieRemaster_SuppressesClassicFlight(snapshot);
	TieClassicLayer_SetSuppressed(suppress_classic);

	const TieClassicOutputKind output_kind = TieClassicDisplay_OutputKind();
	const TieFramebuffer* corrected_vga = TieFrameLoop_Tie98AspectCorrectedFramebuffer(
		flight_options->aspect_correct_legacy_scenes, output_kind, snapshot);
	if (*previous_output_kind == TIE_CLASSIC_OUTPUT_DX5_SURFACE &&
		output_kind == TIE_CLASSIC_OUTPUT_INDEXED_FRAMEBUFFER)
		AeronDx5_ResetPresentationState();
	*previous_output_kind = output_kind;

	if (output_kind == TIE_CLASSIC_OUTPUT_DX5_SURFACE)
		AeronDx5_EndFrame();
	if (!suppress_classic && output_kind == TIE_CLASSIC_OUTPUT_INDEXED_FRAMEBUFFER)
		TieClassicLayer_Submit(TieClassicFramebuffer_Current());
	else if (!suppress_classic && corrected_vga)
		TieClassicLayer_Submit(corrected_vga);
	TieRemaster_Frame(snapshot, delta_us, paused);
	TieFlightMarker_Submit(snapshot);
	/* The VR flight stick was moved by hand: keep its spot. */
	char stick_pos[64];
	if (TieVrHands_TakeStickPlacement(stick_pos, (int)sizeof stick_pos))
		TieQuest_SaveSetting("XWA_XR_STICK_POS", stick_pos);
	/* A VR HUD panel was moved: keep the cockpit's layout. */
	char hud_key[96], hud_layout[1024];
	if (TieVrCockpitHud_TakeLayout(hud_key, (int)sizeof hud_key, hud_layout, (int)sizeof hud_layout))
		TieQuest_SaveSetting(hud_key, hud_layout);
}

static void TieFrameLoop_DiscoverControllers(const AeronInputSnapshot* input) {
	static char previous_error[512];
	TieAppConfigState* config = TieAppConfig_Current();
	if (!config || TieSettings_Open())
		return;
	TieControllerOptions candidate = config->requested.controller;
	char error[512] = { 0 };
	bool ok = TieControllerMapping_InitializeGamepads(&candidate, &config->gamepad_defaults, input, error,
													  sizeof error);
	if (!TieControllerMapping_Optionsequal(&candidate, &config->requested.controller)) {
		char save_error[512];
		if (TieAppConfig_SetController(config, &candidate, save_error, sizeof save_error))
			TieControllerMapping_SetOptions(&config->requested.controller);
		else {
			snprintf(error, sizeof error, "%s", save_error);
			ok = false;
		}
	}
	if (!ok && strcmp(error, previous_error))
		Aeron_LogWarn("tie.input", "%s", error);
	snprintf(previous_error, sizeof previous_error, "%s", ok ? "" : error);
}

void TieFrameLoop_Run(void) {
	const TiePresentationLayout* presentation = TiePresentation_Layout();
	if (presentation)
		TieInput_SetClassicLayout(&presentation->frame, &presentation->classic);

	TieClassicLayer_Init();
	TieHotkeys hotkeys;
	TieHotkeys_Init(&hotkeys);
	TieClassicOutputKind previous_output_kind = TieClassicDisplay_OutputKind();
	uint32_t profile_generation = TieFlightAssets_ProfileGeneration();
	bool audio_paused = false;
	bool ever_had_focus = false;
	TieVrConsole_SetVoiceList(TieVoiceCommands_Count, TieVoiceCommands_Label, TieVoiceCommands_Phrase);

	while (TieRuntime_IsActive() && !TieRuntime_ShouldExit() && !Aeron_FatalErrorRequested()) {
		/* CPU frame breakdown for the VR log (tie_remaster/xr_prof.h). */
		uint64_t prof_t = TieXrProf_Now();
		const int32_t delta_us = Aeron_BeginFrame();
		prof_t = TieXrProf_Add(TIE_XR_PROF_WAIT, prof_t);
		if (Aeron_FatalErrorRequested())
			break;
		if (Aeron_QuitRequested())
			TieRuntime_RequestExit();

		TieQuest_Frame();
		const AeronInputSnapshot* input = Aeron_InputSnapshot();
		if (TieTest_Active())
			TieTest_ApplyInput((AeronInputSnapshot*)input);
		TieFrameLoop_DiscoverControllers(input);
		const bool focus_paused = TieFrameLoop_UpdateFocus(input, &ever_had_focus);
		TieFrameLoop_UpdatePresentation(input, delta_us);
		const TieHotkeysFrame hotkey_frame = TieHotkeys_Process(&hotkeys, input);
		bool menu_open = hotkey_frame.menu_open;
		bool settings_opened_this_frame = hotkey_frame.settings_opened;
		/* VR controllers: pointer / flight-button modes, B = back. */
		if ((TieVrControls_Frame(TieSettings_Open(), hotkey_frame.paused) & TIE_VR_REQUEST_CLOSE_SETTINGS) &&
			TieSettings_Open() && !settings_opened_this_frame) {
			TieSettings_Toggle();
			menu_open = TieSettings_Open();
		}
		TieInput_UpdateCapture(TieSnapshot_Current(), TieSettings_Open());
		TieClassicLayer_SyncInputExtent();

		TieAppLiveFlightOptions flight_options;
		TieFlightOptions_Get(&flight_options);

		TieRemaster_BeginFrame(input);
		if (Aeron_FatalErrorRequested())
			break;
		TieInput_UpdateKeyboard(input, menu_open || hotkey_frame.paused || focus_paused);
		/* VR keyboard for text fields: types into the key queue, owns its clicks. */
		TieVrKeyboard_BeginFrame(input, delta_us, menu_open || hotkey_frame.paused || focus_paused);
		TieVrVoice_Update(menu_open || hotkey_frame.paused || focus_paused);
		/* VR lap console / quick wheel / side panel: last frame's presses
		 * become game actions (ignored outside cockpit flight), and the
		 * ship state they show. */
		TieVrConsole_BeginFrame((float)delta_us * 1e-6f);
		{
			TieVrConsoleAction console_action;
			int console_arg;
			while (TieVrConsole_PollAction(&console_action, &console_arg))
				TieVrConsoleGame_Action(console_action, console_arg);
			/* Slow motion (45 % speed) while the quick wheel is open in
			 * flight, eased in and out (XWA_XR_WHEEL_SLOWMO=0 turns it off). */
			static float slow;
			const char* slow_env = SDL_getenv("XWA_XR_WHEEL_SLOWMO");
			const bool slow_on = !(slow_env && slow_env[0] == '0');
			const float slow_dt = (float)delta_us * 1e-6f;
			const bool slow_want = slow_on && TieVrConsole_WheelOpen() && TieVrControls_InFlight();
			static bool slow_logged;
			if (slow_want != slow_logged) {
				Aeron_LogInfo("tie.vrconsole", "quick wheel slow motion %s", slow_want ? "on" : "off");
				slow_logged = slow_want;
			}
			slow = slow_want ? SDL_min(1.0f, slow + slow_dt / 0.15f) : SDL_max(0.0f, slow - slow_dt / 0.25f);
			const float slow_k = slow * slow * (3.0f - 2.0f * slow);
			TieRuntime_SetSimTimeScale(1.0f - 0.55f * slow_k);
			TieVrConsoleState console_state;
			TieVrConsoleGame_GetState(&console_state);
			TieVrConsole_SetState(&console_state);
		}
		if (!menu_open)
			TieInput_BeginFrame(delta_us);
		prof_t = TieXrProf_Add(TIE_XR_PROF_INPUT, prof_t);
		if (!hotkey_frame.paused && !menu_open && !focus_paused)
			TieRuntime_Tick(delta_us);
		prof_t = TieXrProf_Add(TIE_XR_PROF_SIM, prof_t);
		if (TieRuntime_ConsumeSettingsMenuRequest()) {
			const bool was_open = TieSettings_Open();
			TieSettings_Show();
			settings_opened_this_frame |= !was_open && TieSettings_Open();
			menu_open = menu_open || TieSettings_Open();
		}
		if (Aeron_FatalErrorRequested() || !TieFrameLoop_SynchronizeFlightProfile(&profile_generation))
			break;

		const bool pause_requested = hotkey_frame.paused || menu_open || focus_paused;
		if (pause_requested != audio_paused) {
			Aeron_AudioSetPaused(pause_requested ? 1 : 0);
			audio_paused = pause_requested;
			Aeron_LogInfo("tie", "%s", audio_paused ? "paused" : "resumed");
		}

		TieClassicLayer_SyncInputExtent();
		const TieSnapshot* snapshot = TieSnapshot_Current();
		prof_t = TieXrProf_Add(TIE_XR_PROF_INPUT, prof_t);
		const uint64_t prof_mark = TieXrProf_Mark();
		TieFrameLoop_SubmitGameLayers(&previous_output_kind, snapshot, &flight_options, delta_us,
									  hotkey_frame.paused || menu_open);
		prof_t = TieXrProf_AddOuter(TIE_XR_PROF_LAYERS, prof_t, prof_mark);
		{
			/* The player put the lap console somewhere new / the side panel
			 * shows another tab: keep it. */
			char console_value[160];
			if (TieVrConsole_TakePlacement(console_value, (int)sizeof console_value))
				TieQuest_SaveSetting("XWA_XR_CONSOLE_POSE", console_value);
			if (TieVrConsole_TakePanelTab(console_value, (int)sizeof console_value))
				TieQuest_SaveSetting("XWA_XR_PANEL_TAB", console_value);
		}
		TieVrKeyboard_Submit();
		/* Do not let the input edge that opened the menu close it again. */
		if (!settings_opened_this_frame)
			TieSettings_Frame(input, (float)delta_us * 1e-6f);
		TieInput_SyncSystemCursor(TieSettings_Open());

		if (Aeron_FatalErrorRequested())
			break;
		if (TieTest_Active())
			TieTest_BeforePresent();
		prof_t = TieXrProf_Add(TIE_XR_PROF_UI, prof_t);
		if (!Aeron_Present()) {
			Aeron_RequestFatalRendererError("frame presentation");
			break;
		}
		prof_t = TieXrProf_Add(TIE_XR_PROF_PRESENT, prof_t);
		if (TieTest_Active() && TieTest_AfterPresent())
			TieRuntime_RequestExit();

		uint64_t wake_delay_us = TieFrameLoop_PresentationIntervalUs();
		if (!pause_requested) {
			const uint64_t engine_delay_us = TieRuntime_NextWakeDelayUs();
			if (engine_delay_us < wake_delay_us)
				wake_delay_us = engine_delay_us;
		}
		prof_t = TieXrProf_Now();
		Aeron_WaitForNextFrame(wake_delay_us);
		(void)TieXrProf_Add(TIE_XR_PROF_WAIT, prof_t);
		TieXrProf_FrameEnd();
	}
	TieVrVoice_Shutdown();
	TieClassicLayer_Shutdown();
}

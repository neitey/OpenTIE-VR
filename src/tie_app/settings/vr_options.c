/* VR settings page (headset builds, or the desktop with AERON_XR=1).
 *
 * Every row is an environment variable that the VR code reads (most of them
 * live, every frame); a change sets it at once and is saved to
 * opentie-quest.cfg on the headset. The setting names are the ones
 * OpenXWA-VR uses where the two share the VR code. */

#include "tie_app/settings/vr_options.h"

#include <stdio.h>
#include <string.h>

#include <SDL3/SDL_stdinc.h>

#include "aeron/aeron.h"
#include "aeron/scene/ui.h"
#include "tie_app/quest/quest_app.h"

typedef struct TieVrOptionRow {
	const char* key;
	const char* label;
	const char* const* labels;
	const char* const* values;
	int count;
	int default_index;
	bool restart;
	const char* help;
} TieVrOptionRow;

static const char* const k_off_on[] = { "Off", "On" };
static const char* const k_0_1[] = { "0", "1" };

static const char* const k_cockpit_labels[] = { "X-Wing Alliance 3D", "TIE Fighter 2D" };
static const char* const k_cockpit_values[] = { "xwa", "2d" };
static const char* const k_hud_scale_labels[] = { "Small", "Medium", "Large", "Extra Large" };
static const char* const k_hud_scale_values[] = { "0.45", "0.55", "0.65", "0.8" };
static const char* const k_hud_dist_labels[] = { "2 m", "3 m", "4 m" };
static const char* const k_hud_dist_values[] = { "2.0", "3.0", "4.0" };
static const char* const k_world_scale_labels[] = { "75 %", "85 %", "100 %" };
static const char* const k_world_scale_values[] = { "0.75", "0.85", "1.0" };
static const char* const k_depth_labels[] = { "Real", "2x", "4x" };
static const char* const k_depth_values[] = { "1.0", "2.0", "4.0" };
static const char* const k_res_labels[] = { "90 %", "100 %", "110 %", "125 %" };
static const char* const k_res_values[] = { "0.9", "1.0", "1.1", "1.25" };
static const char* const k_refresh_labels[] = { "Headset Default", "72 Hz", "90 Hz", "120 Hz" };
static const char* const k_refresh_values[] = { "0", "72", "90", "120" };
static const char* const k_fov_labels[] = { "Off", "Low", "Medium", "High" };
static const char* const k_fov_values[] = { "0", "low", "medium", "high" };
static const char* const k_voice_labels[] = { "Off", "Double-Tap Thumb Rest", "Hold Thumb Rest" };
static const char* const k_voice_values[] = { "0", "doubletap", "hold" };
static const char* const k_env_labels[] = { "Imperial Fleet", "Stars Only", "Off" };
static const char* const k_env_values[] = { "1", "0", "off" };
static const char* const k_env_bright_labels[] = { "Dim", "Medium", "Bright" };
static const char* const k_env_bright_values[] = { "0.5", "0.8", "1.0" };
static const char* const k_screen_labels[] = { "2.5 m", "3.2 m", "4 m" };
static const char* const k_screen_values[] = { "2.5", "3.2", "4.0" };
static const char* const k_hud_layout_labels[] = { "Cockpit Panels", "Helmet" };
static const char* const k_hud_layout_values[] = { "cockpit", "helmet" };
static const char* const k_hands_labels[] = { "Controllers", "Pilot Gloves", "Off" };
static const char* const k_hands_values[] = { "controllers", "gloves", "off" };

static const char* const k_curve_labels[] = { "Direct", "Medium", "Precise", "Very Precise" };
static const char* const k_curve_values[] = { "1.0", "1.5", "2.0", "2.6" };

static const TieVrOptionRow k_rows[] = {
	{ "TIE_XR_COCKPIT", "Cockpit", k_cockpit_labels, k_cockpit_values, 2, 0, false,
	  "X-Wing Alliance's 3D TIE cockpits need their cockpit files copied over by the PC launcher." },
	{ "XWA_XR_STEREO", "3D Flight", k_off_on, k_0_1, 2, 1, false,
	  "Off shows the flight view on the flat screen as well." },
	{ "XWA_XR_HUD_LAYOUT", "HUD", k_hud_layout_labels, k_hud_layout_values, 2, 0, false,
	  "Cockpit Panels: the sensors, targeting computer and gauges float around the canopy; put a hand on one "
	  "and squeeze the grip to move it. Helmet: the instruments follow your view (2D cockpit) or sit on the "
	  "dashboard (3D cockpit)." },
	{ "XWA_XR_HUD_SCALE", "Helmet HUD Size", k_hud_scale_labels, k_hud_scale_values, 4, 1, false, NULL },
	{ "XWA_XR_HUD_DISTANCE", "Helmet HUD Distance", k_hud_dist_labels, k_hud_dist_values, 3, 1, false, NULL },
	{ "XWA_XR_WORLD_SCALE", "World Scale", k_world_scale_labels, k_world_scale_values, 3, 2, false,
	  "Lower makes the cockpit and ships feel bigger." },
	{ "XWA_XR_DEPTH_BOOST", "Depth Boost", k_depth_labels, k_depth_values, 3, 0, false,
	  "More stereo depth for ships at combat range." },
	{ "XWA_XR_HANDS", "Hands", k_hands_labels, k_hands_values, 3, 0, false,
	  "What your hands are in flight: the controllers, or flight gloves." },
	{ "XWA_XR_CONTROLLER_LABELS", "Controller Labels", k_off_on, k_0_1, 2, 1, false,
	  "Look at a controller, buttons towards you: labels show what each button does." },
	{ "XWA_XR_FLIGHT_STICK", "Flight Stick", k_off_on, k_0_1, 2, 0, false,
	  "A stick right of the seat: grab it with the right grip and fly with it (the thumbstick still works). "
	  "The left grip moves it." },
	{ "XWA_XR_AUTO_PAUSE", "Pause When Headset Is Off", k_off_on, k_0_1, 2, 1, false, NULL },
	{ "TIE_XR_STICK_CURVE", "Stick Response", k_curve_labels, k_curve_values, 4, 2, false,
	  "Precise: small thumbstick movements turn gently for fine aim; full deflection still turns at full rate." },
	{ "AERON_XR_INVERT_PITCH", "Invert Pitch", k_off_on, k_0_1, 2, 0, false, NULL },
	{ "AERON_XR_LEFT_HANDED", "Left-Handed Controls", k_off_on, k_0_1, 2, 0, false, NULL },
	{ "XWA_XR_VOICE", "Voice Commands", k_voice_labels, k_voice_values, 3, 1, false,
	  "In flight: double-tap a thumb rest (or hold it), then speak, e.g. \"attack my target\", \"next target\", "
	  "\"full throttle\", \"lasers to shields\", \"cover me\"." },
	{ "XWA_XR_ENVIRONMENT", "Menu Scene", k_env_labels, k_env_values, 3, 0, false,
	  "What surrounds the menu screen: a Star Destroyer and TIE fighters in space, or just stars." },
	{ "XWA_XR_ENV_BRIGHTNESS", "Menu Scene Brightness", k_env_bright_labels, k_env_bright_values, 3, 1, false,
	  NULL },
	{ "XWA_XR_CONSOLE", "Lap Console", k_off_on, k_0_1, 2, 1, false,
	  "A touch panel over your lap (look down): energy, targeting, wingmen, ship. Touch a button or point at it "
	  "and pull the trigger; squeeze a grip at its end to move it." },
	{ "XWA_XR_WHEEL", "Radial Menu", k_off_on, k_0_1, 2, 1, false,
	  "Hold the left grip still for a moment (no button with it), point at a command, pull the left trigger." },
	{ "XWA_XR_SIDE_PANEL", "Side Panel", k_off_on, k_0_1, 2, 1, false,
	  "On your right: tactical map, voice commands, button mapping." },
	{ "XWA_XR_DIRECT_OUTPUT", "Direct Output (faster)", k_off_on, k_0_1, 2, 0, false,
	  "On: the 3D view goes straight into the headset images, about 3 ms less GPU work per frame in "
	  "flight; there is no bloom glow on lasers and engines." },
	{ "AERON_XR_RESOLUTION_SCALE", "Resolution", k_res_labels, k_res_values, 4, 2, true, NULL },
	{ "AERON_XR_REFRESH_RATE", "Refresh Rate", k_refresh_labels, k_refresh_values, 4, 0, true, NULL },
	{ "XWA_XR_FOVEATION", "Foveation", k_fov_labels, k_fov_values, 4, 1, true,
	  "Renders the edges of the view coarser to save GPU time." },
	{ "AERON_XR_SCREEN_DISTANCE", "Menu Screen Distance", k_screen_labels, k_screen_values, 3, 1, true, NULL },
};

enum { TIE_VR_ROW_COUNT = (int)(sizeof k_rows / sizeof k_rows[0]) };

static bool s_restart_needed;

/* The VR cockpit HUD's per-cockpit panel layouts (vr_cockpit_hud.h); empty =
 * that cockpit's built-in layout. */
static const char* const k_hud_pose_keys[] = {
	"XWA_XR_HUD_POSE_TIECOCKPIT",
	"XWA_XR_HUD_POSE_TIEBOMBERCOCKPIT",
	"XWA_XR_HUD_POSE_COMBATSIMCOCKPIT",
	"XWA_XR_HUD_POSE_2D",
};

static bool TieVrOptions_HudPosesSet(void) {
	for (size_t i = 0; i < sizeof k_hud_pose_keys / sizeof k_hud_pose_keys[0]; ++i) {
		const char* v = SDL_getenv(k_hud_pose_keys[i]);
		if (v && v[0])
			return true;
	}
	return false;
}

static void TieVrOptions_ResetHudPoses(void) {
	for (size_t i = 0; i < sizeof k_hud_pose_keys / sizeof k_hud_pose_keys[0]; ++i) {
		SDL_setenv_unsafe(k_hud_pose_keys[i], "", 1);
		TieQuest_SaveSetting(k_hud_pose_keys[i], "");
	}
}

bool TieVrOptions_Available(void) {
#if defined(SDL_PLATFORM_ANDROID)
	return true;
#else
	return Aeron_XrIsPresenting() != 0;
#endif
}

static int TieVrOptions_Current(const TieVrOptionRow* row) {
	const char* value = SDL_getenv(row->key);
	if (!value || !value[0])
		return row->default_index;
	for (int i = 0; i < row->count; ++i)
		if (SDL_strcasecmp(value, row->values[i]) == 0)
			return i;
	/* A numeric value between the choices: the nearest one. */
	char* end = NULL;
	const double number = SDL_strtod(value, &end);
	if (end && end != value) {
		int best = row->default_index;
		double best_diff = 1e30;
		for (int i = 0; i < row->count; ++i) {
			char* vend = NULL;
			const double v = SDL_strtod(row->values[i], &vend);
			if (vend == row->values[i])
				continue;
			const double diff = v > number ? v - number : number - v;
			if (diff < best_diff) {
				best_diff = diff;
				best = i;
			}
		}
		return best;
	}
	return row->default_index;
}

static void TieVrOptions_Set(const TieVrOptionRow* row, int index) {
	SDL_setenv_unsafe(row->key, row->values[index], 1);
	TieQuest_SaveSetting(row->key, row->values[index]);
	if (row->restart)
		s_restart_needed = true;
}

void TieVrOptions_Draw(AeronUiContext* ui) {
	AeronUi_Header(ui, "Virtual Reality");
	for (int i = 0; i < TIE_VR_ROW_COUNT; ++i) {
		const TieVrOptionRow* row = &k_rows[i];
		/* The rows that apply after a restart come last, under their own
		 * header. */
		if (row->restart && (i == 0 || !k_rows[i - 1].restart)) {
			AeronUi_Spacer(ui, 8.0f);
			AeronUi_Header(ui, "Headset (restart to apply)");
		}
		int index = TieVrOptions_Current(row);
		if (AeronUi_Selector(ui, row->label, &index, row->labels, row->count))
			TieVrOptions_Set(row, index);
		if (row->help && !row->restart)
			AeronUi_Help(ui, row->help);
		if (strcmp(row->key, "XWA_XR_HUD_LAYOUT") == 0) {
			if (AeronUi_ButtonEnabled(ui, "Reset HUD Positions", TieVrOptions_HudPosesSet()))
				TieVrOptions_ResetHudPoses();
		}
		if (strcmp(row->key, "XWA_XR_CONSOLE") == 0) {
			const char* pose = SDL_getenv("XWA_XR_CONSOLE_POSE");
			if (AeronUi_ButtonEnabled(ui, "Reset Console Position", pose && pose[0])) {
				SDL_setenv_unsafe("XWA_XR_CONSOLE_POSE", "", 1);
				TieQuest_SaveSetting("XWA_XR_CONSOLE_POSE", "");
			}
		}
	}
	AeronUi_Spacer(ui, 8.0f);
	if (AeronUi_Button(ui, "Restore VR Defaults")) {
		for (int i = 0; i < TIE_VR_ROW_COUNT; ++i)
			TieVrOptions_Set(&k_rows[i], k_rows[i].default_index);
		SDL_setenv_unsafe("XWA_XR_CONSOLE_POSE", "", 1);
		TieQuest_SaveSetting("XWA_XR_CONSOLE_POSE", "");
		TieVrOptions_ResetHudPoses();
		/* The flight stick back to its own spot. */
		SDL_unsetenv_unsafe("XWA_XR_STICK_POS");
		TieQuest_SaveSetting("XWA_XR_STICK_POS", "");
	}
	if (s_restart_needed)
		AeronUi_Error(ui, "Some changes will take effect after restarting the game.");
	AeronUi_Help(ui, "More settings: opentie-quest.cfg in the app's folder on the headset.");
}

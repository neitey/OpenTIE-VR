#include "tie_runtime/input/vr_controls.h"

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "tie/tie.h"
#include "tie_runtime/input/actions.h"
#include "tie_runtime/input/controller_mapping.h"
#include "tie_runtime/input/input.h"
#include "tie_runtime/input/vr_buttons.h"
#include "tie_runtime/runtime/flight_requests.h"
#include "tie_runtime/runtime/flight_screen.h"
#include "tie_runtime/snapshot/snapshot.h"

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <math.h>
#include <string.h>

static struct {
	bool active;  /* modes were set for a presenting session */
	bool flight;  /* this frame's mode is flight */
	int pointer;  /* last Aeron_XrSetPointerEnabled value, -1 unknown */
	int buttons;  /* last Aeron_XrSetFlightButtons value, -1 unknown */
	bool logged;
} g_vr = { .pointer = -1, .buttons = -1 };

/* ---- helpers ---- */

static bool TieVrControls_LeftHanded(void) { return Aeron_XrLeftHanded() != 0; }

/* The physical controller that plays a game-side hand (0 = left, 1 =
 * right): left-handed controls swap them. */
static int TieVrControls_PhysicalHand(int game_hand) {
	return TieVrControls_LeftHanded() ? 1 - game_hand : game_hand;
}

static float TieVrControls_HandTrigger(int physical_hand) {
	AeronXrHand hand;
	return Aeron_XrGetHand(physical_hand, &hand) ? hand.trigger : 0.0f;
}

/* The shift layer: the (game-side) left grip held. */
static bool TieVrControls_ShiftHeld(void) {
	AeronXrHand hand;
	return Aeron_XrGetHand(TieVrControls_PhysicalHand(0), &hand) && hand.grip >= TIE_VR_SHIFT_GRIP;
}

static bool TieVrControls_HapticsEnabled(void) {
	const char* value = SDL_getenv("TIE_XR_HAPTICS");
	return !(value && value[0] == '0');
}

static void TieVrControls_Dispatch(TieInputAction action) {
	if (action == TIE_INPUT_ACTION_NONE)
		return;
	TieInputActions_DispatchController(action, true);
	TieInputActions_DispatchController(action, false);
}

static void TieVrControls_SetPointer(int enabled) {
	if (enabled != g_vr.pointer) {
		g_vr.pointer = enabled;
		Aeron_XrSetPointerEnabled(enabled);
	}
}

static void TieVrControls_SetButtons(int enabled) {
	if (enabled != g_vr.buttons) {
		g_vr.buttons = enabled;
		Aeron_XrSetFlightButtons(enabled);
	}
}

float TieVrControls_LeverForThrottle(uint16_t throttle) {
	/* Inverse of TieControllerMapping_ThrottlePosition for a gamepad
	 * trigger axis (its endpoint saturation included). */
	const float end_tolerance = ceilf(TIE_INPUT_THROTTLE_ENDPOINT_PERCENT * UINT16_MAX / 100.0f);
	const float range = (float)UINT16_MAX - 2.0f * end_tolerance;
	if (throttle == 0)
		return 0.0f;
	if (throttle == UINT16_MAX)
		return 1.0f;
	const float position = end_tolerance + (float)throttle * range / (float)UINT16_MAX;
	return position / (float)UINT16_MAX;
}

/* ---- flight: chord (both triggers) ---- */

/* Both triggers pulled together: Space when the game asks for it (dock /
 * tractor prompt, end-mission prompt, missile warning, reinforcements),
 * hyperspace ('h') with the left grip held. While either is on offer the
 * left trigger's own action waits for its release, so the chord does not
 * also change weapons. */
static struct {
	bool down[2];
	uint64_t down_ms[2];
	bool fired;           /* chord fired; wait for both triggers released */
	bool trigger_pending; /* deferred left-trigger press */
	bool trigger_shifted; /* its layer */
} g_chord;

static bool TieVrControls_ChordOffered(bool shift) {
	return pstate.space_confirm_action != 0 || shift;
}

static void TieVrControls_UpdateChord(bool enabled, bool shift) {
	const uint64_t now = SDL_GetTicks();
	const int left = TieVrControls_PhysicalHand(0); /* the weapon trigger */
	if (!enabled) {
		memset(&g_chord, 0, sizeof g_chord);
		return;
	}
	for (int h = 0; h < 2; ++h) {
		const float t = TieVrControls_HandTrigger(h);
		if (!g_chord.down[h] && t >= TIE_VR_CHORD_PULL) {
			g_chord.down[h] = true;
			g_chord.down_ms[h] = now;
		} else if (g_chord.down[h] && t < TIE_VR_CHORD_RELEASE) {
			g_chord.down[h] = false;
		}
		if (h == left && g_chord.trigger_pending && t < TIE_VR_CHORD_RELEASE) {
			/* A plain left-trigger pull after all (it may never have
			 * reached the chord's travel). */
			if (!g_chord.fired)
				TieVrControls_Dispatch(TieVrButtons_Action(AERON_XR_BUTTON_LEFT_TRIGGER, g_chord.trigger_shifted));
			g_chord.trigger_pending = false;
		}
	}
	if (!g_chord.down[0] && !g_chord.down[1])
		g_chord.fired = false;
	if (g_chord.fired || !g_chord.down[0] || !g_chord.down[1] || !TieVrControls_ChordOffered(shift))
		return;
	const uint64_t a = g_chord.down_ms[0], b = g_chord.down_ms[1];
	if ((a > b ? a - b : b - a) > TIE_VR_CHORD_WINDOW_MS)
		return;
	g_chord.fired = true;
	g_chord.trigger_pending = false;
	if (pstate.space_confirm_action)
		TieVrControls_Dispatch(TIE_INPUT_ACTION_CONFIRM);
	else
		TieVrControls_Dispatch(TIE_INPUT_ACTION_HYPERSPACE);
	if (TieVrControls_HapticsEnabled()) {
		Aeron_XrHapticPulse(0, 0.4f, 40);
		Aeron_XrHapticPulse(1, 0.4f, 40);
	}
}

/* ---- flight: throttle lever ---- */

/* The lever is an absolute throttle and the flight input only acts on it
 * when it moves (TieInput_ReadThrottleCommand), so a lever left at full
 * from the last mission, or moved while the game set the throttle itself
 * (keys, match speed, a new mission), would do nothing until pulled past
 * where it stood. While it rests, it follows the craft's real throttle. */
static void TieVrControls_SyncThrottleLever(bool flight) {
	static float last_lever = -1.0f;
	static uint64_t resting_since_ms;
	const uint64_t now = SDL_GetTicks();
	if (!flight) {
		last_lever = -1.0f;
		return;
	}
	const float lever = Aeron_XrThrottleLever();
	if (lever != last_lever) {
		last_lever = lever;
		resting_since_ms = now;
		return;
	}
	const CraftData* craft = pstate.player_craft;
	/* Resting for a moment: the steps the lever sent have landed. */
	if (!craft || now - resting_since_ms < 600u)
		return;
	const float actual = TieVrControls_LeverForThrottle(craft->throttle_speed);
	if (fabsf(actual - lever) <= 0.6f / 16.0f)
		return;
	Aeron_XrSetThrottleLever(actual);
	last_lever = Aeron_XrThrottleLever();
}

/* ---- flight: haptics ---- */

static void TieVrControls_UpdateHaptics(bool flight) {
	static const CraftData* craft_seen;
	static uint32_t shots;
	static int32_t shields;
	static uint16_t hull;
	const CraftData* craft = flight ? pstate.player_craft : NULL;
	if (!craft) {
		craft_seen = NULL;
		return;
	}
	const uint32_t now_shots = (uint32_t)craft->laser_fired + craft->missile_fired + craft->warhead_fired;
	const int32_t now_shields = (int32_t)craft->forward_shield + craft->rear_shield;
	const uint16_t now_hull = craft->hull_damage;
	if (craft == craft_seen && TieVrControls_HapticsEnabled()) {
		if (now_shots > shots)
			Aeron_XrHapticPulse(TieVrControls_PhysicalHand(1), 0.3f, 20);
		if (now_shields < shields || now_hull > hull) {
			const float strength = now_hull > hull ? 0.9f : 0.6f;
			Aeron_XrHapticPulse(0, strength, 60);
			Aeron_XrHapticPulse(1, strength, 60);
		}
	}
	craft_seen = craft;
	shots = now_shots;
	shields = now_shields;
	hull = now_hull;
}

/* ---- buttons ---- */

static void TieVrControls_FlightButton(AeronXrButton button, bool shifted, bool chord_offered) {
	if (button == AERON_XR_BUTTON_LEFT_TRIGGER && chord_offered) {
		/* Decided on release (or dropped by the chord). */
		if (!g_chord.fired) {
			g_chord.trigger_pending = true;
			g_chord.trigger_shifted = shifted;
		}
		return;
	}
	TieVrControls_Dispatch(TieVrButtons_Action(button, shifted));
}

static void TieVrControls_MenuButton(AeronXrButton button, bool flight_scene) {
	if (flight_scene && TieFlightPause_IsActive()) {
		/* The game's own pause ('p'): A or B resumes. */
		if (button == AERON_XR_BUTTON_A || button == AERON_XR_BUTTON_B)
			TieVrControls_Dispatch(TIE_INPUT_ACTION_PAUSE);
		return;
	}
	const int key = TieVrButtons_MenuKey(button);
	if (key)
		TieInput_EnqueueKey((int16_t)key);
}

bool TieVrControls_InFlight(void) { return g_vr.flight; }

uint32_t TieVrControls_Frame(bool settings_open, bool host_paused) {
	uint32_t requests = 0;
	if (!Aeron_XrIsPresenting()) {
		if (g_vr.active) {
			TieVrControls_SetButtons(0);
			TieVrControls_SetPointer(1);
			TieControllerMapping_SetXrFlight(false);
			TieVrControls_UpdateChord(false, false);
			TieVrControls_SyncThrottleLever(false);
			TieVrControls_UpdateHaptics(false);
			g_vr.active = false;
			g_vr.flight = false;
		}
		return 0;
	}
	if (!g_vr.logged) {
		Aeron_LogInfo("tie.input", "VR controls: Touch flight buttons + virtual gamepad (left-handed %d)",
					  TieVrControls_LeftHanded() ? 1 : 0);
		g_vr.logged = true;
	}
	g_vr.active = true;
	Aeron_XrSetMenuKeys(0);

	const TieSnapshot* snapshot = TieSnapshot_Current();
	const bool flight_scene = snapshot && snapshot->scene_kind == TIE_SCENE_FLIGHT;
	const bool flight = flight_scene && !settings_open && !host_paused &&
						snapshot->flight_screen == TIE_FLIGHT_SCREEN_NORMAL && snapshot->replay_mode != 2 &&
						!replayviewmode && !TieFlightPause_IsActive() && pstate.player_craft != NULL;
	g_vr.flight = flight;

	TieVrControls_SetPointer(flight ? 0 : 1);
	TieVrControls_SetButtons(settings_open ? 0 : 1);
	TieControllerMapping_SetXrFlight(flight);

	/* B in the menus: Aeron latches it while the pointer is on. The settings
	 * menu has no flight buttons (its tabs use X/Y as shoulders), so B
	 * closes it from here; elsewhere B is a flight button (below). */
	if (Aeron_XrTakeMenuBPressed() && settings_open)
		requests |= TIE_VR_REQUEST_CLOSE_SETTINGS;

	const bool shift = flight && TieVrControls_ShiftHeld();
	TieVrControls_UpdateChord(flight, shift);
	const bool chord_offered = flight && TieVrControls_ChordOffered(shift);
	if (!flight)
		g_chord.trigger_pending = false;

	AeronXrButton button;
	int engine_shifted = 0;
	while (Aeron_XrPollButton(&button, &engine_shifted)) {
		if ((int)button < 0 || (int)button >= AERON_XR_BUTTON_COUNT || settings_open)
			continue;
		if (flight)
			TieVrControls_FlightButton(button, shift || engine_shifted, chord_offered);
		else
			TieVrControls_MenuButton(button, flight_scene);
	}

	TieVrControls_SyncThrottleLever(flight);
	TieVrControls_UpdateHaptics(flight);
	return requests;
}

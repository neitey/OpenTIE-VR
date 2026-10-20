#include "tie_runtime/input/vr_buttons.h"

#include <stdio.h>
#include <stdlib.h>

/* Flight buttons, one action each per layer. Left-handed controls swap the
 * controllers under these in Aeron (Aeron_XrLeftHanded), so the table
 * stays as it is. Plain layer as in OpenXWA-VR where TIE has the action
 * (A/B/X/Y targeting, left trigger weapon, left stick click linking); the
 * right grip is "target in sights" (TIE has no countermeasures) and the
 * right stick click matches speed. The left grip held selects the shifted
 * layer: energy management and the rarer targeting keys. */
static const TieVrButtonBinding k_bindings[] = {
	{ AERON_XR_BUTTON_A, false, TIE_INPUT_ACTION_TARGET_NEXT, "next target" },
	{ AERON_XR_BUTTON_B, false, TIE_INPUT_ACTION_TARGET_PREV, "previous target" },
	{ AERON_XR_BUTTON_X, false, TIE_INPUT_ACTION_TARGET_NEAREST_FIGHTER_OR_MINE, "nearest fighter" },
	{ AERON_XR_BUTTON_Y, false, TIE_INPUT_ACTION_TARGET_MY_ATTACKER, "my attacker" },
	{ AERON_XR_BUTTON_LEFT_TRIGGER, false, TIE_INPUT_ACTION_CYCLE_WEAPON_GROUP, "change weapon" },
	{ AERON_XR_BUTTON_LEFT_STICK, false, TIE_INPUT_ACTION_CYCLE_WEAPON_FIRING_MODE, "firing mode" },
	{ AERON_XR_BUTTON_RIGHT_STICK, false, TIE_INPUT_ACTION_MATCH_SPEED, "match speed" },
	{ AERON_XR_BUTTON_RIGHT_GRIP, false, TIE_INPUT_ACTION_AUTO_TARGET, "target in sights" },

	{ AERON_XR_BUTTON_A, true, TIE_INPUT_ACTION_CYCLE_SHIELD_RECHARGE_RATE, "shield recharge" },
	{ AERON_XR_BUTTON_B, true, TIE_INPUT_ACTION_CYCLE_CANNON_RECHARGE_RATE, "laser recharge" },
	{ AERON_XR_BUTTON_X, true, TIE_INPUT_ACTION_XFER_CANNON_TO_SHIELDS, "lasers to shields" },
	{ AERON_XR_BUTTON_Y, true, TIE_INPUT_ACTION_XFER_SHIELDS_TO_CANNON, "shields to lasers" },
	{ AERON_XR_BUTTON_LEFT_TRIGGER, true, TIE_INPUT_ACTION_TOGGLE_BEAM, "beam on/off" },
	{ AERON_XR_BUTTON_LEFT_STICK, true, TIE_INPUT_ACTION_CYCLE_SHIELD_MODE, "shield direction" },
	{ AERON_XR_BUTTON_RIGHT_STICK, true, TIE_INPUT_ACTION_END_MISSION, "end mission" },
	{ AERON_XR_BUTTON_RIGHT_GRIP, true, TIE_INPUT_ACTION_TARGET_NEWEST_CRAFT, "newest craft" },
};

const TieVrButtonBinding* TieVrButtons_Table(size_t* count) {
	if (count)
		*count = sizeof k_bindings / sizeof k_bindings[0];
	return k_bindings;
}

TieInputAction TieVrButtons_Action(AeronXrButton button, bool shifted) {
	for (size_t i = 0; i < sizeof k_bindings / sizeof k_bindings[0]; ++i)
		if (k_bindings[i].button == button && k_bindings[i].shifted == shifted)
			return k_bindings[i].action;
	return TIE_INPUT_ACTION_NONE;
}

bool TieVrButtons_SticksSwapped(void) {
	const char* value = getenv("XWA_XR_SWAP_STICKS");
	return value && value[0] == '1';
}

int TieVrButtons_MenuKey(AeronXrButton button) {
	switch (button) {
		case AERON_XR_BUTTON_A:
			return 13; /* Enter */
		case AERON_XR_BUTTON_B:
			return 27; /* Escape: one screen back */
		case AERON_XR_BUTTON_X:
			return ' ';
		default:
			return 0;
	}
}

const char* TieVrButtons_ButtonName(AeronXrButton button, bool compact, bool left_handed) {
	static const char* const names[AERON_XR_BUTTON_COUNT][2] = {
		[AERON_XR_BUTTON_A] = { "A", "A" },
		[AERON_XR_BUTTON_B] = { "B", "B" },
		[AERON_XR_BUTTON_X] = { "X", "X" },
		[AERON_XR_BUTTON_Y] = { "Y", "Y" },
		[AERON_XR_BUTTON_LEFT_TRIGGER] = { "left trigger", "L-trigger" },
		[AERON_XR_BUTTON_LEFT_STICK] = { "left stick click", "L-stick click" },
		[AERON_XR_BUTTON_RIGHT_STICK] = { "right stick click", "R-stick click" },
		[AERON_XR_BUTTON_RIGHT_GRIP] = { "right grip", "R-grip" },
	};
	/* Left-handed: the control on the other controller (aeron.h). */
	static const char* const mirrored[AERON_XR_BUTTON_COUNT][2] = {
		[AERON_XR_BUTTON_A] = { "X", "X" },
		[AERON_XR_BUTTON_B] = { "Y", "Y" },
		[AERON_XR_BUTTON_X] = { "A", "A" },
		[AERON_XR_BUTTON_Y] = { "B", "B" },
		[AERON_XR_BUTTON_LEFT_TRIGGER] = { "right trigger", "R-trigger" },
		[AERON_XR_BUTTON_LEFT_STICK] = { "right stick click", "R-stick click" },
		[AERON_XR_BUTTON_RIGHT_STICK] = { "left stick click", "L-stick click" },
		[AERON_XR_BUTTON_RIGHT_GRIP] = { "left grip", "L-grip" },
	};
	if ((int)button < 0 || (int)button >= AERON_XR_BUTTON_COUNT)
		return "?";
	return (left_handed ? mirrored : names)[button][compact ? 1 : 0];
}

const char* TieVrButtons_ControlName(TieVrControl control, bool compact) {
	static const char* const names[TIE_VR_CONTROL_COUNT][2] = {
		[TIE_VR_CONTROL_RIGHT_STICK] = { "right stick", "R-stick" },
		[TIE_VR_CONTROL_LEFT_STICK_X] = { "left stick left/right", "L-stick X" },
		[TIE_VR_CONTROL_LEFT_STICK_Y] = { "left stick forward/back", "L-stick Y" },
		[TIE_VR_CONTROL_RIGHT_TRIGGER] = { "right trigger", "R-trigger" },
		[TIE_VR_CONTROL_LEFT_GRIP] = { "left grip (hold)", "L-grip" },
		[TIE_VR_CONTROL_MENU] = { "menu button", "menu" },
		[TIE_VR_CONTROL_CHORD] = { "both triggers", "both triggers" },
		[TIE_VR_CONTROL_SHIFT_CHORD] = { "grip + both triggers", "grip+both triggers" },
	};
	if ((int)control < 0 || control >= TIE_VR_CONTROL_COUNT)
		return "?";
	return names[control][compact ? 1 : 0];
}

const char* TieVrButtons_ControlLabel(TieVrControl control) {
	static const char* const labels[TIE_VR_CONTROL_COUNT] = {
		[TIE_VR_CONTROL_RIGHT_STICK] = "yaw / pitch",
		[TIE_VR_CONTROL_LEFT_STICK_X] = "roll",
		[TIE_VR_CONTROL_LEFT_STICK_Y] = "throttle lever",
		[TIE_VR_CONTROL_RIGHT_TRIGGER] = "fire",
		[TIE_VR_CONTROL_LEFT_GRIP] = "shift",
		[TIE_VR_CONTROL_MENU] = "settings",
		[TIE_VR_CONTROL_CHORD] = "confirm (Space)",
		[TIE_VR_CONTROL_SHIFT_CHORD] = "hyperspace",
	};
	if ((int)control < 0 || control >= TIE_VR_CONTROL_COUNT)
		return "?";
	return labels[control];
}

const char* TieVrButtons_ActionControl(TieInputAction action, bool compact, bool left_handed) {
	static char label[48];
	if (action == TIE_INPUT_ACTION_FIRE_WEAPON)
		return TieVrButtons_ControlName(TIE_VR_CONTROL_RIGHT_TRIGGER, compact);
	if (action == TIE_INPUT_ACTION_CONFIRM)
		return TieVrButtons_ControlName(TIE_VR_CONTROL_CHORD, compact);
	if (action == TIE_INPUT_ACTION_HYPERSPACE)
		return TieVrButtons_ControlName(TIE_VR_CONTROL_SHIFT_CHORD, compact);
	/* A plain press wins over a shifted one. */
	for (int shifted = 0; shifted < 2; ++shifted)
		for (size_t i = 0; i < sizeof k_bindings / sizeof k_bindings[0]; ++i) {
			const TieVrButtonBinding* b = &k_bindings[i];
			if (b->action != action || b->shifted != (shifted != 0))
				continue;
			snprintf(label, sizeof label, "%s%s", shifted ? (compact ? "grip+" : "grip + ") : "",
					 TieVrButtons_ButtonName(b->button, compact, left_handed));
			return label;
		}
	return NULL;
}

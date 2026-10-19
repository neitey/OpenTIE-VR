#include "tie_runtime/input/vr_console_game.h"

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "tie/tie.h"
#include "tie_runtime/input/actions.h"
#include "tie_runtime/input/vr_controls.h"
#include "tie_runtime/snapshot/snapshot.h"

#include <string.h>

/* The player's craft while the console may be used: VR cockpit flight
 * (vr_controls' flight mode: normal flight screen, no pause, menu or
 * replay) seen from a cockpit view, not an external camera (16+). */
static CraftData* TieVrConsoleGame_Craft(void) {
	if (!Aeron_XrIsPresenting() || !TieVrControls_InFlight() || camera.pilotview >= 16)
		return NULL;
	return pstate.player_craft;
}

static int TieVrConsoleGame_Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void TieVrConsoleGame_GetState(TieVrConsoleState* out) {
	if (!out)
		return;
	memset(out, 0, sizeof *out);
	out->player_slot = out->target_slot = -1;
	const TieSnapshot* snap = TieSnapshot_Current();
	if (snap)
		out->mission_serial = snap->mission_load_generation;
	const CraftData* craft = TieVrConsoleGame_Craft();
	if (!craft)
		return;
	out->valid = 1;
	out->laser_level = TieVrConsoleGame_Clamp(craft->laser_power, 0, 4);
	out->shield_level = TieVrConsoleGame_Clamp(craft->shield_power, 0, 4);
	out->beam_level = TieVrConsoleGame_Clamp(craft->beam_power, 0, 4);
	/* As the key handlers (user.c F9/F10/F8, 's', 'b') test them. */
	out->has_lasers = (craft->status_flags & 0x10) != 0;
	out->has_shields = (craft->subsystem_active & 1) != 0;
	out->has_beam = (craft->subsystem_active & 0x100) != 0;
	out->beam_on = out->has_beam && (craft->status_flags & 0x100) != 0;
	out->has_hyperdrive = (craft->subsystem_active & 0x80) != 0;
	out->shield_mode = TieVrConsoleGame_Clamp(craft->is_player_craft, 0, 2); /* +0x0CF: the shield mode */
	if (out->has_shields && pstate.player_spec_num < NUM_SPEC_DATA) {
		int cap = (int)spec_data[pstate.player_spec_num].shield_points * 2;
		if (!mission.difficulty)
			cap *= 2;
		if (cap > 0) {
			out->shield_front = TieVrConsoleGame_Clamp(craft->forward_shield * 100 / cap, 0, 100);
			out->shield_rear = TieVrConsoleGame_Clamp(craft->rear_shield * 100 / cap, 0, 100);
		}
	}
	out->hull = craft->hull_max > 0
					? TieVrConsoleGame_Clamp(100 - (int)craft->hull_damage * 100 / (int)craft->hull_max, 0, 100)
					: 100;
	out->throttle = (int)((uint32_t)craft->throttle_speed * 100u / 65535u);
	out->has_target = pstate.target_obj_idx != 0xFFFF;
	out->player_slot = pstate.object_idx < NUM_OBJECTS ? (int)pstate.object_idx : -1;
	out->target_slot = out->has_target && pstate.target_obj_idx < NUM_OBJECTS ? (int)pstate.target_obj_idx : -1;
	for (int i = 0; i < 3; ++i)
		out->preset_set[i] = pstate.target_presets[i] != 0xFFFF;
	out->confirm_offered = pstate.space_confirm_action != 0;
}

static void TieVrConsoleGame_Press(TieInputAction action, int times) {
	for (int i = 0; i < times; ++i) {
		TieInputActions_DispatchController(action, true);
		TieInputActions_DispatchController(action, false);
	}
}

void TieVrConsoleGame_Action(TieVrConsoleAction action, int arg) {
	CraftData* craft = TieVrConsoleGame_Craft();
	if (!craft)
		return;
	Aeron_LogInfo("tie.vrconsole", "%s %d",
				  action == TIE_VRC_INPUT ? TieInputActions_ToName((TieInputAction)arg)
				  : action == TIE_VRC_LASER_LEVEL  ? "laser level"
				  : action == TIE_VRC_SHIELD_LEVEL ? "shield level"
				  : action == TIE_VRC_BEAM_LEVEL   ? "beam level"
				  : action == TIE_VRC_SHIELD_MODE  ? "shield mode"
												   : "?",
				  arg);
	switch (action) {
		case TIE_VRC_INPUT:
			if (arg > TIE_INPUT_ACTION_NONE && arg < TIE_INPUT_ACTION_COUNT)
				TieVrConsoleGame_Press((TieInputAction)arg, 1);
			break;
		case TIE_VRC_LASER_LEVEL:
			/* The keys step up one level, MAX wraps to OFF (user.c): as many
			 * presses as it takes to reach `arg` (none when it is set). */
			TieVrConsoleGame_Press(TIE_INPUT_ACTION_CYCLE_CANNON_RECHARGE_RATE,
								   (TieVrConsoleGame_Clamp(arg, 0, 4) - craft->laser_power + 5) % 5);
			break;
		case TIE_VRC_SHIELD_LEVEL:
			TieVrConsoleGame_Press(TIE_INPUT_ACTION_CYCLE_SHIELD_RECHARGE_RATE,
								   (TieVrConsoleGame_Clamp(arg, 0, 4) - craft->shield_power + 5) % 5);
			break;
		case TIE_VRC_BEAM_LEVEL:
			TieVrConsoleGame_Press(TIE_INPUT_ACTION_CYCLE_BEAM_RECHARGE_RATE,
								   (TieVrConsoleGame_Clamp(arg, 0, 4) - craft->beam_power + 5) % 5);
			break;
		case TIE_VRC_SHIELD_MODE:
			TieVrConsoleGame_Press(TIE_INPUT_ACTION_CYCLE_SHIELD_MODE,
								   (TieVrConsoleGame_Clamp(arg, 0, 2) - craft->is_player_craft + 3) % 3);
			break;
		default:
			break;
	}
}

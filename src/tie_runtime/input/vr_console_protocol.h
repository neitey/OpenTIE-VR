/*
 * Data shared between the VR lap console / quick wheel / side panel
 * (tie_remaster/flight/vr_console.c, which draws them and handles the
 * hands) and the game side (tie_runtime/input/vr_console_game.c, which
 * reads the player's craft and turns presses into TIE's own input
 * actions). Plain data; the host passes it across each frame. TIE port of
 * OpenXWA-VR's xwa_remaster/vr_console_protocol.h.
 */
#ifndef TIE_RUNTIME_INPUT_VR_CONSOLE_PROTOCOL_H
#define TIE_RUNTIME_INPUT_VR_CONSOLE_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum TieVrConsoleAction {
	TIE_VRC_NONE = 0,
	/* arg: a TieInputAction, dispatched as the key binding would. */
	TIE_VRC_INPUT,
	/* Recharge rates, arg = the level wanted (0 off .. 4 max): the game's
	 * F9 / F10 / F8 cycle as often as it takes. */
	TIE_VRC_LASER_LEVEL,
	TIE_VRC_SHIELD_LEVEL,
	TIE_VRC_BEAM_LEVEL,
	/* arg 0 front, 1 even, 2 rear: the game's 'S' cycle. */
	TIE_VRC_SHIELD_MODE,
	TIE_VRC_COUNT
} TieVrConsoleAction;

/* Live ship state the console shows. */
typedef struct TieVrConsoleState {
	int valid; /* flying a craft from the cockpit (else the panels are hidden) */
	int laser_level, shield_level, beam_level; /* 0 off .. 4 max (2 normal) */
	int has_lasers, has_shields, has_beam, has_hyperdrive;
	int beam_on;
	int shield_mode;              /* 0 front, 1 even, 2 rear */
	int shield_front, shield_rear; /* 0..100 % of one side's capacity */
	int hull;                      /* 0..100 % hull left */
	int throttle;                  /* 0..100 % */
	int has_target;
	int preset_set[3]; /* the F5..F7 target memories hold a craft */
	int confirm_offered; /* the game waits for Space (dock, end mission, ...) */
	int player_slot;     /* the player's craft in TieSnapshot.flights[].slot, -1 none */
	int target_slot;     /* the current target's slot, -1 none */
	/* Changes when a new mission starts (the side panel returns to its
	 * spot). */
	unsigned int mission_serial;
} TieVrConsoleState;

#ifdef __cplusplus
}
#endif

#endif

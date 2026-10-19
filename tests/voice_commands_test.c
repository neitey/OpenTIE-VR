/* Voice command matcher test (host): every phrase in the table must come
 * back as its own command, and a set of recognizer-like transcripts must
 * match (or be rejected) as expected.
 *   ninja tie_voice_test && ./tie_voice_test
 * With arguments, prints what each argument matches instead. */

#include "tie_app/quest/voice_commands.h"

#include <stdio.h>
#include <string.h>

typedef struct VoiceCase {
	const char* heard;
	TieInputAction expect; /* TIE_INPUT_ACTION_NONE: must be rejected */
} VoiceCase;

static const VoiceCase k_cases[] = {
	/* As the recognizer writes it (upper case, no punctuation). */
	{ "ATTACK MY TARGET", TIE_INPUT_ACTION_COMM_ASSIGN_TARGET },
	{ "WINGMEN ATTACK MY TARGET", TIE_INPUT_ACTION_COMM_ASSIGN_TARGET },
	{ "uh attack my target please", TIE_INPUT_ACTION_COMM_ASSIGN_TARGET },
	{ "cover me", TIE_INPUT_ACTION_COMM_COVER_ME },
	{ "cover my six", TIE_INPUT_ACTION_COMM_COVER_ME },
	{ "ignore my target", TIE_INPUT_ACTION_COMM_IGNORE_TARGET },
	{ "evasive maneuvers", TIE_INPUT_ACTION_COMM_EVADE },
	{ "evade", TIE_INPUT_ACTION_COMM_EVADE },
	{ "hold position", TIE_INPUT_ACTION_COMM_STOP_AND_WAIT },
	{ "stop and wait", TIE_INPUT_ACTION_COMM_STOP_AND_WAIT },
	{ "go ahead", TIE_INPUT_ACTION_COMM_CONTINUE_MISSION },
	{ "return to base", TIE_INPUT_ACTION_COMM_HEAD_HOME },
	{ "RTB", TIE_INPUT_ACTION_COMM_HEAD_HOME },
	{ "go home", TIE_INPUT_ACTION_COMM_HEAD_HOME },
	{ "report in", TIE_INPUT_ACTION_COMM_REPORT_ORDERS },
	{ "request resupply", TIE_INPUT_ACTION_COMM_REQUEST_RESUPPLY },
	{ "call for reinforcements", TIE_INPUT_ACTION_COMM_REINFORCEMENTS },
	{ "next target", TIE_INPUT_ACTION_TARGET_NEXT },
	{ "previous target", TIE_INPUT_ACTION_TARGET_PREV },
	{ "target nearest enemy", TIE_INPUT_ACTION_TARGET_NEAREST_FIGHTER_OR_MINE },
	{ "closest bandit", TIE_INPUT_ACTION_TARGET_NEAREST_FIGHTER_OR_MINE },
	{ "nearest fighter", TIE_INPUT_ACTION_TARGET_NEAREST_FIGHTER_OR_MINE },
	{ "who's shooting at me", TIE_INPUT_ACTION_TARGET_MY_ATTACKER },
	{ "target my attacker", TIE_INPUT_ACTION_TARGET_MY_ATTACKER },
	{ "who is attacking my target", TIE_INPUT_ACTION_TARGET_ATTACKER_CHAIN },
	{ "target my target's attacker", TIE_INPUT_ACTION_TARGET_ATTACKER_CHAIN },
	{ "target newest craft", TIE_INPUT_ACTION_TARGET_NEWEST_CRAFT },
	{ "new arrival", TIE_INPUT_ACTION_TARGET_NEWEST_CRAFT },
	{ "clear target", TIE_INPUT_ACTION_TARGET_CLEAR },
	{ "target in sights", TIE_INPUT_ACTION_AUTO_TARGET },
	{ "next component", TIE_INPUT_ACTION_TARGET_COMPONENT_NEXT },
	{ "next sub system", TIE_INPUT_ACTION_TARGET_COMPONENT_NEXT },
	{ "previous component", TIE_INPUT_ACTION_TARGET_COMPONENT_PREV },
	{ "change weapon", TIE_INPUT_ACTION_CYCLE_WEAPON_GROUP },
	{ "switch to warheads", TIE_INPUT_ACTION_CYCLE_WEAPON_GROUP },
	{ "link lasers", TIE_INPUT_ACTION_CYCLE_WEAPON_FIRING_MODE },
	{ "fire mode", TIE_INPUT_ACTION_CYCLE_WEAPON_FIRING_MODE },
	{ "toggle beam", TIE_INPUT_ACTION_TOGGLE_BEAM },
	{ "tractor beam", TIE_INPUT_ACTION_TOGGLE_BEAM },
	{ "lasers to shields", TIE_INPUT_ACTION_XFER_CANNON_TO_SHIELDS },
	{ "transfer energy to shields", TIE_INPUT_ACTION_XFER_CANNON_TO_SHIELDS },
	{ "shields to lasers", TIE_INPUT_ACTION_XFER_SHIELDS_TO_CANNON },
	{ "shields forward", TIE_INPUT_ACTION_CYCLE_SHIELD_MODE },
	{ "shield direction", TIE_INPUT_ACTION_CYCLE_SHIELD_MODE },
	{ "laser recharge rate", TIE_INPUT_ACTION_CYCLE_CANNON_RECHARGE_RATE },
	{ "recharge lasers", TIE_INPUT_ACTION_CYCLE_CANNON_RECHARGE_RATE },
	{ "shield recharge", TIE_INPUT_ACTION_CYCLE_SHIELD_RECHARGE_RATE },
	{ "recharge shields", TIE_INPUT_ACTION_CYCLE_SHIELD_RECHARGE_RATE },
	{ "beam recharge", TIE_INPUT_ACTION_CYCLE_BEAM_RECHARGE_RATE },
	{ "full throttle", TIE_INPUT_ACTION_THROTTLE_FULL },
	{ "full speed", TIE_INPUT_ACTION_THROTTLE_FULL },
	{ "full stop", TIE_INPUT_ACTION_THROTTLE_ZERO },
	{ "zero throttle", TIE_INPUT_ACTION_THROTTLE_ZERO },
	{ "one third throttle", TIE_INPUT_ACTION_THROTTLE_ONE_THIRD },
	{ "one third", TIE_INPUT_ACTION_THROTTLE_ONE_THIRD },
	{ "two thirds", TIE_INPUT_ACTION_THROTTLE_TWO_THIRDS },
	{ "to thirds", TIE_INPUT_ACTION_THROTTLE_TWO_THIRDS },
	{ "too thirds throttle", TIE_INPUT_ACTION_THROTTLE_TWO_THIRDS },
	{ "two thirds speed", TIE_INPUT_ACTION_THROTTLE_TWO_THIRDS },
	{ "match speed", TIE_INPUT_ACTION_MATCH_SPEED },
	{ "match their speed", TIE_INPUT_ACTION_MATCH_SPEED },
	{ "show goals", TIE_INPUT_ACTION_INFO_GOALS },
	{ "mission objectives", TIE_INPUT_ACTION_INFO_GOALS },
	{ "map", TIE_INPUT_ACTION_INFO_MAP },
	{ "damage report", TIE_INPUT_ACTION_INFO_DAMAGE },
	{ "messages", TIE_INPUT_ACTION_INFO_MESSAGES },
	{ "message log", TIE_INPUT_ACTION_INFO_MESSAGES },
	{ "hyperspace", TIE_INPUT_ACTION_HYPERSPACE },
	{ "hyper space", TIE_INPUT_ACTION_HYPERSPACE },
	{ "jump to light speed", TIE_INPUT_ACTION_HYPERSPACE },
	{ "pause", TIE_INPUT_ACTION_PAUSE },
	{ "resume", TIE_INPUT_ACTION_PAUSE },
	{ "confirm", TIE_INPUT_ACTION_CONFIRM },
	{ "yes", TIE_INPUT_ACTION_CONFIRM },
	{ "end mission", TIE_INPUT_ACTION_END_MISSION },
	/* Not commands. */
	{ "", TIE_INPUT_ACTION_NONE },
	{ "the", TIE_INPUT_ACTION_NONE },
	{ "what time is it", TIE_INPUT_ACTION_NONE },
	{ "i like turtles", TIE_INPUT_ACTION_NONE },
	{ "banana", TIE_INPUT_ACTION_NONE },
};

static const char* ActionName(TieInputAction a) {
	if (a == TIE_INPUT_ACTION_NONE)
		return "(none)";
	for (int i = 0; i < TieVoiceCommands_Count(); ++i)
		if (TieVoiceCommands_Action(i) == a)
			return TieVoiceCommands_Label(i);
	return "?";
}

int main(int argc, char** argv) {
	if (argc > 1) {
		for (int i = 1; i < argc; ++i) {
			TieInputAction action = TIE_INPUT_ACTION_NONE;
			const char* label = NULL;
			const int ok = TieVoiceCommands_Match(argv[i], &action, &label);
			printf("\"%s\" -> %s\n", argv[i], ok ? label : "(not understood)");
		}
		return 0;
	}
	int failures = 0, checks = 0;
	/* Every phrase is its own command. */
	for (int i = 0; i < TieVoiceCommands_Count(); ++i) {
		for (int k = 0; k < 8 && TieVoiceCommands_Phrase(i, k); ++k) {
			const char* phrase = TieVoiceCommands_Phrase(i, k);
			TieInputAction action = TIE_INPUT_ACTION_NONE;
			const int ok = TieVoiceCommands_Match(phrase, &action, NULL);
			++checks;
			if (!ok || action != TieVoiceCommands_Action(i)) {
				printf("FAIL phrase \"%s\": want %s, got %s\n", phrase, TieVoiceCommands_Label(i),
					   ok ? ActionName(action) : "(not understood)");
				++failures;
			}
		}
	}
	for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; ++i) {
		TieInputAction action = TIE_INPUT_ACTION_NONE;
		const int ok = TieVoiceCommands_Match(k_cases[i].heard, &action, NULL);
		const TieInputAction got = ok ? action : TIE_INPUT_ACTION_NONE;
		++checks;
		if (got != k_cases[i].expect) {
			printf("FAIL \"%s\": want %s, got %s\n", k_cases[i].heard, ActionName(k_cases[i].expect),
				   ActionName(got));
			++failures;
		}
	}
	printf("%d commands, %d checks, %d failures\n", TieVoiceCommands_Count(), checks, failures);
	return failures ? 1 : 0;
}

/*
 * Unit test for the VR key-name rewriting of game text (vr_text.c) on the
 * real TIE strings that name keys (STRINGS.DAT prompts, mission messages).
 *
 *   ninja tie_vr_text_test && ./tie_vr_text_test
 */
#include "tie_runtime/input/vr_text.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static void expect(const char* in, TieVrTextContext context, bool compact, const char* want) {
	char out[256];
	const bool changed = TieVrText_RewriteKeys(in, out, sizeof out, context, compact, false);
	const char* got = changed ? out : in;
	if (strcmp(got, want) != 0) {
		fprintf(stderr, "FAIL: \"%s\"\n  got  \"%s\"\n  want \"%s\"\n", in, got, want);
		++g_failures;
	}
}

static void flight(const char* in, const char* want) { expect(in, TIE_VR_TEXT_FLIGHT, false, want); }

int main(void) {
	/* STRINGS.DAT prompts (type byte first). */
	flight("\004Hit [Space Bar] to activate tractor beam and enter hangar",
		   "\004Pull [both triggers] to activate tractor beam and enter hangar");
	flight("\004Hit [Space Bar] to confirm reinforcement request",
		   "\004Pull [both triggers] to confirm reinforcement request");
	flight("\004Press [Space Bar] to end mission", "\004Pull [both triggers] to end mission");
	/* Mission messages (HB1W, HA1W, HF1W, HD2W, HF2W). */
	flight("Press ['T'] to select your first target, the first container",
		   "Press [A] to select your first target, the first container");
	flight("Press ['W'] to toggle the weapon system to Proton Torpedo Mode",
		   "Pull [left trigger] to toggle the weapon system to Proton Torpedo Mode");
	flight("Press ['R'] to target the nearest enemy (X-W: TARGET 1)",
		   "Press [X] to target the nearest enemy (X-W: TARGET 1)");
	flight("Press ['U'] to target the newest craft (X-W: TARGET 2)",
		   "Press [grip + right grip] to target the newest craft (X-W: TARGET 2)");
	flight("To match speed with the target, press the ['RETURN'] key.",
		   "To match speed with the target, press [right stick click].");
	flight("at you.  Pressing the 'E' key will", "at you.  Pressing Y will");
	flight("Press ['B'] to engage the beam weapon on the current target",
		   "Press [grip + left trigger] to engage the beam weapon on the current target");
	/* No VR control: unchanged. */
	flight("If the missile is evaded, press ['F2'] for the warhead view",
		   "If the missile is evaded, press ['F2'] for the warhead view");
	flight("Target a ship and press Shift-A to have your wingmen attack it",
		   "Target a ship and press Shift-A to have your wingmen attack it");
	flight("Press ['F8'] to set your beam weapon recharge rate", "Press ['F8'] to set your beam weapon recharge rate");
	flight("the 'A' key after targeting one of", "the 'A' key after targeting one of");
	flight("screen, toggled by hitting 'G'.", "screen, toggled by hitting 'G'.");
	flight("If you run out of missiles you have [1] reload available.",
		   "If you run out of missiles you have [1] reload available.");
	/* Prose that only looks like keys. */
	flight("\001Return to base, Alpha 2", "\001Return to base, Alpha 2");
	flight("\001Entering hyperspace", "\001Entering hyperspace");
	flight("\001Keep your shields up and press the attack on the convoy",
		   "\001Keep your shields up and press the attack on the convoy");
	flight("\001Mu 1: I've been hit bad, you'll have to finish without me",
		   "\001Mu 1: I've been hit bad, you'll have to finish without me");
	flight("\001A wing of T/I is coming in, press E", "\001A wing of T/I is coming in, press Y");
	/* Compact names. */
	expect("Press ['W'] to cycle", TIE_VR_TEXT_FLIGHT, true, "Pull [L-trigger] to cycle");
	/* Rooms: Space -> X (menu keys), color codes are separators. */
	expect("Press SPACEBAR or button to continue", TIE_VR_TEXT_MENU, false, "Press X or button to continue");
	expect("\xfeOSPACEBAR\xfeV: Toggles \xfeW2-D\xfeV/3-D mode", TIE_VR_TEXT_MENU, false,
		   "\xfeOX\xfeV: Toggles \xfeW2-D\xfeV/3-D mode");
	expect("\xfeOC\xfeV: Centers map on \xfeOt\xfeVarget", TIE_VR_TEXT_MENU, false,
		   "\xfeOC\xfeV: Centers map on \xfeOt\xfeVarget");
	/* Too long for the buffer: nothing (the caller keeps the original). */
	{
		char out[20];
		if (TieVrText_RewriteKeys("Press ['U'] to target the newest", out, sizeof out, TIE_VR_TEXT_FLIGHT, false,
								  false)) {
			fprintf(stderr, "FAIL: overflow reported as rewritten: \"%s\"\n", out);
			++g_failures;
		}
	}
	if (g_failures) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	printf("vr_text_test: all passed\n");
	return 0;
}

/*
 * Unit test for the VR (Quest Touch) controls: the flight button table,
 * the throttle lever math, the built-in XR gamepad profile and the
 * per-frame dispatch in vr_controls.c, with Aeron's OpenXR input faked at
 * link time (-Wl,--wrap, see CMakeLists.txt: tie_vr_controls_test).
 *
 *   ninja tie_vr_controls_test && ./tie_vr_controls_test
 */
#include "aeron/aeron.h"
#include "aeron/input.h"
#include "tie/tie.h"
#include "tie_runtime/input/actions.h"
#include "tie_runtime/input/controller_mapping.h"
#include "tie_runtime/input/input.h"
#include "tie_runtime/input/vr_buttons.h"
#include "tie_runtime/input/vr_controls.h"
#include "tie_runtime/runtime/flight_screen.h"
#include "tie_runtime/snapshot/snapshot.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL3/SDL_stdinc.h>
#include <string.h>

static int g_failures;
#define CHECK(cond)                                                                                           \
	do {                                                                                                      \
		if (!(cond)) {                                                                                        \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                       \
			++g_failures;                                                                                     \
		}                                                                                                     \
	} while (0)

/* ---- fakes (linked with --wrap) ---- */

static struct {
	int presenting;
	int pointer;
	int flight_buttons;
	int menu_keys;
	int left_handed;
	int b_latched;
	AeronXrButton queue[16];
	int queue_count;
	float trigger[2];
	float grip[2];
	float lever;
	int lever_sets;
	int haptics[2];
	uint64_t ticks;
	int paused;
	TieSnapshot snapshot;
} fake;

int __wrap_Aeron_XrIsPresenting(void) { return fake.presenting; }
void __wrap_Aeron_XrSetPointerEnabled(int enabled) { fake.pointer = enabled; }
void __wrap_Aeron_XrSetFlightButtons(int enabled) { fake.flight_buttons = enabled; }
void __wrap_Aeron_XrSetMenuKeys(int enabled) { fake.menu_keys = enabled; }
int __wrap_Aeron_XrLeftHanded(void) { return fake.left_handed; }
int __wrap_Aeron_XrTakeMenuBPressed(void) {
	const int pressed = fake.b_latched;
	fake.b_latched = 0;
	return pressed;
}
int __wrap_Aeron_XrPollButton(AeronXrButton* button, int* shifted) {
	if (!fake.queue_count)
		return 0;
	*button = fake.queue[0];
	*shifted = 0;
	memmove(fake.queue, fake.queue + 1, (size_t)--fake.queue_count * sizeof fake.queue[0]);
	return 1;
}
int __wrap_Aeron_XrGetHand(int hand, AeronXrHand* out) {
	memset(out, 0, sizeof *out);
	out->valid = 1;
	out->trigger = fake.trigger[hand];
	out->grip = fake.grip[hand];
	return 1;
}
float __wrap_Aeron_XrThrottleLever(void) { return fake.lever; }
void __wrap_Aeron_XrSetThrottleLever(float value) {
	fake.lever = value;
	++fake.lever_sets;
}
void __wrap_Aeron_XrHapticPulse(int hand, float amplitude, int duration_ms) {
	(void)amplitude;
	(void)duration_ms;
	if (hand >= 0 && hand < 2)
		++fake.haptics[hand];
}
uint64_t __wrap_SDL_GetTicks(void) { return fake.ticks; }
const TieSnapshot* __wrap_TieSnapshot_Current(void) { return &fake.snapshot; }
bool __wrap_TieFlightPause_IsActive(void) { return fake.paused != 0; }

/* ---- helpers ---- */

static void Press(AeronXrButton button) { fake.queue[fake.queue_count++] = button; }

static void Frame(void) {
	fake.ticks += 16;
	(void)TieVrControls_Frame(false, false);
}

/* Drains the key queue into `out` (DOS getch values; extended keys as 0,
 * scan), returns the count. */
static int Keys(int* out, int capacity) {
	int n = 0;
	while (TieInput_KeyPending() && n < capacity)
		out[n++] = TieInput_ReadKey();
	while (TieInput_KeyPending())
		(void)TieInput_ReadKey();
	return n;
}

static int OneKey(void) {
	int keys[8];
	const int n = Keys(keys, 8);
	if (n == 1)
		return keys[0];
	if (n == 2 && keys[0] == 0)
		return 0x100 | keys[1]; /* extended: 0x100 | scan */
	return n == 0 ? -1 : -2;
}

/* ---- tests ---- */

static void TestTable(void) {
	size_t count = 0;
	const TieVrButtonBinding* table = TieVrButtons_Table(&count);
	CHECK(count == 2 * AERON_XR_BUTTON_COUNT);
	for (size_t i = 0; i < count; ++i) {
		CHECK(table[i].action > TIE_INPUT_ACTION_NONE && table[i].action < TIE_INPUT_ACTION_COUNT);
		CHECK(table[i].label && table[i].label[0]);
		/* Flight buttons are presses only: no hold (button-bit) actions. */
		CHECK(TieInputActions_ButtonBit(table[i].action) == 0);
		for (size_t j = 0; j < i; ++j) {
			CHECK(!(table[i].button == table[j].button && table[i].shifted == table[j].shifted));
			CHECK(table[i].action != table[j].action);
		}
		CHECK(TieVrButtons_Action(table[i].button, table[i].shifted) == table[i].action);
	}
	CHECK(TieVrButtons_Action(AERON_XR_BUTTON_A, false) == TIE_INPUT_ACTION_TARGET_NEXT);
	CHECK(TieVrButtons_Action(AERON_XR_BUTTON_RIGHT_GRIP, false) == TIE_INPUT_ACTION_AUTO_TARGET);
	CHECK(!strcmp(TieVrButtons_ActionControl(TIE_INPUT_ACTION_TARGET_PREV, false, false), "B"));
	CHECK(!strcmp(TieVrButtons_ActionControl(TIE_INPUT_ACTION_TARGET_PREV, true, true), "Y"));
	CHECK(!strcmp(TieVrButtons_ActionControl(TIE_INPUT_ACTION_CYCLE_SHIELD_RECHARGE_RATE, false, false),
				  "grip + A"));
	CHECK(!strcmp(TieVrButtons_ActionControl(TIE_INPUT_ACTION_FIRE_WEAPON, true, false), "R-trigger"));
	CHECK(TieVrButtons_ActionControl(TIE_INPUT_ACTION_EJECT, false, false) == NULL);
	CHECK(TieVrButtons_MenuKey(AERON_XR_BUTTON_B) == 27);
}

static void TestThrottleMath(void) {
	int worst = 0;
	for (uint32_t t = 0; t <= UINT16_MAX; ++t) {
		const float lever = TieVrControls_LeverForThrottle((uint16_t)t);
		/* What Aeron's virtual pad reports for that lever (xr_input_map.c). */
		const int16_t raw = (int16_t)lrintf(lever * 32767.0f);
		const uint16_t back =
			TieControllerMapping_ThrottlePosition(raw, AERON_CONTROLLER_KIND_GAMEPAD,
												  AERON_GAMEPAD_AXIS_RIGHT_TRIGGER, false);
		const int error = abs((int)back - (int)t);
		if (error > worst)
			worst = error;
		CHECK(lever >= 0.0f && lever <= 1.0f);
	}
	printf("throttle lever round trip: worst error %d / 65535\n", worst);
	CHECK(worst <= 3);
	/* Lever ends: full travel. */
	CHECK(TieControllerMapping_ThrottlePosition(0, AERON_CONTROLLER_KIND_GAMEPAD, AERON_GAMEPAD_AXIS_RIGHT_TRIGGER,
												false) == 0);
	CHECK(TieControllerMapping_ThrottlePosition(32767, AERON_CONTROLLER_KIND_GAMEPAD,
												AERON_GAMEPAD_AXIS_RIGHT_TRIGGER, false) == UINT16_MAX);
}

static AeronInputSnapshot g_input;

static AeronControllerSnapshot* XrPad(void) {
	AeronControllerSnapshot* pad = &g_input.controllers[AERON_CONTROLLER_MAX - 1];
	memset(&g_input, 0, sizeof g_input);
	g_input.has_focus = 1;
	pad->connected = 1;
	pad->kind = AERON_CONTROLLER_KIND_GAMEPAD;
	pad->instance_id = 0x58520001u;
	snprintf(pad->name, sizeof pad->name, "Meta Quest Touch (OpenXR)");
	snprintf(pad->path, sizeof pad->path, "openxr:touch");
	snprintf(pad->guid, sizeof pad->guid, "4f70656e5852546f7563680000000001");
	pad->gamepad_available_axes = (1u << AERON_GAMEPAD_AXIS_COUNT) - 1u;
	pad->gamepad_available_buttons = 0xFFFFu;
	return pad;
}

static void TestXrProfile(void) {
	char error[128] = "";
	const TieControllerProfile* profile = TieControllerMapping_XrProfile();
	CHECK(TieControllerMapping_Profilevalidate(profile, AERON_CONTROLLER_KIND_GAMEPAD, error, sizeof error));
	CHECK(profile->mapping.axes[TIE_INPUT_AXIS_THROTTLE].source == AERON_GAMEPAD_AXIS_RIGHT_TRIGGER);
	/* The lever is not a fire button on this pad. */
	for (size_t i = 0; i < profile->binding_count; ++i)
		CHECK(profile->bindings[i].source.kind == AERON_CONTROLLER_DIGITAL_BUTTON);

	TieControllerOptions empty = { 0 };
	CHECK(TieControllerMapping_SetOptions(&empty));

	/* Auto-profiling skips the XR pad, and drops one an earlier build added
	 * with the plain gamepad defaults. */
	AeronControllerSnapshot* pad = XrPad();
	TieControllerOptions options = { 0 };
	TieControllerProfile defaults;
	TieControllerMapping_ClearProfile(&defaults, AERON_CONTROLLER_KIND_GAMEPAD);
	CHECK(TieControllerMapping_InitializeGamepads(&options, &defaults, &g_input, error, sizeof error));
	CHECK(options.count == 0);
	CHECK(TieControllerMapping_AddModel(&options, pad, error, sizeof error));
	options.models[0].profile = defaults;
	CHECK(TieControllerMapping_InitializeGamepads(&options, &defaults, &g_input, error, sizeof error));
	CHECK(options.count == 0);
	/* A player's own XR profile stays; a cleared one goes. */
	CHECK(TieControllerMapping_AddModel(&options, pad, error, sizeof error));
	options.models[0].profile = *TieControllerMapping_XrProfile();
	options.models[0].profile.mapping.axes[TIE_INPUT_AXIS_ROLL].invert = true;
	CHECK(TieControllerMapping_InitializeGamepads(&options, &defaults, &g_input, error, sizeof error));
	CHECK(options.count == 1);
	TieControllerMapping_ClearProfile(&options.models[0].profile, AERON_CONTROLLER_KIND_GAMEPAD);
	CHECK(TieControllerMapping_InitializeGamepads(&options, &defaults, &g_input, error, sizeof error));
	CHECK(options.count == 0);
	/* Real gamepads still get the defaults. */
	g_input.controllers[0] = *pad;
	snprintf(g_input.controllers[0].path, sizeof g_input.controllers[0].path, "/dev/input/event7");
	snprintf(g_input.controllers[0].guid, sizeof g_input.controllers[0].guid, "030000005e0400008e02000014010000");
	g_input.controllers[0].instance_id = 7;
	CHECK(TieControllerMapping_InitializeGamepads(&options, &defaults, &g_input, error, sizeof error));
	CHECK(options.count == 1);
	memset(&g_input.controllers[0], 0, sizeof g_input.controllers[0]);

	/* Stick forward (negative LEFTY) is nose down (negative pitch). */
	TieControllerMapping_SetXrFlight(true);
	pad->gamepad_axes[AERON_GAMEPAD_AXIS_LEFTX] = 16384;
	pad->gamepad_axes[AERON_GAMEPAD_AXIS_LEFTY] = -32767;
	pad->gamepad_axes[AERON_GAMEPAD_AXIS_RIGHTX] = -32767;
	pad->gamepad_axes[AERON_GAMEPAD_AXIS_RIGHT_TRIGGER] = 16384;
	TieControllerMapping_Update(&g_input);
	int16_t axes[3];
	uint16_t buttons = 0;
	TieControllerMapping_Read(axes, 3, &buttons);
	const TieInputMapping* mapping = TieInput_Mapping();
	const int16_t yaw = TieInput_MapAxis(axes, 3, mapping->axes[TIE_INPUT_AXIS_YAW]);
	const int16_t pitch = TieInput_MapAxis(axes, 3, mapping->axes[TIE_INPUT_AXIS_PITCH]);
	const int16_t roll = TieInput_MapAxis(axes, 3, mapping->axes[TIE_INPUT_AXIS_ROLL]);
	printf("XR pad: yaw %d pitch %d roll %d\n", yaw, pitch, roll);
	{
		/* Linear (TIE_XR_STICK_CURVE=1): half deflection is half the axis. */
		SDL_setenv_unsafe("TIE_XR_STICK_CURVE", "1", 1);
		TieControllerMapping_Update(&g_input);
		int16_t lin[3];
		uint16_t lin_buttons = 0;
		TieControllerMapping_Read(lin, 3, &lin_buttons);
		const int16_t lin_yaw = TieInput_MapAxis(lin, 3, TieInput_Mapping()->axes[TIE_INPUT_AXIS_YAW]);
		printf("XR pad, linear: yaw %d\n", lin_yaw);
		CHECK(lin_yaw > 50 && lin_yaw < 70);
		SDL_unsetenv_unsafe("TIE_XR_STICK_CURVE");
	}
	/* Default response curve (TIE_XR_STICK_CURVE=2): half deflection turns
	 * gently, full deflection fully. */
	CHECK(yaw > 30 && yaw < 45);
	CHECK(pitch == -127);
	CHECK(roll == -127);
	CHECK(TieControllerMapping_Present());
	uint16_t throttle = 0;
	uint32_t generation = 0;
	CHECK(TieControllerMapping_ThrottleSample(&throttle, &generation));
	printf("XR pad: lever 0.5 -> throttle %u\n", throttle);
	CHECK(abs((int)throttle - 32768) < 200);

	/* Fire (right trigger = SOUTH) is the virtual button bit 0. */
	pad->gamepad_buttons = 1u << AERON_GAMEPAD_BUTTON_SOUTH;
	TieControllerMapping_Update(&g_input);
	CHECK(TieInputActions_VirtualButtons & 1u);
	pad->gamepad_buttons = 0;
	TieControllerMapping_Update(&g_input);
	CHECK(!(TieInputActions_VirtualButtons & 1u));

	/* Menus: the sticks do not steer (or move Landru's cursor). */
	TieControllerMapping_SetXrFlight(false);
	TieControllerMapping_Update(&g_input);
	TieControllerMapping_Read(axes, 3, &buttons);
	CHECK(axes[0] == 0 && axes[1] == 0 && axes[2] == 0);
	CHECK(TieControllerMapping_ThrottleSample(&throttle, &generation));
}

/* The Quest's Android lists its own Touch controllers as two gamepads
 * ("Device 0x..."), idle while the XR session owns them. In headset mode
 * they get no gamepad defaults (0.2.0 gave them, so the first owned
 * yaw/pitch/roll and the Touch sticks did not steer), saved untouched ones
 * are dropped, and the XR sticks win over an idle configured pad. */
static void Ghost(int slot, const char* name, const char* guid, uint32_t id) {
	AeronControllerSnapshot* d = &g_input.controllers[slot];
	memset(d, 0, sizeof *d);
	d->connected = 1;
	d->kind = AERON_CONTROLLER_KIND_GAMEPAD;
	d->instance_id = id;
	snprintf(d->name, sizeof d->name, "%s", name);
	snprintf(d->guid, sizeof d->guid, "%s", guid);
	d->gamepad_available_axes = (1u << AERON_GAMEPAD_AXIS_COUNT) - 1u;
	d->gamepad_available_buttons = 0x1FFFFu;
}

static void TestHeadsetGhostPads(void) {
	char error[128] = "";
	static const char* const g1 = "05000000d0145f14f8000000ce4af46a";
	static const char* const g2 = "050000002a9463e6cf000000ce4af46a";
	TieControllerProfile defaults;
	TieControllerMapping_ClearProfile(&defaults, AERON_CONTROLLER_KIND_GAMEPAD);
	defaults.mapping.axes[TIE_INPUT_AXIS_YAW] = (TieInputAxisBinding) { AERON_GAMEPAD_AXIS_LEFTX, false, 0.08f };
	defaults.mapping.axes[TIE_INPUT_AXIS_PITCH] = (TieInputAxisBinding) { AERON_GAMEPAD_AXIS_LEFTY, false, 0.08f };
	defaults.mapping.axes[TIE_INPUT_AXIS_ROLL] = (TieInputAxisBinding) { AERON_GAMEPAD_AXIS_RIGHTX, false, 0.08f };
	defaults.bindings[0] = (TieInputActionBinding) {
		.source = { .kind = AERON_CONTROLLER_DIGITAL_BUTTON, .index = AERON_GAMEPAD_BUTTON_SOUTH, .threshold = 0.5f },
		.action = TIE_INPUT_ACTION_FIRE_WEAPON,
	};
	defaults.binding_count = 1;

	AeronControllerSnapshot* pad = XrPad();
	Ghost(0, "Device 0x6AF4CED0145F14F8", g1, 2);
	Ghost(1, "Device 0x6AF4CECF2A9463E6", g2, 3);

	/* Desktop (no headset): both are auto-profiled, the first owns the sticks. */
	TieControllerMapping_SetHeadset(false);
	fake.presenting = 0;
	TieControllerOptions options = { 0 };
	CHECK(TieControllerMapping_InitializeGamepads(&options, &defaults, &g_input, error, sizeof error));
	CHECK(options.count == 2);
	CHECK(TieControllerMapping_IsAutoProfile(&options.models[0].profile, &defaults));
	CHECK(TieControllerMapping_IsAutoProfile(&options.models[1].profile, &defaults));
	/* Sorted by GUID: the second device (g2) is the first model and owns the axes. */
	const int owner = TieControllerMapping_FindModel(&options, g2);
	const int other = TieControllerMapping_FindModel(&options, g1);
	CHECK(owner == 0 && other == 1);
	CHECK(options.models[owner].profile.mapping.axes[TIE_INPUT_AXIS_YAW].source == AERON_GAMEPAD_AXIS_LEFTX);
	CHECK(options.models[other].profile.mapping.axes[TIE_INPUT_AXIS_YAW].source == -1);

	/* That was 0.2.0 on the Quest: the XR sticks did not steer. Keep the
	 * old rule here to show it (owned axes skipped when the pad is idle). */
	CHECK(TieControllerMapping_SetOptions(&options));
	TieControllerMapping_SetXrFlight(true);
	pad->gamepad_axes[AERON_GAMEPAD_AXIS_LEFTX] = 32767;
	pad->gamepad_axes[AERON_GAMEPAD_AXIS_LEFTY] = -32767;
	pad->gamepad_axes[AERON_GAMEPAD_AXIS_RIGHTX] = 32767;
	TieControllerMapping_Update(&g_input);
	int16_t axes[3];
	uint16_t buttons = 0;
	TieControllerMapping_Read(axes, 3, &buttons);
	const TieInputMapping* mapping = TieInput_Mapping();
	int16_t yaw = TieInput_MapAxis(axes, 3, mapping->axes[TIE_INPUT_AXIS_YAW]);
	int16_t pitch = TieInput_MapAxis(axes, 3, mapping->axes[TIE_INPUT_AXIS_PITCH]);
	int16_t roll = TieInput_MapAxis(axes, 3, mapping->axes[TIE_INPUT_AXIS_ROLL]);
	printf("XR pad beside idle auto-profiled pads: yaw %d pitch %d roll %d\n", yaw, pitch, roll);
	/* The XR sticks now win over the idle owner. */
	CHECK(yaw == 127 && pitch == -127 && roll == 127);
	TieControllerXrDebug debug;
	TieControllerMapping_XrDebug(&debug);
	CHECK(debug.slot == AERON_CONTROLLER_MAX - 1 && debug.flight && debug.owned_axes == 7u && debug.xr_axes == 7u);
	/* A pad that is used (deflected further) keeps its axis. */
	g_input.controllers[1].gamepad_axes[AERON_GAMEPAD_AXIS_LEFTX] = 32767;
	pad->gamepad_axes[AERON_GAMEPAD_AXIS_LEFTX] = -16384;
	TieControllerMapping_Update(&g_input);
	TieControllerMapping_Read(axes, 3, &buttons);
	yaw = TieInput_MapAxis(axes, 3, mapping->axes[TIE_INPUT_AXIS_YAW]);
	CHECK(yaw == 127);
	g_input.controllers[1].gamepad_axes[AERON_GAMEPAD_AXIS_LEFTX] = 0;

	/* Headset: saved untouched auto profiles are dropped, an edited one
	 * stays; nothing new is auto-added. */
	TieControllerMapping_SetHeadset(true);
	options.models[owner].profile.mapping.axes[TIE_INPUT_AXIS_ROLL].invert = true; /* the player edited it */
	CHECK(TieControllerMapping_InitializeGamepads(&options, &defaults, &g_input, error, sizeof error));
	CHECK(options.count == 1 && !strcmp(options.models[0].guid, g2));
	options.count = 0;
	CHECK(TieControllerMapping_InitializeGamepads(&options, &defaults, &g_input, error, sizeof error));
	CHECK(options.count == 0);
	/* A session presenting counts as headset too. */
	TieControllerMapping_SetHeadset(false);
	fake.presenting = 1;
	CHECK(TieControllerMapping_HeadsetMode());
	CHECK(TieControllerMapping_InitializeGamepads(&options, &defaults, &g_input, error, sizeof error));
	CHECK(options.count == 0);
	fake.presenting = 0;

	/* With no models the XR pad steers alone. */
	CHECK(TieControllerMapping_SetOptions(&options));
	pad->gamepad_axes[AERON_GAMEPAD_AXIS_LEFTX] = 32767;
	TieControllerMapping_Update(&g_input);
	TieControllerMapping_Read(axes, 3, &buttons);
	yaw = TieInput_MapAxis(axes, 3, mapping->axes[TIE_INPUT_AXIS_YAW]);
	pitch = TieInput_MapAxis(axes, 3, mapping->axes[TIE_INPUT_AXIS_PITCH]);
	CHECK(yaw == 127 && pitch == -127);
	TieControllerMapping_SetXrFlight(false);
}

static CraftData g_craft;

static void EnterFlight(void) {
	memset(&fake.snapshot, 0, sizeof fake.snapshot);
	fake.snapshot.scene_kind = TIE_SCENE_FLIGHT;
	fake.snapshot.flight_screen = TIE_FLIGHT_SCREEN_NORMAL;
	memset(&g_craft, 0, sizeof g_craft);
	pstate.player_craft = &g_craft;
	pstate.space_confirm_action = 0;
}

static void TestFlightFrame(void) {
	fake.presenting = 1;
	EnterFlight();
	TieInput_ClearKeys();
	Frame();
	CHECK(TieVrControls_InFlight());
	CHECK(fake.pointer == 0);
	CHECK(fake.flight_buttons == 1);
	CHECK(fake.menu_keys == 0);

	Press(AERON_XR_BUTTON_A);
	Frame();
	CHECK(OneKey() == 't');
	Press(AERON_XR_BUTTON_RIGHT_GRIP);
	Frame();
	CHECK(OneKey() == (0x100 | 0x78)); /* Alt+1 */
	Press(AERON_XR_BUTTON_RIGHT_STICK);
	Frame();
	CHECK(OneKey() == 13); /* match speed */

	/* Left grip held: shifted layer. */
	fake.grip[0] = 1.0f;
	Press(AERON_XR_BUTTON_A);
	Frame();
	CHECK(OneKey() == (0x100 | 0x44)); /* F10 */
	Press(AERON_XR_BUTTON_X);
	Frame();
	CHECK(OneKey() == '\'');
	fake.grip[0] = 0.0f;

	/* Left-handed: the shift grip is the physical right one. */
	fake.left_handed = 1;
	fake.grip[1] = 1.0f;
	Press(AERON_XR_BUTTON_B);
	Frame();
	CHECK(OneKey() == (0x100 | 0x43)); /* F9 */
	fake.grip[1] = 0.0f;
	fake.left_handed = 0;

	/* Left trigger with no chord on offer: at once. */
	fake.trigger[0] = 0.8f;
	Press(AERON_XR_BUTTON_LEFT_TRIGGER);
	Frame();
	CHECK(OneKey() == 'w');
	fake.trigger[0] = 0.0f;
	Frame();
	CHECK(OneKey() == -1);

	/* A prompt waits for Space: both triggers confirm it, the left trigger
	 * does not also change weapons. */
	pstate.space_confirm_action = 2;
	fake.trigger[1] = 0.9f;
	Frame();
	fake.trigger[0] = 0.9f;
	Press(AERON_XR_BUTTON_LEFT_TRIGGER);
	Frame();
	CHECK(OneKey() == ' ');
	fake.trigger[0] = fake.trigger[1] = 0.0f;
	Frame();
	Frame();
	CHECK(OneKey() == -1);

	/* Prompt on offer, left trigger alone: changes weapon on release. */
	fake.trigger[0] = 0.6f;
	Press(AERON_XR_BUTTON_LEFT_TRIGGER);
	Frame();
	CHECK(OneKey() == -1);
	fake.trigger[0] = 0.1f;
	Frame();
	CHECK(OneKey() == 'w');
	pstate.space_confirm_action = 0;

	/* Grip + both triggers: hyperspace. */
	fake.grip[0] = 1.0f;
	fake.trigger[0] = fake.trigger[1] = 0.9f;
	Frame();
	CHECK(OneKey() == 'h');
	fake.trigger[0] = fake.trigger[1] = 0.0f;
	fake.grip[0] = 0.0f;
	Frame();
	CHECK(OneKey() == -1);

	/* Firing far apart in time is no chord. */
	fake.trigger[1] = 0.9f;
	Frame();
	fake.ticks += 1000;
	fake.grip[0] = 1.0f;
	fake.trigger[0] = 0.9f;
	Frame();
	CHECK(OneKey() == -1);
	fake.trigger[0] = fake.trigger[1] = fake.grip[0] = 0.0f;
	Frame();
	TieInput_ClearKeys();
}

static void TestThrottleSync(void) {
	EnterFlight();
	fake.lever = 1.0f; /* left at full from the last mission */
	fake.lever_sets = 0;
	g_craft.throttle_speed = 0;
	Frame();
	CHECK(fake.lever_sets == 0);
	for (int i = 0; i < 50; ++i)
		Frame(); /* rests 800 ms */
	CHECK(fake.lever_sets == 1);
	CHECK(fake.lever == 0.0f);
	/* Keys set two thirds: the lever follows once it rests. */
	g_craft.throttle_speed = 43690;
	for (int i = 0; i < 50; ++i)
		Frame();
	CHECK(fake.lever_sets == 2);
	CHECK(fabsf(fake.lever - TieVrControls_LeverForThrottle(43690)) < 1e-6f);
	/* A lever the player is moving is never overridden. */
	g_craft.throttle_speed = 0;
	for (int i = 0; i < 50; ++i) {
		fake.lever = 0.3f + 0.001f * (float)i;
		Frame();
	}
	CHECK(fake.lever_sets == 2);
}

static void TestHaptics(void) {
	EnterFlight();
	fake.haptics[0] = fake.haptics[1] = 0;
	Frame();
	g_craft.laser_fired = 1;
	Frame();
	CHECK(fake.haptics[1] == 1 && fake.haptics[0] == 0);
	g_craft.forward_shield = -10;
	Frame();
	CHECK(fake.haptics[0] == 1 && fake.haptics[1] == 2);
}

static void TestMenus(void) {
	/* Briefing / frontend: pointer on, A = Enter, X = Space, B = Esc. */
	memset(&fake.snapshot, 0, sizeof fake.snapshot);
	fake.snapshot.scene_kind = TIE_SCENE_BRIEFING;
	TieInput_ClearKeys();
	Frame();
	CHECK(!TieVrControls_InFlight());
	CHECK(fake.pointer == 1);
	CHECK(fake.flight_buttons == 1);
	Press(AERON_XR_BUTTON_A);
	Frame();
	CHECK(OneKey() == 13);
	Press(AERON_XR_BUTTON_B);
	Frame();
	CHECK(OneKey() == 27);
	Press(AERON_XR_BUTTON_Y);
	Frame();
	CHECK(OneKey() == -1);

	/* In-flight map room: B leaves it (Escape straight to the game). */
	EnterFlight();
	fake.snapshot.flight_screen = TIE_FLIGHT_SCREEN_MAP;
	Press(AERON_XR_BUTTON_B);
	Frame();
	CHECK(fake.pointer == 1);
	CHECK(OneKey() == 27);

	/* The game's own pause: A resumes ('p'). */
	EnterFlight();
	fake.paused = 1;
	Press(AERON_XR_BUTTON_A);
	Frame();
	CHECK(OneKey() == 'p');
	fake.paused = 0;

	/* Settings menu open: no flight buttons, B closes it. */
	fake.b_latched = 1;
	fake.ticks += 16;
	CHECK(TieVrControls_Frame(true, false) & TIE_VR_REQUEST_CLOSE_SETTINGS);
	CHECK(fake.flight_buttons == 0);
	CHECK(fake.pointer == 1);

	/* Session gone: back to desktop defaults. */
	fake.presenting = 0;
	Frame();
	CHECK(fake.pointer == 1 && fake.flight_buttons == 0);
}

int main(void) {
	TestTable();
	TestThrottleMath();
	TestXrProfile();
	TestHeadsetGhostPads();
	TestFlightFrame();
	TestThrottleSync();
	TestHaptics();
	TestMenus();
	if (g_failures) {
		fprintf(stderr, "vr_controls_test: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("vr_controls_test: all passed\n");
	return 0;
}

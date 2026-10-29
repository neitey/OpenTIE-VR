#include "tie_runtime/input/controller_mapping.h"
#include "aeron/aeron.h"
#include "aeron/log.h"
#include "tie_runtime/input/keyboard_mapping.h"
#include <landru/joy.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Physical holds are independent even when several instances share a profile. */
typedef struct ControllerInstance {
	uint32_t id;
	int model;
	bool down[TIE_CONTROLLER_BINDING_CAP];
	bool armed[TIE_CONTROLLER_BINDING_CAP];
	bool dispatched[TIE_CONTROLLER_BINDING_CAP];
	bool primed;
} ControllerInstance;
static struct {
	TieControllerOptions options;
	ControllerInstance instances[AERON_CONTROLLER_MAX];
	uint32_t analog[TIE_CONTROLLER_MODEL_CAP];
	bool suspended, present, throttle_valid;
	int16_t axes[3];
	uint16_t throttle;
	uint32_t throttle_instance, generation;
	/* Aeron's OpenXR virtual gamepad (Touch controllers) when no configured
	 * model claims it: built-in profile, own hold state. */
	ControllerInstance xr;
	bool xr_flight;
	bool headset;
	TieControllerXrDebug xr_debug;
} g_controller;

/* Identity Aeron gives its virtual Touch gamepad (aeron/src/xr_input.c). */
#define TIE_XR_PAD_GUID "4f70656e5852546f7563680000000001"
#define TIE_XR_PAD_PATH "openxr:touch"

bool TieControllerMapping_IsXrPad(const AeronControllerSnapshot* device) {
	return device && device->connected && device->kind == AERON_CONTROLLER_KIND_GAMEPAD &&
		   (!strcmp(device->path, TIE_XR_PAD_PATH) || !strcmp(device->guid, TIE_XR_PAD_GUID));
}

const TieControllerProfile* TieControllerMapping_XrProfile(void) {
	/* Matches aeron/src/xr_input_map.h: right stick -> LEFTX/LEFTY (yaw,
	 * pitch), left stick X -> RIGHTX (roll), the integrated throttle lever
	 * (left stick Y) -> RIGHT_TRIGGER (absolute), right trigger -> SOUTH.
	 * Stick forward reports negative LEFTY like an SDL pad pushed up; the
	 * player-facing pitch invert makes that nose down, like a flight stick
	 * (AERON_XR_INVERT_PITCH=1 flips it in the engine). Swapped thumbsticks
	 * (XWA_XR_SWAP_STICKS) trade the two sticks in the engine before this
	 * mapping, as left-handed controls do, so the profile stays. Everything
	 * else on the Touch controllers is a flight button (vr_buttons.c). */
	static TieControllerProfile profile;
	static bool built;
	if (!built) {
		TieControllerMapping_ClearProfile(&profile, AERON_CONTROLLER_KIND_GAMEPAD);
		profile.mapping.axes[TIE_INPUT_AXIS_YAW] =
			(TieInputAxisBinding) { .source = AERON_GAMEPAD_AXIS_LEFTX, .invert = false, .deadzone = 0.10f };
		profile.mapping.axes[TIE_INPUT_AXIS_PITCH] =
			(TieInputAxisBinding) { .source = AERON_GAMEPAD_AXIS_LEFTY, .invert = true, .deadzone = 0.10f };
		profile.mapping.axes[TIE_INPUT_AXIS_ROLL] =
			(TieInputAxisBinding) { .source = AERON_GAMEPAD_AXIS_RIGHTX, .invert = false, .deadzone = 0.10f };
		profile.mapping.axes[TIE_INPUT_AXIS_THROTTLE] = (TieInputAxisBinding) {
			.source = AERON_GAMEPAD_AXIS_RIGHT_TRIGGER, .invert = false, .deadzone = 0.0f
		};
		profile.bindings[0] = (TieInputActionBinding) {
			.source = { .kind = AERON_CONTROLLER_DIGITAL_BUTTON, .index = AERON_GAMEPAD_BUTTON_SOUTH,
						.threshold = 0.5f },
			.action = TIE_INPUT_ACTION_FIRE_WEAPON,
		};
		profile.binding_count = 1;
		built = true;
	}
	return &profile;
}

void TieControllerMapping_SetXrFlight(bool flight) { g_controller.xr_flight = flight; }

void TieControllerMapping_SetHeadset(bool headset) { g_controller.headset = headset; }
bool TieControllerMapping_HeadsetMode(void) { return g_controller.headset || Aeron_XrIsPresenting(); }

void TieControllerMapping_XrDebug(TieControllerXrDebug* out) {
	if (out)
		*out = g_controller.xr_debug;
}

static bool TieControllerMapping_SameSource(const AeronControllerDigitalSource* left,
											const AeronControllerDigitalSource* right) {
	return left->kind == right->kind && left->index == right->index &&
		   (left->kind != AERON_CONTROLLER_DIGITAL_HAT || left->hat_direction == right->hat_direction);
}

static bool TieControllerMapping_ProfileEqual(const TieControllerProfile* left,
											  const TieControllerProfile* right) {
	if (left->binding_count != right->binding_count)
		return false;
	for (int axis = 0; axis < TIE_INPUT_AXIS_COUNT; ++axis) {
		const TieInputAxisBinding* a = &left->mapping.axes[axis];
		const TieInputAxisBinding* b = &right->mapping.axes[axis];
		if (a->source != b->source || a->invert != b->invert || a->deadzone != b->deadzone)
			return false;
	}
	for (size_t index = 0; index < left->binding_count; ++index) {
		const TieInputActionBinding* a = &left->bindings[index];
		const TieInputActionBinding* b = &right->bindings[index];
		if (a->action != b->action || !TieControllerMapping_SameSource(&a->source, &b->source) ||
			a->source.threshold != b->source.threshold)
			return false;
	}
	return true;
}

bool TieControllerMapping_IsAutoProfile(const TieControllerProfile* profile, const TieControllerProfile* defaults) {
	if (!profile || !defaults || profile->binding_count != defaults->binding_count)
		return false;
	for (int axis = 0; axis < TIE_INPUT_AXIS_COUNT; ++axis) {
		const TieInputAxisBinding* a = &profile->mapping.axes[axis];
		const TieInputAxisBinding* b = &defaults->mapping.axes[axis];
		/* InitializeGamepads leaves an axis an earlier model owns at -1. */
		if ((a->source != b->source && a->source != -1) || a->invert != b->invert || a->deadzone != b->deadzone)
			return false;
	}
	/* Bindings in any order (a saved model reloads in the config's order). */
	for (size_t index = 0; index < profile->binding_count; ++index) {
		const TieInputActionBinding* a = &profile->bindings[index];
		bool found = false;
		for (size_t j = 0; j < defaults->binding_count && !found; ++j) {
			const TieInputActionBinding* b = &defaults->bindings[j];
			found = a->action == b->action && TieControllerMapping_SameSource(&a->source, &b->source) &&
					a->source.threshold == b->source.threshold;
		}
		if (!found)
			return false;
	}
	return true;
}

bool TieControllerMapping_Optionsequal(const TieControllerOptions* left, const TieControllerOptions* right) {
	if (!left || !right)
		return left == right;
	if (left->count != right->count)
		return false;
	for (size_t i = 0; i < left->count; ++i) {
		const TieControllerModel *a = &left->models[i], *b = &right->models[i];
		if (strcmp(a->guid, b->guid) || strcmp(a->name, b->name) || a->kind != b->kind ||
			!TieControllerMapping_ProfileEqual(&a->profile, &b->profile))
			return false;
	}
	return true;
}

bool TieControllerMapping_EffectiveAxisInvert(AeronControllerKind kind, TieInputAxis axis, bool invert) {
	/* SDL gamepad Y is positive downward, while positive TIE pitch raises the nose. */
	return kind == AERON_CONTROLLER_KIND_GAMEPAD && axis == TIE_INPUT_AXIS_PITCH ? !invert : invert;
}

static bool TieControllerMapping_ValidationError(char* error, size_t capacity, const char* message) {
	if (error && capacity)
		snprintf(error, capacity, "%s", message);
	return false;
}

bool TieControllerMapping_Profilevalidate(const TieControllerProfile* profile, AeronControllerKind kind,
										  char* error, size_t capacity) {
	const int axis_limit =
		kind == AERON_CONTROLLER_KIND_GAMEPAD ? AERON_GAMEPAD_AXIS_COUNT : AERON_CONTROLLER_AXIS_MAX;
	const int button_limit =
		kind == AERON_CONTROLLER_KIND_GAMEPAD ? AERON_GAMEPAD_BUTTON_COUNT : AERON_CONTROLLER_BUTTON_MAX;
	if (!profile || (kind != AERON_CONTROLLER_KIND_GAMEPAD && kind != AERON_CONTROLLER_KIND_JOYSTICK))
		return TieControllerMapping_ValidationError(error, capacity, "invalid controller profile");
	if (profile->binding_count > TIE_CONTROLLER_BINDING_CAP)
		return TieControllerMapping_ValidationError(error, capacity, "controller binding capacity exceeded");
	for (int axis = 0; axis < TIE_INPUT_AXIS_COUNT; ++axis) {
		const TieInputAxisBinding* binding = &profile->mapping.axes[axis];
		if (binding->source < -1 || binding->source >= axis_limit || !isfinite(binding->deadzone) ||
			binding->deadzone < 0.0f || binding->deadzone > (axis == TIE_INPUT_AXIS_THROTTLE ? 0.10f : 1.0f))
			return TieControllerMapping_ValidationError(error, capacity,
														"controller axis binding is invalid");
		if (binding->source < 0)
			continue;
		for (int prior = 0; prior < axis; ++prior)
			if (profile->mapping.axes[prior].source == binding->source)
				return TieControllerMapping_ValidationError(
					error, capacity, "controller axis source is assigned more than once");
	}
	for (size_t index = 0; index < profile->binding_count; ++index) {
		const TieInputActionBinding* binding = &profile->bindings[index];
		const AeronControllerDigitalSource* source = &binding->source;
		if (binding->action <= TIE_INPUT_ACTION_NONE || binding->action >= TIE_INPUT_ACTION_COUNT)
			return TieControllerMapping_ValidationError(error, capacity,
														"controller action binding is invalid");
		if (!isfinite(source->threshold) || source->threshold <= 0.0f || source->threshold > 1.0f)
			return TieControllerMapping_ValidationError(error, capacity,
														"controller digital threshold is invalid");
		switch (source->kind) {
			case AERON_CONTROLLER_DIGITAL_BUTTON:
				if (source->index >= button_limit)
					return TieControllerMapping_ValidationError(error, capacity,
																"controller button index is out of range");
				break;
			case AERON_CONTROLLER_DIGITAL_AXIS_POSITIVE:
			case AERON_CONTROLLER_DIGITAL_AXIS_NEGATIVE:
				if (source->index >= axis_limit)
					return TieControllerMapping_ValidationError(error, capacity,
																"controller digital axis is invalid");
				break;
			case AERON_CONTROLLER_DIGITAL_HAT:
				if (kind != AERON_CONTROLLER_KIND_JOYSTICK || source->index >= AERON_CONTROLLER_HAT_MAX ||
					(source->hat_direction != AERON_CONTROLLER_HAT_UP &&
					 source->hat_direction != AERON_CONTROLLER_HAT_RIGHT &&
					 source->hat_direction != AERON_CONTROLLER_HAT_DOWN &&
					 source->hat_direction != AERON_CONTROLLER_HAT_LEFT))
					return TieControllerMapping_ValidationError(error, capacity,
																"controller hat binding is invalid");
				break;
			default:
				return TieControllerMapping_ValidationError(error, capacity,
															"controller source kind is invalid");
		}
		for (size_t prior = 0; prior < index; ++prior)
			if (TieControllerMapping_SameSource(source, &profile->bindings[prior].source))
				return TieControllerMapping_ValidationError(error, capacity,
															"controller source is bound more than once");
	}
	return true;
}

void TieControllerMapping_ClearProfile(TieControllerProfile* profile, AeronControllerKind kind) {
	memset(profile, 0, sizeof *profile);
	for (int i = 0; i < TIE_INPUT_AXIS_COUNT; ++i)
		profile->mapping.axes[i].source = -1;
	/* Raw HOTAS levers commonly decrease their reported value toward full power. */
	profile->mapping.axes[TIE_INPUT_AXIS_THROTTLE].invert = kind == AERON_CONTROLLER_KIND_JOYSTICK;
}

int TieControllerMapping_FindModel(const TieControllerOptions* options, const char* guid) {
	if (options && guid)
		for (size_t i = 0; i < options->count; ++i)
			if (!strcmp(options->models[i].guid, guid))
				return (int)i;
	return -1;
}

static bool GuidValid(const char* guid) {
	bool nonzero = false;
	for (int i = 0; i < 32; ++i) {
		if (!((guid[i] >= '0' && guid[i] <= '9') || (guid[i] >= 'a' && guid[i] <= 'f')))
			return false;
		nonzero |= guid[i] != '0';
	}
	return nonzero && guid[32] == 0;
}

bool TieControllerMapping_OptionsValid(const TieControllerOptions* options, char* error, size_t capacity) {
	if (!options || options->count > TIE_CONTROLLER_MODEL_CAP)
		return TieControllerMapping_ValidationError(error, capacity, "controller model capacity exceeded");
	for (size_t i = 0; i < options->count; ++i) {
		const TieControllerModel* m = &options->models[i];
		if (!GuidValid(m->guid) || !memchr(m->name, 0, sizeof m->name))
			return TieControllerMapping_ValidationError(error, capacity, "invalid controller model identity");
		if (!TieControllerMapping_Profilevalidate(&m->profile, m->kind, error, capacity))
			return false;
		for (size_t j = 0; j < i; ++j) {
			if (!strcmp(m->guid, options->models[j].guid))
				return TieControllerMapping_ValidationError(error, capacity,
															"duplicate controller model GUID");
			for (int axis = 0; axis < TIE_INPUT_AXIS_COUNT; ++axis)
				if (m->profile.mapping.axes[axis].source >= 0 &&
					options->models[j].profile.mapping.axes[axis].source >= 0)
					return TieControllerMapping_ValidationError(error, capacity,
																"logical axis has multiple model owners");
		}
	}
	return true;
}

bool TieControllerMapping_AddModel(TieControllerOptions* options, const AeronControllerSnapshot* device,
								   char* error, size_t capacity) {
	if (!device || !GuidValid(device->guid))
		return TieControllerMapping_ValidationError(error, capacity, "controller has no usable model GUID");
	if (TieControllerMapping_FindModel(options, device->guid) >= 0)
		return true;
	if (options->count >= TIE_CONTROLLER_MODEL_CAP)
		return TieControllerMapping_ValidationError(error, capacity, "controller model capacity exceeded");
	TieControllerModel* m = &options->models[options->count++];
	memset(m, 0, sizeof *m);
	memcpy(m->guid, device->guid, sizeof m->guid);
	snprintf(m->name, sizeof m->name, "%s", device->name);
	m->kind = device->kind;
	TieControllerMapping_ClearProfile(&m->profile, device->kind);
	return true;
}

bool TieControllerMapping_InitializeGamepads(TieControllerOptions* options,
											 const TieControllerProfile* defaults,
											 const AeronInputSnapshot* input, char* error, size_t capacity) {
	const AeronControllerSnapshot* sorted[AERON_CONTROLLER_MAX];
	int count = 0;
	if (!input)
		return true;
	/* The OpenXR virtual gamepad uses the built-in XR profile unless the
	 * player configured it. Earlier builds auto-added it with the ordinary
	 * gamepad defaults (whose right trigger fires: the throttle lever on
	 * this pad); drop such an untouched entry, and a cleared one ("Clear
	 * All" in the settings), which would leave the headset without
	 * controls. */
	for (size_t i = 0; i < options->count; ++i) {
		TieControllerModel* m = &options->models[i];
		TieControllerProfile cleared;
		TieControllerMapping_ClearProfile(&cleared, AERON_CONTROLLER_KIND_GAMEPAD);
		if (strcmp(m->guid, TIE_XR_PAD_GUID) || m->kind != AERON_CONTROLLER_KIND_GAMEPAD ||
			(!TieControllerMapping_ProfileEqual(&m->profile, defaults) &&
			 !TieControllerMapping_ProfileEqual(&m->profile, &cleared)))
			continue;
		memmove(m, m + 1, (options->count - i - 1) * sizeof *m);
		--options->count;
		break;
	}
	/* Headset: the Quest's Android lists its own Touch controllers as two
	 * gamepads ("Device 0x...") that report nothing while the XR session
	 * owns them. Given the gamepad defaults (0.2.0 did that and saved them)
	 * the first one owned yaw/pitch/roll, so the XR pad's sticks no longer
	 * steered (throttle and fire, not in the defaults' axes, still worked).
	 * Here no gamepad gets the defaults on its own, and untouched
	 * auto-configured ones are dropped; a real pad is added in Settings >
	 * Controllers. */
	if (TieControllerMapping_HeadsetMode()) {
		for (size_t i = 0; i < options->count;) {
			TieControllerModel* m = &options->models[i];
			if (m->kind != AERON_CONTROLLER_KIND_GAMEPAD || !strcmp(m->guid, TIE_XR_PAD_GUID) ||
				!TieControllerMapping_IsAutoProfile(&m->profile, defaults)) {
				++i;
				continue;
			}
			Aeron_LogInfo("tie.input",
						  "headset: dropped the auto-configured gamepad '%s' (the Touch controllers fly; "
						  "add a real gamepad in Settings > Controllers)",
						  m->name);
			memmove(m, m + 1, (options->count - i - 1) * sizeof *m);
			--options->count;
		}
		return true;
	}
	for (int slot = 0; slot < AERON_CONTROLLER_MAX; ++slot) {
		const AeronControllerSnapshot* d = &input->controllers[slot];
		if (!d->connected || d->kind != AERON_CONTROLLER_KIND_GAMEPAD || TieControllerMapping_IsXrPad(d))
			continue;
		int j = count++;
		while (j > 0 && strcmp(sorted[j - 1]->guid, d->guid) > 0) {
			sorted[j] = sorted[j - 1];
			--j;
		}
		sorted[j] = d;
	}
	for (int i = 0; i < count; ++i) {
		if (TieControllerMapping_FindModel(options, sorted[i]->guid) >= 0)
			continue;
		if (!TieControllerMapping_AddModel(options, sorted[i], error, capacity))
			return false;
		TieControllerProfile* p = &options->models[options->count - 1].profile;
		*p = *defaults;
		for (size_t j = 0; j + 1 < options->count; ++j)
			for (int axis = 0; axis < TIE_INPUT_AXIS_COUNT; ++axis)
				if (options->models[j].profile.mapping.axes[axis].source >= 0)
					p->mapping.axes[axis].source = -1;
	}
	return true;
}

const AeronControllerSnapshot* TieControllerMapping_Resolve(const TieControllerModel* model,
															const AeronInputSnapshot* input,
															uint32_t preferred) {
	const AeronControllerSnapshot* best = NULL;
	if (!input)
		return NULL;
	for (int i = 0; i < AERON_CONTROLLER_MAX; ++i) {
		const AeronControllerSnapshot* d = &input->controllers[i];
		if (!d->connected || strcmp(model->guid, d->guid) || d->kind != model->kind)
			continue;
		if (d->instance_id == preferred)
			return d;
		if (!best || d->instance_id < best->instance_id)
			best = d;
	}
	return best;
}

static void ReleaseState(ControllerInstance* state, const TieControllerProfile* p) {
	if (state->id)
		for (size_t i = 0; i < p->binding_count; ++i)
			if (state->dispatched[i])
				TieInputActions_DispatchController(p->bindings[i].action, false);
	memset(state, 0, sizeof *state);
}

static void ReleaseInstance(ControllerInstance* state) {
	ReleaseState(state, state->id ? &g_controller.options.models[state->model].profile : NULL);
}

bool TieControllerMapping_SetOptions(const TieControllerOptions* options) {
	static const TieControllerOptions empty = { 0 };
	if (!options)
		options = &empty;
	char error[128];
	if (!TieControllerMapping_OptionsValid(options, error, sizeof error)) {
		Aeron_LogWarn("tie.input", "%s", error);
		return false;
	}
	if (TieControllerMapping_Optionsequal(options, &g_controller.options))
		return true;
	uint32_t analog[TIE_CONTROLLER_MODEL_CAP] = { 0 };
	for (int i = 0; i < AERON_CONTROLLER_MAX; ++i) {
		ControllerInstance* state = &g_controller.instances[i];
		if (!state->id)
			continue;
		const TieControllerModel* old = &g_controller.options.models[state->model];
		int next = TieControllerMapping_FindModel(options, old->guid);
		if (next < 0 || old->kind != options->models[next].kind ||
			!TieControllerMapping_ProfileEqual(&old->profile, &options->models[next].profile))
			ReleaseInstance(state);
	}
	for (size_t i = 0; i < options->count; ++i) {
		int old = TieControllerMapping_FindModel(&g_controller.options, options->models[i].guid);
		if (old >= 0 && options->models[i].kind == g_controller.options.models[old].kind)
			analog[i] = g_controller.analog[old];
	}
	for (int i = 0; i < AERON_CONTROLLER_MAX; ++i) {
		ControllerInstance* state = &g_controller.instances[i];
		if (state->id)
			state->model =
				TieControllerMapping_FindModel(options, g_controller.options.models[state->model].guid);
	}
	/* Only changes to the effective throttle binding invalidate its movement baseline. */
	for (size_t i = 0; i < g_controller.options.count; ++i) {
		const TieControllerModel* old = &g_controller.options.models[i];
		const TieInputAxisBinding* a = &old->profile.mapping.axes[TIE_INPUT_AXIS_THROTTLE];
		if (a->source < 0)
			continue;
		int n = TieControllerMapping_FindModel(options, old->guid);
		if (n < 0 || old->kind != options->models[n].kind ||
			a->source != options->models[n].profile.mapping.axes[TIE_INPUT_AXIS_THROTTLE].source ||
			a->invert != options->models[n].profile.mapping.axes[TIE_INPUT_AXIS_THROTTLE].invert ||
			a->deadzone != options->models[n].profile.mapping.axes[TIE_INPUT_AXIS_THROTTLE].deadzone)
			TieInput_ResetThrottle();
	}
	g_controller.options = *options;
	memcpy(g_controller.analog, analog, sizeof analog);
	return true;
}

void TieControllerMapping_Suspend(void) {
	for (int i = 0; i < AERON_CONTROLLER_MAX; ++i)
		ReleaseInstance(&g_controller.instances[i]);
	ReleaseState(&g_controller.xr, TieControllerMapping_XrProfile());
	g_controller.suspended = true;
	g_controller.present = false;
	g_controller.throttle_valid = false;
	memset(g_controller.axes, 0, sizeof g_controller.axes);
	TieInput_ResetThrottle();
}
void TieControllerMapping_Resume(void) {
	g_controller.suspended = false;
	TieInput_ResetThrottle();
}

static void LogUnavailableControls(const TieControllerModel* model, const AeronControllerSnapshot* device) {
	int missing = 0;
	for (int axis = 0; axis < TIE_INPUT_AXIS_COUNT; ++axis) {
		int source = model->profile.mapping.axes[axis].source;
		if (source >= 0 && !Aeron_ControllerAxisAvailable(device, source))
			++missing;
	}
	for (size_t i = 0; i < model->profile.binding_count; ++i) {
		const AeronControllerDigitalSource* source = &model->profile.bindings[i].source;
		bool available = false;
		if (source->kind == AERON_CONTROLLER_DIGITAL_BUTTON)
			available = model->kind == AERON_CONTROLLER_KIND_GAMEPAD
							? (device->gamepad_available_buttons & (1u << source->index)) != 0
							: source->index < device->button_count;
		else if (source->kind == AERON_CONTROLLER_DIGITAL_HAT)
			available = source->index < device->hat_count;
		else
			available = Aeron_ControllerAxisAvailable(device, source->index);
		if (!available)
			++missing;
	}
	if (missing || device->controls_truncated)
		Aeron_LogWarn("tie.input", "controller '%s': %d unavailable configured controls%s", device->name,
					  missing, device->controls_truncated ? ", hardware controls truncated" : "");
}

static void SampleProfileDigital(ControllerInstance* state, const TieControllerProfile* profile,
								 const AeronControllerSnapshot* device) {
	for (size_t i = 0; i < profile->binding_count; ++i) {
		bool down = Aeron_ControllerDigitalSourceDown(device, &profile->bindings[i].source, state->down[i]);
		if (!state->primed)
			state->armed[i] = !down;
		else if (!down) {
			if (state->dispatched[i])
				TieInputActions_DispatchController(profile->bindings[i].action, false);
			state->dispatched[i] = false;
			state->armed[i] = true;
		} else if (state->armed[i] && !state->down[i]) {
			TieInputActions_DispatchController(profile->bindings[i].action, true);
			state->dispatched[i] = true;
		}
		state->down[i] = down;
	}
	state->primed = true;
}

static void SampleDigital(ControllerInstance* state, const AeronControllerSnapshot* device) {
	const TieControllerModel* model = &g_controller.options.models[state->model];
	if (!state->primed)
		LogUnavailableControls(model, device);
	SampleProfileDigital(state, &model->profile, device);
}

uint16_t TieControllerMapping_ThrottlePosition(int16_t raw, AeronControllerKind kind, int source,
											   bool invert) {
	const uint32_t end_tolerance = (uint32_t)ceilf(TIE_INPUT_THROTTLE_ENDPOINT_PERCENT * UINT16_MAX / 100.0f);
	const uint32_t range = UINT16_MAX - 2 * end_tolerance;
	uint32_t position = (int32_t)raw + 32768;
	if (kind == AERON_CONTROLLER_KIND_GAMEPAD && source >= AERON_GAMEPAD_AXIS_LEFT_TRIGGER)
		position = ((uint32_t)(raw < 0 ? 0 : raw) * UINT16_MAX + 16383u) / 32767u;
	if (invert)
		position = UINT16_MAX - position;
	/* Saturate both endpoints and scale the remaining travel continuously. */
	if (position <= end_tolerance)
		return 0;
	if (position >= UINT16_MAX - end_tolerance)
		return UINT16_MAX;
	return (uint16_t)(((position - end_tolerance) * UINT16_MAX + range / 2) / range);
}

static void SampleProfileAnalog(const TieControllerProfile* profile, AeronControllerKind kind,
								const AeronControllerSnapshot* d, TieInputMapping* mapping, unsigned axis_mask) {
	for (int axis = 0; axis < TIE_INPUT_AXIS_COUNT; ++axis) {
		TieInputAxisBinding b = profile->mapping.axes[axis];
		if (!(axis_mask & (1u << axis)) || !Aeron_ControllerAxisAvailable(d, b.source))
			continue;
		int16_t raw = Aeron_ControllerAxisValue(d, b.source);
		if (axis == TIE_INPUT_AXIS_THROTTLE) {
			g_controller.throttle = TieControllerMapping_ThrottlePosition(raw, kind, b.source, b.invert);
			g_controller.throttle_valid = true;
			if (g_controller.throttle_instance != d->instance_id) {
				g_controller.throttle_instance = d->instance_id;
				++g_controller.generation;
			}
		} else {
			int value = ((int32_t)raw * 127) / 32767;
			if (value < -127)
				value = -127;
			/* Landru also reads these slots for its menu cursor. Flight polarity
			 * belongs in TieInputMapping, which only feinput_getrawinput applies. */
			g_controller.axes[axis] = (int16_t)value;
			mapping->axes[axis] = (TieInputAxisBinding) {
				.source = (int8_t)axis,
				.invert = TieControllerMapping_EffectiveAxisInvert(kind, (TieInputAxis)axis, b.invert),
				.deadzone = b.deadzone,
			};
		}
	}
}

static void SampleAnalog(size_t index, const AeronControllerSnapshot* d, TieInputMapping* mapping) {
	const TieControllerModel* model = &g_controller.options.models[index];
	SampleProfileAnalog(&model->profile, model->kind, d, mapping, ~0u);
}

/* The OpenXR virtual gamepad without a configured model: the built-in XR
 * profile, on the logical axes no connected configured model drives. Its
 * sticks steer only in flight (in the menus the right controller is the
 * pointer, and Landru would also move its cursor with them). */
/* Response of the Touch thumbsticks in flight (TIE_XR_STICK_CURVE "Stick
 * Response", default 2.0 "Precise"; 1 = linear), as OpenXWA's
 * (XwaControllerMapping_XrStickRadial): small deflections turn gently for
 * fine aim, full deflection still gives the full turn rate.
 *
 * The steering thumbstick (yaw LEFTX / pitch LEFTY) is one stick with one
 * round 10 % dead zone; the last 5 % of the throw is full, and the round
 * gate is stretched onto the game's square one, so a diagonal turns as fast
 * in pitch and yaw as straight up or across. Roll (the other stick's X) is
 * its own axis with a 20 % dead zone (that stick's Y is the throttle lever).
 * The curve works on the full-resolution value and is handed to Landru with
 * its own dead zone (LANDRU_JOYSTICK_DEADZONE) added back and the binding's
 * gate off, so the game sees it continuously from the edge of the one dead
 * zone: before, Landru's 12, the binding's 13 and a curve on the truncated
 * value stacked into a cross-shaped dead zone of 27 % of the throw, a jump to
 * 12 % turn rate past it and 46 % at most on a full diagonal.
 *
 * An axis the grabbed flight stick drives (Aeron_XrSetVirtualStick: the
 * engine puts it on the same axes, whichever is pushed further) stays as it
 * was, linear. Desktop pads, joysticks and a configured XR model never come
 * here. */
enum { TIE_XR_STEER_DEADZONE_PCT = 10, TIE_XR_ROLL_DEADZONE_PCT = 20 };

static struct {
	bool held;
	float stick[3]; /* x, y, roll */
} g_xr_vstick;

void TieControllerMapping_SetXrVirtualStick(bool held, float x, float y, float roll) {
	g_xr_vstick.held = held;
	g_xr_vstick.stick[0] = held ? x : 0.0f;
	g_xr_vstick.stick[1] = held ? y : 0.0f;
	g_xr_vstick.stick[2] = held ? roll : 0.0f;
}

double TieControllerMapping_XrStickExponent(void) {
	const char* env = getenv("TIE_XR_STICK_CURVE");
	double exponent = env && env[0] ? atof(env) : 2.0;
	if (!(exponent >= 1.0))
		exponent = 1.0;
	return exponent > 4.0 ? 4.0 : exponent;
}

static double XrAxisUnit(int16_t value) { return value < 0 ? value / 32768.0 : value / 32767.0; }

void TieControllerMapping_XrStickRadial(int16_t x, int16_t y, double deadzone, double exponent, double out[2]) {
	static const double k_outer = 0.95;
	const double fx = XrAxisUnit(x), fy = XrAxisUnit(y);
	const double r = sqrt(fx * fx + fy * fy);
	const double major = fabs(fx) > fabs(fy) ? fabs(fx) : fabs(fy);
	out[0] = out[1] = 0.0;
	if (deadzone < 0.0)
		deadzone = 0.0;
	if (deadzone >= 0.9 || r <= deadzone || major <= 0.0)
		return;
	double t = (r - deadzone) / (k_outer - deadzone);
	t = t > 1.0 ? 1.0 : t;
	const double m = deadzone + (1.0 - deadzone) * (exponent > 1.0 ? pow(t, exponent) : t);
	/* Round gate onto square gate: the axis pushed further gets m, the other
	 * keeps the direction's ratio. */
	const double scale = m / major;
	out[0] = fx * scale;
	out[1] = fy * scale;
	for (int k = 0; k < 2; ++k)
		out[k] = out[k] > 1.0 ? 1.0 : out[k] < -1.0 ? -1.0 : out[k];
}

double TieControllerMapping_XrAxisCurve(int16_t value, double deadzone, double exponent) {
	static const double k_outer = 0.95;
	const double f = XrAxisUnit(value);
	const double a = fabs(f);
	if (a <= deadzone)
		return 0.0;
	double t = (a - deadzone) / (k_outer - deadzone);
	t = t > 1.0 ? 1.0 : t;
	const double m = deadzone + (1.0 - deadzone) * (exponent > 1.0 ? pow(t, exponent) : t);
	return f < 0.0 ? -m : m;
}

int16_t TieControllerMapping_XrLandruAxis(double unit) {
	/* Landru takes LANDRU_JOYSTICK_DEADZONE off what it reads: the game sees
	 * |unit| of its full 115. */
	const long game = lround(fabs(unit) * (LANDRU_JOYSTICK_AXIS_LIMIT - LANDRU_JOYSTICK_DEADZONE));
	if (game <= 0)
		return 0;
	long v = LANDRU_JOYSTICK_DEADZONE + game;
	if (v > LANDRU_JOYSTICK_AXIS_LIMIT)
		v = LANDRU_JOYSTICK_AXIS_LIMIT;
	return (int16_t)(unit < 0.0 ? -v : v);
}

static bool XrVirtualStickDrives(int axis, int16_t value) {
	const float stick = g_xr_vstick.stick[axis];
	return g_xr_vstick.held && stick != 0.0f && fabsf(stick) * 32767.0f + 64.0f >= (float)abs(value);
}

static void TieControllerMapping_XrSteer(const AeronControllerSnapshot* pad, TieInputMapping* mapping) {
	const double exponent = TieControllerMapping_XrStickExponent();
	const int16_t lx = pad->gamepad_axes[AERON_GAMEPAD_AXIS_LEFTX];
	const int16_t ly = pad->gamepad_axes[AERON_GAMEPAD_AXIS_LEFTY];
	const int16_t rx = pad->gamepad_axes[AERON_GAMEPAD_AXIS_RIGHTX];
	double steer[2];
	TieControllerMapping_XrStickRadial(lx, ly, TIE_XR_STEER_DEADZONE_PCT / 100.0, exponent, steer);
	const bool steer_driven = XrVirtualStickDrives(0, lx) || XrVirtualStickDrives(1, ly);
	const double unit[3] = { steer[0], steer[1],
							 TieControllerMapping_XrAxisCurve(rx, TIE_XR_ROLL_DEADZONE_PCT / 100.0, exponent) };
	const bool driven[3] = { steer_driven, steer_driven, XrVirtualStickDrives(2, rx) };
	for (int axis = 0; axis < 3; ++axis) {
		if (driven[axis])
			continue; /* the flight stick: linear, as sampled */
		g_controller.axes[axis] = TieControllerMapping_XrLandruAxis(unit[axis]);
		mapping->axes[axis].deadzone = 0.0f;
	}
}

static void SampleXrPad(const AeronInputSnapshot* input, unsigned owned_axes, TieInputMapping* mapping) {
	const AeronControllerSnapshot* pad = NULL;
	TieControllerXrDebug* debug = &g_controller.xr_debug;
	memset(debug, 0, sizeof *debug);
	debug->slot = -1;
	debug->flight = g_controller.xr_flight;
	debug->owned_axes = owned_axes;
	debug->models = g_controller.options.count;
	for (int i = 0; i < AERON_CONTROLLER_MAX && !pad; ++i)
		if (TieControllerMapping_IsXrPad(&input->controllers[i])) {
			debug->slot = i;
			debug->leftx = input->controllers[i].gamepad_axes[AERON_GAMEPAD_AXIS_LEFTX];
			debug->lefty = input->controllers[i].gamepad_axes[AERON_GAMEPAD_AXIS_LEFTY];
			debug->rightx = input->controllers[i].gamepad_axes[AERON_GAMEPAD_AXIS_RIGHTX];
			debug->lever = input->controllers[i].gamepad_axes[AERON_GAMEPAD_AXIS_RIGHT_TRIGGER];
			if (TieControllerMapping_FindModel(&g_controller.options, input->controllers[i].guid) < 0)
				pad = &input->controllers[i];
			else
				debug->claimed = true;
		}
	const TieControllerProfile* profile = TieControllerMapping_XrProfile();
	if (!pad || pad->instance_id != g_controller.xr.id)
		ReleaseState(&g_controller.xr, profile);
	if (!pad)
		return;
	g_controller.xr.id = pad->instance_id;
	g_controller.present = true;
	/* The throttle lever only where no configured model drives the
	 * throttle. In flight the sticks also on an axis a configured model
	 * owns, whichever is deflected further this frame, so a connected pad or
	 * HOTAS that is not being used never takes the Touch sticks away. */
	unsigned mask = (1u << TIE_INPUT_AXIS_THROTTLE) & ~owned_axes;
	const unsigned steer = (1u << TIE_INPUT_AXIS_YAW) | (1u << TIE_INPUT_AXIS_PITCH) | (1u << TIE_INPUT_AXIS_ROLL);
	int16_t owner_axes[3];
	TieInputAxisBinding owner_bindings[3];
	if (g_controller.xr_flight) {
		mask |= steer;
		memcpy(owner_axes, g_controller.axes, sizeof owner_axes);
		for (int axis = 0; axis < 3; ++axis)
			owner_bindings[axis] = mapping->axes[axis];
	}
	SampleProfileAnalog(profile, AERON_CONTROLLER_KIND_GAMEPAD, pad, mapping, mask);
	if (g_controller.xr_flight)
		TieControllerMapping_XrSteer(pad, mapping);
	if (g_controller.xr_flight)
		for (int axis = 0; axis < 3; ++axis) {
			if ((owned_axes & (1u << axis)) && abs(owner_axes[axis]) >= abs(g_controller.axes[axis])) {
				g_controller.axes[axis] = owner_axes[axis];
				mapping->axes[axis] = owner_bindings[axis];
			} else
				debug->xr_axes |= 1u << axis;
		}
	SampleProfileDigital(&g_controller.xr, profile, pad);
}

void TieControllerMapping_Update(const AeronInputSnapshot* input) {
	if (g_controller.suspended)
		return;
	if (!input || !input->has_focus) {
		TieControllerMapping_Suspend();
		g_controller.suspended = false;
		return;
	}
	g_controller.present = false;
	g_controller.throttle_valid = false;
	memset(g_controller.axes, 0, sizeof g_controller.axes);
	for (int i = 0; i < AERON_CONTROLLER_MAX; ++i) {
		ControllerInstance* state = &g_controller.instances[i];
		bool found = false;
		for (int j = 0; j < AERON_CONTROLLER_MAX && state->id; ++j) {
			const AeronControllerSnapshot* d = &input->controllers[j];
			const TieControllerModel* m = &g_controller.options.models[state->model];
			if (d->connected && d->instance_id == state->id && !strcmp(d->guid, m->guid) &&
				d->kind == m->kind)
				found = true;
		}
		if (!found)
			ReleaseInstance(state);
	}
	TieInputMapping mapping = { 0 };
	unsigned owned_axes = 0;
	for (int axis = 0; axis < TIE_INPUT_AXIS_COUNT; ++axis)
		mapping.axes[axis].source = -1;
	for (size_t model = 0; model < g_controller.options.count; ++model) {
		const AeronControllerSnapshot* analog = TieControllerMapping_Resolve(
			&g_controller.options.models[model], input, g_controller.analog[model]);
		g_controller.analog[model] = analog ? analog->instance_id : 0;
		if (analog) {
			g_controller.present = true;
			SampleAnalog(model, analog, &mapping);
			for (int axis = 0; axis < TIE_INPUT_AXIS_COUNT; ++axis)
				if (g_controller.options.models[model].profile.mapping.axes[axis].source >= 0)
					owned_axes |= 1u << axis;
		}
		uint32_t previous = 0;
		for (int n = 0; n < AERON_CONTROLLER_MAX; ++n) {
			const AeronControllerSnapshot* d = NULL;
			for (int j = 0; j < AERON_CONTROLLER_MAX; ++j) {
				const AeronControllerSnapshot* candidate = &input->controllers[j];
				if (candidate->connected && candidate->instance_id > previous &&
					!strcmp(candidate->guid, g_controller.options.models[model].guid) &&
					candidate->kind == g_controller.options.models[model].kind &&
					(!d || candidate->instance_id < d->instance_id))
					d = candidate;
			}
			if (!d)
				break;
			previous = d->instance_id;
			ControllerInstance* state = NULL;
			for (int j = 0; j < AERON_CONTROLLER_MAX; ++j)
				if (g_controller.instances[j].id == d->instance_id)
					state = &g_controller.instances[j];
			if (!state)
				for (int j = 0; j < AERON_CONTROLLER_MAX; ++j)
					if (!g_controller.instances[j].id) {
						state = &g_controller.instances[j];
						state->id = d->instance_id;
						state->model = (int)model;
						break;
					}
			if (state)
				SampleDigital(state, d);
		}
	}
	SampleXrPad(input, owned_axes, &mapping);
	memcpy(g_controller.xr_debug.axes, g_controller.axes, sizeof g_controller.xr_debug.axes);
	if (!g_controller.throttle_valid) {
		g_controller.throttle_instance = 0;
		TieInput_ResetThrottle();
	}
	TieInput_SetMapping(&mapping);
}

uint32_t TieControllerMapping_AnalogInstance(const char* guid) {
	int i = TieControllerMapping_FindModel(&g_controller.options, guid);
	return i < 0 ? 0 : g_controller.analog[i];
}
bool TieControllerMapping_ThrottleSample(uint16_t* position, uint32_t* generation) {
	*position = g_controller.throttle;
	*generation = g_controller.generation;
	return g_controller.throttle_valid;
}
int TieControllerMapping_Present(void) { return g_controller.present; }
void TieControllerMapping_Read(int16_t* axes, int count, uint16_t* buttons) {
	if (axes && count > 0) {
		memset(axes, 0, (size_t)count * sizeof *axes);
		for (int i = 0; i < count && i < 3; ++i)
			axes[i] = g_controller.axes[i];
	}
	if (buttons)
		*buttons = TieKeyboardMapping_ReadButtons();
}

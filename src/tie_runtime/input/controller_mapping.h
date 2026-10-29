#ifndef TIE_CONTROLLER_MAPPING_H
#define TIE_CONTROLLER_MAPPING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aeron/input.h"
#include "tie_runtime/input/actions.h"
#include "tie_runtime/input/input.h"

enum {
	TIE_CONTROLLER_BINDING_CAP =
		AERON_CONTROLLER_BUTTON_MAX + 2 * AERON_CONTROLLER_AXIS_MAX + 4 * AERON_CONTROLLER_HAT_MAX,
};

typedef struct TieInputActionBinding {
	AeronControllerDigitalSource source;
	TieInputAction action;
} TieInputActionBinding;

typedef struct TieControllerProfile {
	TieInputMapping mapping;
	TieInputActionBinding bindings[TIE_CONTROLLER_BINDING_CAP];
	size_t binding_count;
} TieControllerProfile;

enum { TIE_CONTROLLER_MODEL_CAP = 8 };
typedef struct TieControllerModel {
	char guid[33];
	char name[AERON_CONTROLLER_NAME_CAPACITY];
	AeronControllerKind kind; /* Saved SDL device type; must match the connected snapshot. */
	TieControllerProfile profile;
} TieControllerModel;
typedef struct TieControllerOptions {
	TieControllerModel models[TIE_CONTROLLER_MODEL_CAP];
	size_t count;
} TieControllerOptions;

void TieControllerMapping_ClearProfile(TieControllerProfile* profile, AeronControllerKind kind);
int TieControllerMapping_FindModel(const TieControllerOptions* options, const char* guid);
bool TieControllerMapping_OptionsValid(const TieControllerOptions* options, char* error, size_t capacity);
bool TieControllerMapping_AddModel(TieControllerOptions* options, const AeronControllerSnapshot* device,
								   char* error, size_t capacity);
bool TieControllerMapping_InitializeGamepads(TieControllerOptions* options,
											 const TieControllerProfile* defaults,
											 const AeronInputSnapshot* input, char* error, size_t capacity);
const AeronControllerSnapshot* TieControllerMapping_Resolve(const TieControllerModel* model,
															const AeronInputSnapshot* input,
															uint32_t preferred);
uint32_t TieControllerMapping_AnalogInstance(const char* guid);
uint16_t TieControllerMapping_ThrottlePosition(int16_t raw, AeronControllerKind kind, int source,
											   bool invert);
bool TieControllerMapping_ThrottleSample(uint16_t* position, uint32_t* generation);

bool TieControllerMapping_Optionsequal(const TieControllerOptions* left, const TieControllerOptions* right);
bool TieControllerMapping_Profilevalidate(const TieControllerProfile* profile, AeronControllerKind kind,
										  char* error, size_t error_capacity);
/* Converts the player-facing inversion preference to the raw-axis polarity used by TieInputMapping. */
bool TieControllerMapping_EffectiveAxisInvert(AeronControllerKind kind, TieInputAxis axis, bool invert);

/* Aeron's OpenXR virtual gamepad (Quest Touch controllers). Unless a
 * configured model claims its GUID it is driven by the built-in XR profile
 * (never auto-added to the options): yaw/pitch/roll/absolute throttle lever
 * and fire. */
bool TieControllerMapping_IsXrPad(const AeronControllerSnapshot* device);
const TieControllerProfile* TieControllerMapping_XrProfile(void);
/* Whether the XR pad's sticks steer (flight) this frame; set by vr_controls. */
void TieControllerMapping_SetXrFlight(bool flight);
/* The game's grabbed flight stick (as given to Aeron_XrSetVirtualStick): an
 * axis it drives is not given the thumbstick response. */
void TieControllerMapping_SetXrVirtualStick(bool held, float x, float y, float roll);
/* The thumbstick response in flight (controller_mapping.c), for tests:
 * TIE_XR_STICK_CURVE's exponent; the steering stick as one round stick
 * (-1..1 per axis, square gate); one axis; a -1..1 value in Landru's terms
 * (its dead zone added back). */
double TieControllerMapping_XrStickExponent(void);
void TieControllerMapping_XrStickRadial(int16_t x, int16_t y, double deadzone, double exponent, double out[2]);
double TieControllerMapping_XrAxisCurve(int16_t value, double deadzone, double exponent);
int16_t TieControllerMapping_XrLandruAxis(double unit);
/* Headset mode (VR requested at startup; a presenting session counts too).
 * The Quest's Android also lists its own Touch controllers as gamepads
 * ("Device 0x..."): in headset mode connected gamepads are not given the
 * gamepad defaults automatically (the player adds a real pad in Settings >
 * Controllers), saved untouched auto-configured gamepad models are dropped,
 * and the XR pad's sticks steer even on an axis another model owns when
 * they are deflected further. */
void TieControllerMapping_SetHeadset(bool headset);
bool TieControllerMapping_HeadsetMode(void);
/* True for a profile the defaults gave a connected gamepad and nobody edited
 * since (the defaults, possibly with axes left to an earlier model). */
bool TieControllerMapping_IsAutoProfile(const TieControllerProfile* profile, const TieControllerProfile* defaults);

/* Diagnostics: the XR pad as the last TieControllerMapping_Update saw it. */
typedef struct TieControllerXrDebug {
	int slot;            /* controller slot of the XR pad, -1 none */
	bool claimed;        /* a configured model owns the XR pad's GUID */
	bool flight;         /* sticks steer this frame */
	int16_t leftx, lefty, rightx, lever;
	unsigned owned_axes; /* logical axes configured models drive (bit = TieInputAxis) */
	unsigned xr_axes;    /* logical axes the XR pad drove */
	size_t models;       /* configured models */
	int16_t axes[3];     /* logical yaw/pitch/roll handed to Landru */
} TieControllerXrDebug;
void TieControllerMapping_XrDebug(TieControllerXrDebug* out);

bool TieControllerMapping_SetOptions(const TieControllerOptions* options);
void TieControllerMapping_Suspend(void);
void TieControllerMapping_Resume(void);
void TieControllerMapping_Update(const AeronInputSnapshot* input);
int TieControllerMapping_Present(void);
void TieControllerMapping_Read(int16_t* axes, int axis_count, uint16_t* buttons);

#endif

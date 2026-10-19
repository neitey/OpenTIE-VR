#include "tie_runtime/runtime/flight_requests.h"
#include "tie/feinput.h"
#include "tie/frontend_display_tie98.h"
#include "tie/msg.h"
#include "tie/msg_templates.h"
#include "tie/tie.h"
#include "tie/user.h"
#include "tie_runtime/audio/imuse_session.h"
#include "tie_runtime/display/classic_display.h"
#include "tie_runtime/input/input.h"
#include "tie/festring.h"
#include "tie/sys2.h"

#include "aeron/aeron.h"
#include "aeron/input.h"
#include "aeron/log.h"

#include <SDL3/SDL_stdinc.h>
#include <imuse/hilevel.h>
#include <imuse/lolevel.h>
#include <string.h>

static int32_t s_submodal_result;
static int32_t s_info_room_pending = -1;
static bool s_room_key_pending;
static int16_t s_room_key;
static bool s_replay_viewer_pending;
static bool s_pause_active;
static int16_t s_pause_saved_vol;
/* VR launch hold (XWA_HANGAR_PROMPT): armed when a mission flight loads,
 * entered after its first frame, left on any new press. */
static bool s_launch_armed;
static bool s_launch_hold;
static bool s_launch_fire_latch; /* the launching trigger is still held */
static bool TieFlightLaunch_ServiceHold(void);

void TieFlightRequest_SetSubmodalResult(int32_t result) { s_submodal_result = result; }

int32_t TieFlightRequest_SubmodalResult(void) { return s_submodal_result; }

void TieFlightRequest_InfoRoom(int32_t screen) { s_info_room_pending = screen; }

int32_t TieFlightRequest_ConsumeInfoRoom(void) {
	int32_t r = s_info_room_pending;
	s_info_room_pending = -1;
	return r;
}

void TieFlightRequest_SetRoomKey(int16_t key) {
	s_room_key = key;
	s_room_key_pending = true;
}

bool TieFlightRequest_ConsumeRoomKey(int16_t* key) {
	if (!s_room_key_pending)
		return false;
	s_room_key_pending = false;
	*key = s_room_key;
	return true;
}

void TieFlightRequest_ReplayViewer(void) { s_replay_viewer_pending = true; }

bool TieFlightRequest_ConsumeReplayViewer(void) {
	bool r = s_replay_viewer_pending;
	s_replay_viewer_pending = false;
	return r;
}

void TieFlightPause_Enter(void) {
	TieInput_ClearKeys();
	TieInput_ResetThrottle();
	s_pause_saved_vol = imuse_get_master_vol(im);
	imuse_set_master_vol(im, 0);
	imuse_pause(im);
	msg_messageprintf(MSG_PAUSED);
	if (TieClassicDisplay_UsesDx5()) {
		FrontendDisplay_BlitOffscreenToRenderSurface();
		FrontendDisplay_PresentFrame();
	}
	s_pause_active = true;
}

/* Drain commands received while paused; only the mapped Pause command
 * resumes. */
void TieFlightPause_Service(void) {
	if (s_launch_hold) {
		(void)TieFlightLaunch_ServiceHold();
		return;
	}
	while (TieInput_KeyPending()) {
		if (feinput_getrawinput() != KEY_p)
			continue;
		TieInput_ClearKeys();
		TieInput_ResetThrottle();
		if (TieClassicDisplay_UsesDx5())
			FrontendDisplay_PresentFrame();
		msg_messageprintf(MSG_RESUMED);
		calcframerate = 0;
		keypress = 0;
		imuse_set_master_vol(im, s_pause_saved_vol);
		imuse_resume(im);
		s_pause_active = false;
		break;
	}
}

bool TieFlightPause_IsActive(void) { return s_pause_active; }

/* ---- VR launch hold ---- */

/* Everything held now, per source; a press is anything held now that was
 * not at the last look, so a key or trigger still held from the briefing
 * never counts (OpenXWA-VR XwaFlightTask_SampleInput). */
typedef struct TieLaunchInput {
	uint32_t hands[2];
	uint8_t keys[AERON_KEY_COUNT];
	uint32_t mouse;
	uint32_t pads[AERON_CONTROLLER_MAX];
	uint64_t frame_id;
} TieLaunchInput;
static TieLaunchInput s_launch_input;

static void TieFlightLaunch_SampleInput(TieLaunchInput* out) {
	memset(out, 0, sizeof *out);
	for (int hand = 0; hand < 2; ++hand) {
		AeronXrHand h;
		if (Aeron_XrIsPresenting() && Aeron_XrGetHand(hand, &h))
			out->hands[hand] = (h.buttons & (AERON_XR_HAND_BUTTON_LOWER | AERON_XR_HAND_BUTTON_UPPER |
											 AERON_XR_HAND_BUTTON_STICK)) |
							   (h.trigger > 0.6f ? 0x100u : 0u);
	}
	const AeronInputSnapshot* input = Aeron_InputSnapshot();
	if (input) {
		out->frame_id = input->frame_id;
		memcpy(out->keys, input->key_down, sizeof out->keys);
		out->mouse = input->mouse.buttons;
		for (int c = 0; c < AERON_CONTROLLER_MAX; ++c)
			out->pads[c] = input->controllers[c].gamepad_buttons;
	}
}

static bool TieFlightLaunch_NewPress(const TieLaunchInput* now, const TieLaunchInput* before) {
	for (int hand = 0; hand < 2; ++hand)
		if (now->hands[hand] & ~before->hands[hand])
			return true;
	for (int k = 0; k < AERON_KEY_COUNT; ++k)
		if (now->keys[k] && !before->keys[k])
			return true;
	if (now->mouse & ~before->mouse)
		return true;
	for (int c = 0; c < AERON_CONTROLLER_MAX; ++c)
		if (now->pads[c] & ~before->pads[c])
			return true;
	return false;
}

bool TieFlightLaunch_Enabled(void) {
	const char* env = SDL_getenv("XWA_HANGAR_PROMPT");
	if (env && env[0])
		return env[0] != '0';
	return Aeron_XrIsPresenting() != 0;
}

void TieFlightLaunch_Arm(bool armed) { s_launch_armed = armed && TieFlightLaunch_Enabled(); }

bool TieFlightLaunch_TakeArmed(void) {
	const bool armed = s_launch_armed;
	s_launch_armed = false;
	return armed;
}

void TieFlightLaunch_EnterHold(void) {
	if (s_pause_active)
		return;
	s_launch_hold = true;
	TieFlightLaunch_SampleInput(&s_launch_input);
	TieFlightPause_Enter();
	Aeron_LogInfo("tie.flight", "launch hold: waiting for a press (XWA_HANGAR_PROMPT=0 skips)");
}

bool TieFlightLaunch_HoldActive(void) { return s_pause_active && s_launch_hold; }

/* Called by TieFlightPause_Service while the hold is on: any new key,
 * button, trigger or click launches. */
static bool TieFlightLaunch_ServiceHold(void) {
	TieLaunchInput now;
	TieFlightLaunch_SampleInput(&now);
	/* The sim (and so this) stood still for a while: the settings menu or
	 * the host pause was up. Keys still held from closing it (Esc, B) start
	 * a new baseline instead of launching. */
	const bool resumed = now.frame_id > s_launch_input.frame_id + 6;
	bool pressed = false;
	while (TieInput_KeyPending()) {
		(void)feinput_getrawinput();
		pressed = !resumed;
	}
	if (!resumed)
		pressed |= TieFlightLaunch_NewPress(&now, &s_launch_input);
	s_launch_input = now;
	if (!pressed)
		return false;
	/* The press that launched is not also a flight command. */
	AeronXrButton button;
	int shifted;
	while (Aeron_XrPollButton(&button, &shifted)) {
	}
	TieInput_ClearKeys();
	TieInput_ResetThrottle();
	if (TieClassicDisplay_UsesDx5())
		FrontendDisplay_PresentFrame();
	/* Let the launch prompt give way to whatever message waits behind it. */
	timers[TIMER_MSG] = 1;
	calcframerate = 0;
	keypress = 0;
	imuse_set_master_vol(im, s_pause_saved_vol);
	imuse_resume(im);
	s_pause_active = false;
	s_launch_hold = false;
	s_launch_fire_latch = true;
	Aeron_LogInfo("tie.flight", "launch hold: launched");
	return true;
}

void TieFlightLaunch_FilterButtons(uint16_t* buttons) {
	if (!s_launch_fire_latch || !buttons)
		return;
	if (*buttons == 0)
		s_launch_fire_latch = false;
	else
		*buttons = 0;
}

const char* TieFlightLaunch_Message(void) {
	if (!s_launch_hold)
		return NULL;
	return Aeron_XrIsPresenting() ? "\006Pull the trigger to launch" : "\006Press any key to launch";
}

void TieFlightLaunch_CenterBody(char* body, size_t size) {
	if (!s_launch_hold || !body || !body[0] || size < 2)
		return;
	/* Pad after the type byte so the prompt sits mid-bar (message font). */
	const uint8_t saved_font = fontflag;
	festring_setfontsize(1);
	const int text_w = sys2_calclength((const uint8_t*)body);
	const int space_w = sys2_calclength((const uint8_t*)" ");
	festring_setfontsize(saved_font);
	const int bar_w = (int)msgLineRight - 2;
	if (space_w <= 0 || text_w <= 0 || bar_w <= text_w)
		return;
	size_t len = strlen(body);
	size_t pad = (size_t)((bar_w - text_w) / 2 / space_w);
	const size_t limit = size < 70 ? size : 70; /* msg body cap incl. NUL */
	if (len + pad + 1 > limit)
		pad = limit > len + 1 ? limit - len - 1 : 0;
	if (!pad)
		return;
	memmove(body + 1 + pad, body + 1, len); /* keeps the NUL */
	memset(body + 1, ' ', pad);
}

#include "tie_app/vr_keyboard.h"

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "aeron/render.h"
#include "aeron/scene/ui.h"
#include "tie_runtime/input/input.h"

#include <SDL3/SDL_stdinc.h>
#include <stdlib.h>
#include <string.h>

/* Host frames the keyboard stays up after the last frame a text field was
 * live (the game marks live fields every frame it runs them). */
#define VRK_LINGER_FRAMES 20

typedef struct TieVrKeyboard {
	const AeronFontAtlas* font;
	AeronUiContext* theme_source; /* the settings menu's context */
	AeronUiContext* ui;
	bool init_failed;
	uint32_t last_serial;
	int linger;
	bool shift;
	bool visible;
	bool mouse_captured; /* a press started on the keyboard */
	bool frame_open;
	bool dismissed; /* closed by the player while this field is up */
	bool logged;
} TieVrKeyboard;

static TieVrKeyboard g_vrk;

static bool TieVrKeyboard_Enabled(void) {
	const char* env = getenv("TIE_VR_KEYBOARD");
	if (env && env[0] == '0')
		return false;
	if (env && env[0] == '1')
		return true;
	return Aeron_XrIsPresenting() != 0;
}

void TieVrKeyboard_Init(TieUi* ui) {
	g_vrk.font = ui ? &ui->font : NULL;
	g_vrk.theme_source = ui ? TieUi_Context(ui) : NULL;
}

static bool TieVrKeyboard_CreateUi(void) {
	if (g_vrk.ui)
		return true;
	if (g_vrk.init_failed)
		return false;
	if (!g_vrk.font || !g_vrk.font->loaded) {
		g_vrk.init_failed = true;
		Aeron_LogError("tie.vrkeyboard", "no UI font for the VR keyboard");
		return false;
	}
	g_vrk.ui = AeronUi_Create(NULL);
	if (!g_vrk.ui) {
		g_vrk.init_failed = true;
		Aeron_LogError("tie.vrkeyboard", "could not create the VR keyboard UI");
		return false;
	}
	AeronUi_SetFonts(g_vrk.ui, &(AeronUiFontSet) { .regular = g_vrk.font, .title = g_vrk.font });
	/* The settings menu's TIE colors, bigger keys. */
	AeronUiTheme theme =
		*(g_vrk.theme_source ? AeronUi_GetTheme(g_vrk.theme_source) : AeronUi_DefaultTheme());
	theme.text_px *= 1.4f;
	theme.title_px *= 1.1f;
	theme.row_height *= 1.3f;
	theme.column_spacing *= 0.6f;
	theme.open_fade_s = 0.0f;
	AeronUi_SetTheme(g_vrk.ui, &theme);
	g_vrk.shift = true;
	return true;
}

/* Into TIE's key queue, as the keyboard would (DOS getch values). */
static void TieVrKeyboard_Type(int ch) {
	TieInput_EnqueueDosKey((int16_t)ch);
	if (ch >= 'A' && ch <= 'Z')
		g_vrk.shift = false; /* one capital, then lower case (TIE upper-cases names anyway) */
}

/* One row of character keys, optionally followed by one wider key. */
static void TieVrKeyboard_Row(const char* keys, const char* extra_label, int extra_key) {
	const int n = (int)strlen(keys);
	float weights[16];
	for (int i = 0; i < n && i < 15; ++i)
		weights[i] = 1.0f;
	weights[n] = 2.0f;
	AeronUi_BeginColumns(g_vrk.ui, n + (extra_label ? 1 : 0), weights);
	for (int i = 0; i < n; ++i) {
		char label[2] = { keys[i], 0 };
		if (!g_vrk.shift && label[0] >= 'A' && label[0] <= 'Z')
			label[0] = (char)(label[0] - 'A' + 'a');
		if (i > 0)
			AeronUi_NextColumn(g_vrk.ui);
		AeronUi_PushId(g_vrk.ui, (int)(unsigned char)keys[i]);
		if (AeronUi_Button(g_vrk.ui, label))
			TieVrKeyboard_Type((unsigned char)label[0]);
		AeronUi_PopId(g_vrk.ui);
	}
	if (extra_label) {
		AeronUi_NextColumn(g_vrk.ui);
		if (AeronUi_Button(g_vrk.ui, extra_label))
			TieVrKeyboard_Type(extra_key);
	}
	AeronUi_EndColumns(g_vrk.ui);
}

void TieVrKeyboard_BeginFrame(const AeronInputSnapshot* in, int32_t delta_us, bool hidden) {
	g_vrk.frame_open = false;
	TieInput_SetPointerButtonsBlocked(false);
	const uint32_t serial = TieInput_TextEntrySerial();
	if (serial != g_vrk.last_serial) {
		g_vrk.last_serial = serial;
		g_vrk.linger = VRK_LINGER_FRAMES;
	} else if (g_vrk.linger > 0) {
		--g_vrk.linger;
	}
	const bool was_visible = g_vrk.visible;
	g_vrk.visible = in && !hidden && g_vrk.linger > 0 && TieVrKeyboard_Enabled();
	if (!g_vrk.visible) {
		g_vrk.mouse_captured = false;
		if (g_vrk.linger == 0)
			g_vrk.dismissed = false; /* the next text field opens it again */
		return;
	}
	if (!TieVrKeyboard_CreateUi()) {
		g_vrk.visible = false;
		return;
	}
	if (!was_visible) {
		g_vrk.shift = true; /* names start with a capital */
		if (!g_vrk.logged) {
			Aeron_LogInfo("tie.vrkeyboard", "VR keyboard shown for a text field");
			g_vrk.logged = true;
		}
	}

	/* The keyboard sees the mouse (laser pointer) only: the VR trigger is
	 * both a click and the pad's South button, and physical keys belong to
	 * the game's field. */
	static AeronInputSnapshot ui_input;
	ui_input = *in;
	memset(ui_input.controllers, 0, sizeof ui_input.controllers);
	memset(ui_input.key_down, 0, sizeof ui_input.key_down);
	memset(ui_input.key_pressed, 0, sizeof ui_input.key_pressed);
	memset(ui_input.key_released, 0, sizeof ui_input.key_released);
	memset(ui_input.key_typed, 0, sizeof ui_input.key_typed);
	memset(ui_input.key_alt_typed, 0, sizeof ui_input.key_alt_typed);
	ui_input.text_length = 0;
	ui_input.text[0] = 0;
	ui_input.key_event_count = 0;
	ui_input.key_events_overflow = 0;

	AeronUi_BeginFrame(g_vrk.ui, &(AeronUiFrameDesc) { .input = &ui_input, .dt_seconds = (float)delta_us * 1e-6f });
	/* The UI's reference space is 1080 px high and as wide as the content:
	 * 4:3 (1440) on the VR virtual screen, so fit the keyboard into that
	 * instead of assuming 1920. */
	float view_w_ref = 1920.0f;
	{
		AeronRectI rect;
		if (Aeron_GetContentPixelRect(&rect) && rect.height > 0)
			view_w_ref = 1080.0f * (float)rect.width / (float)rect.height;
	}
	const float width_ref = SDL_min(1300.0f, view_w_ref - 80.0f);
	const float x_ref = (view_w_ref - width_ref) * 0.5f;
	/* At the top: TIE's text fields sit low on their screens. */
	const float y_ref = 40.0f;
	if (g_vrk.dismissed) {
		/* Closed: a small button where it was brings it back. */
		if (AeronUi_BeginWindow(g_vrk.ui, "Keyboard (closed)",
								&(AeronUiWindowDesc) { .width_ref = 300.0f, .x_ref = x_ref, .y_ref = y_ref })) {
			if (AeronUi_Button(g_vrk.ui, "Keyboard"))
				g_vrk.dismissed = false;
			AeronUi_EndWindow(g_vrk.ui);
		}
	} else if (AeronUi_BeginWindow(g_vrk.ui, "Keyboard",
								   &(AeronUiWindowDesc) { .width_ref = width_ref, .x_ref = x_ref, .y_ref = y_ref })) {
		TieVrKeyboard_Row("1234567890", "Del", 8);
		TieVrKeyboard_Row("QWERTYUIOP", NULL, 0);
		TieVrKeyboard_Row("ASDFGHJKL", "Enter", 13);
		TieVrKeyboard_Row("ZXCVBNM-_", NULL, 0);
		AeronUi_BeginColumns(g_vrk.ui, 3, (const float[]) { 1.0f, 3.0f, 1.0f });
		if (AeronUi_Button(g_vrk.ui, g_vrk.shift ? "SHIFT" : "Shift"))
			g_vrk.shift = !g_vrk.shift;
		AeronUi_NextColumn(g_vrk.ui);
		if (AeronUi_Button(g_vrk.ui, "Space"))
			TieVrKeyboard_Type(' ');
		AeronUi_NextColumn(g_vrk.ui);
		/* Hides the keyboard only; the field stays as it is. */
		if (AeronUi_Button(g_vrk.ui, "Done"))
			g_vrk.dismissed = true;
		AeronUi_EndColumns(g_vrk.ui);
		AeronUi_EndWindow(g_vrk.ui);
	}
	/* The game's cursor is drawn underneath this overlay; show where the
	 * laser points on top of the keys. */
	AeronUi_DrawPointer(g_vrk.ui, 9.0f);
	const AeronUiOutput out = AeronUi_EndFrame(g_vrk.ui);
	g_vrk.frame_open = true;

	/* Keep the game from seeing clicks that belong to the keyboard, for the
	 * whole press (down through release): Landru would also click whatever
	 * lies under the keys (mouse button and the trigger's joystick button). */
	if (out.mouse_over && (in->mouse.pressed_buttons & AERON_MOUSE_BUTTON_LEFT))
		g_vrk.mouse_captured = true;
	if (g_vrk.mouse_captured || (out.mouse_over && (in->mouse.buttons & AERON_MOUSE_BUTTON_LEFT))) {
		Aeron_InputSuppressMouseButtons();
		TieInput_SetPointerButtonsBlocked(true);
		if (!(in->mouse.buttons & AERON_MOUSE_BUTTON_LEFT))
			g_vrk.mouse_captured = false;
	}
}

void TieVrKeyboard_Submit(void) {
	if (g_vrk.frame_open && g_vrk.ui)
		AeronUi_Submit(g_vrk.ui);
	g_vrk.frame_open = false;
}

bool TieVrKeyboard_Visible(void) { return g_vrk.visible; }

void TieVrKeyboard_Shutdown(void) {
	if (g_vrk.ui)
		AeronUi_Destroy(g_vrk.ui);
	memset(&g_vrk, 0, sizeof g_vrk);
}

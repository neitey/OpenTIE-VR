/* In-headset message screen (ported from OpenXWA-VR's setup_screen.c).
 * OS message boxes are invisible inside an immersive VR app, so the headset
 * build shows missing game files and fatal errors on the virtual screen,
 * clicked with the laser pointer. */

#include "tie_app/quest/error_screen.h"

#include <string.h>

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "aeron/render.h"
#include "aeron/scene/font_atlas.h"
#include "aeron/scene/ui.h"
#include "aeron/time.h"
#include "aeron/vfs.h"
#include "tie_app/test/test_harness.h"

#define TIE_ERROR_FONT_BASENAME "assets/fonts/menu"
#define TIE_ERROR_FONT_MAX_SIZE (4u * 1024u * 1024u)
#define TIE_ERROR_WINDOW_WIDTH 1300.0f
/* Larger than the desktop default: the VR virtual screen is ~2 m wide. */
#define TIE_ERROR_UI_SCALE 1.5f

static int TieErrorScreen_LoadFont(AeronFontAtlas* atlas) {
	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd)
		return 0;
	if (!AeronFontAtlas_LoadVfs(atlas, cmd, Aeron_GetVfs(), AERON_VFS_ROOT_RESOURCE, TIE_ERROR_FONT_BASENAME,
								TIE_ERROR_FONT_MAX_SIZE)) {
		Aeron_CancelCommandBuffer(cmd);
		return 0;
	}
	if (!Aeron_SubmitCommandBuffer(cmd)) {
		AeronFontAtlas_Release(atlas);
		return 0;
	}
	return 1;
}

int TieErrorScreen_Run(const char* title, const char* message, const char* retry_label, int during_fatal) {
	AeronFontAtlas atlas;
	memset(&atlas, 0, sizeof atlas);
	if (!TieErrorScreen_LoadFont(&atlas)) {
		Aeron_LogError("tie.quest", "could not load the font '%s' for the message screen", TIE_ERROR_FONT_BASENAME);
		return TIE_ERROR_SCREEN_FAILED;
	}
	AeronUiContext* ui = AeronUi_Create(NULL);
	if (!ui) {
		AeronFontAtlas_Release(&atlas);
		return TIE_ERROR_SCREEN_FAILED;
	}
	AeronUi_SetFonts(ui, &(AeronUiFontSet) { .regular = &atlas });
	AeronUiTheme theme = *AeronUi_GetTheme(ui);
	theme.text_px *= TIE_ERROR_UI_SCALE;
	theme.title_px *= TIE_ERROR_UI_SCALE;
	theme.help_px *= TIE_ERROR_UI_SCALE;
	theme.row_height *= TIE_ERROR_UI_SCALE;
	theme.title_height *= TIE_ERROR_UI_SCALE;
	theme.window_pad *= TIE_ERROR_UI_SCALE;
	theme.item_spacing *= TIE_ERROR_UI_SCALE;
	theme.column_spacing *= TIE_ERROR_UI_SCALE;
	AeronUi_SetTheme(ui, &theme);
	/* The laser pointer drives the mouse on the virtual screen. */
	Aeron_XrSetPointerEnabled(1);

	int result = -1;
	/* During a fatal error the engine has already asked to quit: the screen
	 * runs anyway (up to ten minutes) so the player can read why. */
	const uint64_t start_us = Aeron_NowUs();
	while (result < 0) {
		if (!during_fatal && (Aeron_QuitRequested() || Aeron_FatalErrorRequested()))
			break;
		if (during_fatal && Aeron_NowUs() - start_us > 600ull * 1000000ull)
			break;
		const int32_t delta_us = Aeron_BeginFrame();
		const AeronInputSnapshot* input = Aeron_InputSnapshot();
		if (TieTest_Active())
			TieTest_ApplyInput((AeronInputSnapshot*)input);
		AeronUi_BeginFrame(ui, &(AeronUiFrameDesc) {
								   .input = input,
								   .dt_seconds = (float)delta_us * 1e-6f,
							   });
		if (AeronUi_BeginWindow(ui, title, &(AeronUiWindowDesc) { .width_ref = TIE_ERROR_WINDOW_WIDTH, .centered = 1 })) {
			AeronUi_Help(ui, message);
			AeronUi_Spacer(ui, 16.0f);
			AeronUi_Separator(ui);
			if (retry_label) {
				AeronUi_BeginColumns(ui, 2, NULL);
				const int retry = AeronUi_Button(ui, retry_label);
				AeronUi_NextColumn(ui);
				const int quit = AeronUi_Button(ui, "Quit");
				AeronUi_EndColumns(ui);
				if (retry)
					result = TIE_ERROR_SCREEN_RETRY;
				else if (quit)
					result = TIE_ERROR_SCREEN_QUIT;
			} else if (AeronUi_Button(ui, "Quit")) {
				result = TIE_ERROR_SCREEN_QUIT;
			}
			AeronUi_EndWindow(ui);
		}
		/* Cancel (Esc / menu button) is ignored on purpose: a stray button
		 * would close the screen before it was read. */
		(void)AeronUi_EndFrame(ui);
		AeronUi_Submit(ui);
		if (TieTest_Active())
			TieTest_BeforePresent();
		if (!Aeron_Present())
			break;
		if (TieTest_Active() && TieTest_AfterPresent())
			break;
		Aeron_WaitForNextFrame(1000000u / 72u);
	}
	AeronUi_Destroy(ui);
	AeronFontAtlas_Release(&atlas);
	return result < 0 ? TIE_ERROR_SCREEN_QUIT : result;
}

void TieErrorScreen_FatalHandler(const char* title, const char* message) {
	(void)TieErrorScreen_Run(title, message, NULL, 1);
}

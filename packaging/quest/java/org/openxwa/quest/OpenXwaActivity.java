package org.openxwa.quest;

import org.libsdl.app.SDLActivity;

/** OpenXWA on Meta Quest: SDL's activity with the game library list. */
public class OpenXwaActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        // Dependencies (FFmpeg, zstd, the OpenXR loader) are resolved by the
        // dynamic linker from the APK's lib directory.
        return new String[] { "c++_shared", "SDL3", "main" };
    }
}

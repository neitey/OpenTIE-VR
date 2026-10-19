# OpenTIE-VR Launcher (PC)

Adapted from the OpenXWA-VR Launcher (its original plan follows below; read
"OpenXWA" as "OpenTIE-VR"). What is different for TIE Fighter:

- App org.opentie.quest (activity org.openxwa.quest.OpenXwaActivity), helper
  authority org.opentie.quest.setup (same SetupProvider class, so the zip
  names openxwa-import.zip / .openxwa-import.zip stay), APK
  OpenTIE-VR-<version>.apk, exe OpenTIE-VR-Launcher.exe, log
  opentie-vr-launcher.log, updates from neitey/OpenTIE-VR (asset
  opentie-vr-*.zip + SHA256SUMS.txt; packaging/quest/tools/make_release_tie.sh).
- Game: TIE Fighter Special Edition. Steam "STAR WARS Tie Fighter":
  remastered/ (1998 edition) -> files/TIE98, collectors_cd/ (Collector's CD,
  incl. its 600 MB CD image game.fil, without DOSBOX/) -> files/TIE95,
  classic/ (floppy) is not used. GOG/CD installs are recognised by content
  (game.go classifyEdition, the same rules as OpenTIE's installation.c).
- X-Wing Alliance (optional, step 3): only FlightModels/TIECOCKPIT.OPT,
  TIEBOMBERCOCKPIT.OPT, COMBATSIMCOCKPIT.OPT -> files/XWA/FlightModels
  (upper-case names). Without it the game uses TIE's 2D cockpits.
- All game files are listed relative to files/ ("TIE98/RESOURCE/X.LFD");
  the helper unpacks each folder's zips into it (TIE98, TIE95,
  XWA/FlightModels). A file bigger than a 128 MB batch gets a zip of its own.
- Install file lists: files/TIE98/opentie-install-files.txt (and TIE95,
  XWA/FlightModels), "<size>\t<path>" relative to that folder.
- Pilots: *.TFR at the top of the edition folders (and Windows VirtualStore);
  the 1998 one wins on a name clash; -> files/pilot-import (the app moves
  them in at start, keeping a pilot already on the headset).
- Logs: opentie.log, opentie-prev.log, opentie-crash.txt, opentie-quest.cfg.
- Env for tests: OPENTIE_ADB, OPENTIE_XWA (folder, or "none"),
  OPENTIE_UPDATE_API, OPENTIE_FAKE_PICKER, OPENTIE_NO_WINDOW.

---

# OpenXWA-VR Launcher (PC) - plan

Windows PC app (one portable .exe, no install) that puts OpenXWA and the
user's own X-Wing Alliance game files on a standalone headset over USB.
Quest, Pico and Steam Frame use the same steps. Mac/Linux later.
Implemented in this folder (Go, standard library only; build.sh): a window UI
(embedded page in an Edge/Chrome app window, web/index.html) or --console.
This file records the original design; see the code for the current flow
(ownership checks, Steam Frame via Lepton).

Since 1.0.0-rc6 the game files are not pushed one by one: on Quest
(Horizon OS 14) every file and folder adb makes in the app's files folder
belongs to the "shell" user and the app cannot read it. The APK has a
helper (SetupProvider, see appimport.go) that adb reaches without starting
the game. rc7: the launcher packs the files into zips of 128 MB, copies
each with adb push into the app's obb folder (where the app can read what
adb copies), and the app unpacks it into its files folder. If the app
cannot read the obb folder, the zips are streamed into the app instead
(exec-in content write; adb's input is kept open until every byte has
arrived, since closing it early crashed the adb server in rc6). A dropped
USB connection is waited for (90 s). The headset can stay on the desk.
Older apps without the helper fall back to pushing.

## Distribution

One zip per release on GitHub Releases (neitey/OpenXWA-Quest):

    OpenXWA-Quest-<version>.zip
      OpenXWA-VR-Launcher.exe
      OpenXWA-quest-<version>.apk
      README.txt
      THIRD-PARTY-NOTICES.txt
      LICENSE.txt               (GPL-3)

plus `SHA256SUMS.txt` as a separate release asset. The launcher installs
the APK next to it. No game files are ever included. Launcher source
lives in this repo (GPL-3). 1.0 ships unsigned (README explains the
SmartScreen warning); apply to SignPath Foundation for free signing,
which requires the .exe to be built by GitHub Actions from the public
repo. The APK keeps being signed locally; its key stays out of the repo.

## adb

1. Reuse an existing adb: PATH, SideQuest, Meta Quest Developer Hub,
   Android SDK platform-tools.
2. Otherwise download Google's platform-tools
   (https://dl.google.com/android/repository/platform-tools-latest-windows.zip)
   after showing the Android SDK License Agreement and getting the user's
   own "I agree". Unpack into the launcher's own folder. Never bundled
   (SDK licence section 3.4 forbids redistribution).

## Install flow

1. Find the game: Steam library folders, GOG registry entries, original
   CD registry entry; several found -> user picks; none -> "Browse...".
   Validate the folder (RESDATA.TXT and the data folders present).
2. Connect: wait for the headset, prompt "allow USB debugging" until
   authorised; read the device model (Quest / Pico / Steam Frame).
3. Install or update the APK (`adb install -r`). On a signature mismatch,
   stop and explain; never uninstall (that deletes the game files).
4. Let the app create its own folder (rc6+: through its helper).
5. Build the PC file list with sizes; skip files the port doesn't use
   (.exe, .dll).
6. Ask whether to bring existing PC pilot files over.
7. Create all folders on the headset first, in one shell call.
8. Push folder by folder with a progress bar.
9. Verify: list the headset folder with sizes, compare, re-push missing or
   wrong-size files individually, up to 3 tries; name any file that still
   fails.
10. Write a file list (manifest) on the headset; the app checks it at start
    and lists exactly which files are missing.
11. Done: button to start the game, or Library -> Unknown sources.

Running it again updates the app and copies only missing/changed files.
"Collect logs" pulls openxwa.log for bug reports.

## Update check

- At launcher start (and from a "Check for updates" button), request
  `https://api.github.com/repos/neitey/OpenXWA-Quest/releases/latest`.
  No account or token needed while the repo is public (GitHub allows 60
  unauthenticated requests per hour per IP; one per start is plenty).
- Compare the release tag (e.g. `v1.0.1`) with the launcher's own
  version. Release candidates (`-rcN`) are only offered when
  "Include test versions" is ticked.
- Newer version found: show version, date and the release notes, and ask
  "Download now?" [Download] [Later] [Skip this version].
- Download: the release zip into the user's Downloads folder with a
  progress bar; check its SHA-256 against `SHA256SUMS.txt` from the same
  release; extract to a new folder next to the zip.
- Then offer "Open the new version now". The new launcher continues with
  the normal install/update flow (app update + changed files only).
  The running launcher never replaces itself.
- No internet or GitHub unreachable: skip silently; the button reports
  the error. No telemetry: the only data sent is the request itself.
- Setting "Check for updates at start" (on by default).

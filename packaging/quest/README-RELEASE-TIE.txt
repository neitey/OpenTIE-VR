OpenTIE-VR @VERSION@ - TIE Fighter in standalone VR
======================================================

Play your own copy of Star Wars: TIE Fighter on Meta Quest 3 / 3S, with
3D cockpits. Pico and Steam Frame are supported but not tested yet.

You need:
- TIE Fighter Special Edition (Steam or GOG) installed on a Windows PC.
  OpenTIE-VR uses its 1998 edition and/or its Collector's CD (1995).
- Optional: X-Wing Alliance (Steam or GOG) on the same PC. OpenTIE-VR can
  then use its three TIE cockpit models (FlightModels\TIECOCKPIT.OPT,
  TIEBOMBERCOCKPIT.OPT and COMBATSIMCOCKPIT.OPT) instead. VR settings ->
  Cockpit chooses: X-Wing Alliance (the default when its files were
  copied), X-Wing Revival (the default otherwise), OpenTIE 3D (a simple
  pod of our own) or TIE Fighter's own 2D cockpits.
No game files are included in this download. 3D cockpits for every TIE
craft are included, from X-Wing Revival (credits under Licence below).


Before you start (once)
-----------------------
1. Turn on Developer Mode for the headset:
   Meta Horizon phone app -> Devices -> your headset -> Headset settings ->
   Developer Mode (needs a free Meta developer account).
2. Have a USB-C cable that carries data (not a charge-only cable). The
   Steam Frame can also be reached over Wi-Fi without a cable (see below).


Install (and update)
--------------------
1. Unzip this folder anywhere on the PC.
2. Connect the headset to the PC with the USB cable.
3. Double-click OpenTIE-VR-Launcher.exe and follow the steps in the window.
   It first asks how to reach the headset: "USB cable", "USB, then switch
   to Wi-Fi" or "Wi-Fi" (it remembers your choice).
   It finds TIE Fighter (and X-Wing Alliance, if installed), installs the
   app on the headset, copies the game files (about 1.1 GB, a few minutes),
   checks every file arrived and asks whether to bring your PC pilots
   (*.TFR) along.
   Put on the headset when it asks you to allow USB debugging (tick
   "Always allow from this computer").
4. Put on the headset: Library -> filter "Unknown sources" -> OpenTIE-VR.

After you have allowed USB debugging once, the headset can stay on the desk
(switched on, cable connected) while the launcher copies: the OpenTIE-VR
app receives and unpacks the files itself, without starting the game.

Unplugging during the copy (Quest, Pico): choose "USB, then switch to
Wi-Fi". After the USB connection works, the launcher switches the headset
to Wi-Fi (adb tcpip) and tells you when you can unplug. This lasts until
the headset sleeps or restarts, so keep it awake while the files are
copied; the PC and the headset must be on the same network. If the
connection drops, the launcher reconnects and continues; a new run only
copies what is missing.

On the headset the files go to Android/data/org.opentie.quest/files:
TIE98 (1998 edition), TIE95 (Collector's CD, including its CD image
game.fil) and XWA/FlightModels (the three cockpit models).

OpenTIE-VR is a separate app from OpenXWA-VR; both can be installed.

Music: TIE Fighter's adaptive iMUSE soundtrack (written for the Roland
SC-55) plays through FluidSynth with the bundled GeneralUser GS SoundFont.
Settings -> Audio -> Synthesizer switches back to the original OPL3 (FM)
sound; Music Source "CD Music" plays the 1998 edition's recorded (not
adaptive) soundtrack instead. To use your own General MIDI / GS SoundFont,
put the .sf2 file in a folder named "soundfonts" next to
OpenTIE-VR-Launcher.exe before running it: it goes to the headset's
"soundfonts" folder and is used instead of GeneralUser GS (first by name).
If you own a Roland SC-55 or SC-55mkII, put dumps of its ROMs in a folder
named "sc55" next to the launcher; Settings -> Audio -> Synthesizer ->
Roland SC-55 then emulates it (Nuked SC-55; heavier on the CPU, not tested
on a headset yet). No Roland ROMs are included.

Steam Frame: turn on Developer Mode (it asks you to set a password). Once,
before starting "Lepton Development": in your Steam Library, open its
Properties and set Launch Options to
   LEPTON_NO_CLEANUP=1 %command%
Without this, closing Lepton Development deletes OpenTIE-VR and the
copied game files; with it they should stay. Then start "Lepton
Development" and keep it running. In the launcher choose "Wi-Fi"
(recommended; the Frame's USB connection may not show up on Windows) and
enter "frame" or the headset's IP address (Quick Settings, or Steam
Settings > Internet). The PC and the Frame must be on the same network;
guest networks with "client isolation" do not work. If the direct
connection is refused, choose "Connect through SSH": the launcher asks for
the Developer Mode password (used only for this connection, never saved),
opens an SSH connection with Windows' own SSH client (user "steamos") and
reaches Lepton through it. Allow USB debugging in the Lepton window when
asked. If Windows' SSH client is missing: Settings > System > Optional
features > Add a feature > "OpenSSH Client". USB also works (the launcher
forwards Lepton's port over the cable). OpenTIE-VR appears in the Lepton
Development window's Android home screen. Not tested on a Steam Frame yet.

Windows may say "Windows protected your PC": the launcher is not
code-signed yet. Click "More info", then "Run anyway".

The launcher needs Google's adb tool. If it can't find one (SideQuest and
Meta Quest Developer Hub bring their own), it offers to download it from
Google; you accept Google's licence terms yourself.

Updating: download the new release and run its launcher. It updates the
app and copies only missing or changed files; your pilots and settings
stay. The launcher also tells you when a new version is out.

If the game was found somewhere unusual: drag the TIE Fighter folder onto
OpenTIE-VR-Launcher.exe, or paste its path when the launcher asks. On
Steam that is "STAR WARS Tie Fighter" (with the remastered and
collectors_cd folders inside).


In the headset
--------------
Menus: point with the right controller and pull the trigger to click.
B goes back. A pilot name is typed on the keyboard that appears on the
screen. The menu button opens the OpenTIE settings (with a VR page).

Flying: when a mission has loaded, it waits until you pull the trigger.
  Right stick ............ pitch and turn
  Left stick left/right .. roll
  Left stick fwd/back .... throttle (stays where you leave it)
  Right trigger .......... fire
  A / B .................. next / previous target
  X / Y .................. nearest fighter / your attacker
  Left trigger ........... weapon (lasers, ions, warheads)
  Left stick click ....... firing mode (single / linked)
  Right stick click ...... match speed
  Right grip ............. target in your sights
  Hold left grip + A / B . shield / laser recharge rate
  Hold left grip + X / Y . energy lasers -> shields / shields -> lasers
  Hold left grip + L trig. beam weapon on/off
  Both triggers .......... confirm (when the game asks, e.g. end mission)
  Hold left grip still ... radial menu: point, pull the left trigger
Settings -> VR: "Swap Thumbsticks" trades what the two sticks do (left
stick: pitch and turn; right stick: roll and throttle), every button stays
where it is; "Left-Handed Controls" mirrors both controllers, buttons too.
Look down: the lap console (energy, targeting, wingmen, ship), touch or
point and pull the trigger. On your right: the side panel (tactical map,
voice commands, button list). Look at a controller to see its labels.
Voice: double-tap a thumb rest, then speak (e.g. "next target", "full
throttle", "cover me").
The game pauses while the headset is off. Settings: menu button -> VR;
in a mission the settings also have a Briefing page (briefing and goals).


Problems?
---------
Choppy flight (below 72 fps): in opentie-quest.cfg (Android/data/
org.opentie.quest/files on the headset) try XWA_XR_FOVEATION=medium, or
XWA_XR_DIRECT_OUTPUT=1 (no bloom glow, noticeably less GPU work).

Run the launcher and choose "Collect logs" at the end (or start it with
--logs): it copies the game's log files (opentie.log, opentie-prev.log,
opentie-crash.txt) and its VR settings (opentie-quest.cfg) into a folder
next to the launcher. Please send those along with your report.

Uninstalling the app (Settings or adb uninstall) also deletes the copied
game files and your pilots on the headset.


Manual install without the launcher
-----------------------------------
Files copied with "adb push" belong to the wrong user on Quest, so the game
cannot read them. Let the app unpack a zip instead:
  adb install -r OpenTIE-VR-@VERSION@.apk
  Zip the CONTENTS of the TIE Fighter 1998 edition folder (on Steam:
  remastered) into tie98.zip (select everything inside the folder -> Send
  to -> Compressed (zipped) folder), so STRINGS.DAT sits at the top of the
  zip. Then:
  adb shell content call --uri content://org.opentie.quest.setup --method mkdirs --arg TIE98
  adb push tie98.zip /sdcard/Android/obb/org.opentie.quest/openxwa-import.zip
  adb shell content call --uri content://org.opentie.quest.setup --method unzip --arg TIE98
The last command prints the number of files unpacked; the app deletes the
zip afterwards. The same works for the Collector's CD (collectors_cd,
without its DOSBOX folder) with TIE95 instead of TIE98, and for the three
cockpit models with XWA/FlightModels.


Licence
-------
OpenTIE-VR is free software under the GNU GPL version 3 (LICENSE.txt).
Source: https://github.com/neitey/OpenTIE-VR
Third-party components and their licences: THIRD-PARTY-NOTICES.txt.
3D cockpits from X-Wing Revival (https://www.raptor007.com/x-wing-revival/)
by Raptor007 (Blair Sherman), included with his permission; the models are
originally by the X-Wing Alliance Upgrade team (https://xwaupgrade.com/).
Star Wars, TIE Fighter and X-Wing Alliance are trademarks of their owners;
this is an unofficial fan project and contains no game data.

OpenTIE-VR (PCVR) @VERSION@ - TIE Fighter in PC VR
====================================================

Play your own copy of Star Wars: TIE Fighter in VR on a Windows PC with a PC
VR headset: SteamVR headsets (Valve Index, HTC Vive, ...), Meta Quest
through Quest Link / Air Link (or Virtual Desktop / Steam Link), Windows
Mixed Reality and other OpenXR headsets. The same program also runs as the
normal flat-screen game.

OpenTIE-VR is based on OpenTIE (https://github.com/elyosh/OpenTIE), the
open-source reimplementation of TIE Fighter, and shares its VR code with
Neitey's XW: Alliance VR.

This is a FIRST PCVR build: it is the same VR mode as the Quest version, but
it has not been tested on a real PC headset yet. Please report what works and
what does not (see "Problems?" below).

No game files are included. You need your own TIE Fighter Special Edition
(Steam or GOG); OpenTIE-VR uses its 1998 edition and/or its Collector's CD
(1995). 3D cockpits for every TIE craft are included, from X-Wing Revival
(credits under Licence below).


Requirements
------------
- Windows 10 or 11, 64-bit, with a DirectX 12 capable GPU (any GeForce GTX
  900 / Radeon RX 400 or newer; VR wants a GTX 1070 / RX 5700 class GPU).
- An OpenXR runtime set as the ACTIVE one:
    SteamVR:        SteamVR -> Settings -> OpenXR -> "Set SteamVR as OpenXR
                    runtime" (on older versions: Settings -> Developer).
    Meta Quest Link: Meta Quest Link app (formerly Oculus app) -> Settings ->
                    General -> OpenXR Runtime -> "Set Meta Quest Link as
                    active".
    Windows Mixed Reality: the Mixed Reality Portal / OpenXR Tools for WMR.
  Only one runtime can be active; the game uses whichever one is.
- TIE Fighter Special Edition installed. On Steam its folder is
    C:\Program Files (x86)\Steam\steamapps\common\STAR WARS Tie Fighter
  with the 1998 edition in "remastered" and the Collector's CD in
  "collectors_cd".


Install
-------
1. Unzip this folder anywhere (not into the game folder; it never changes
   the original game).
2. Start OpenTIE.exe once on the monitor (no headset needed) and tell it
   where TIE Fighter is (the setup screen; it remembers it). Or give the
   folders on the command line (also works with "OpenTIE VR.bat"):
     OpenTIE.exe --tie98-data "C:\...\STAR WARS Tie Fighter\remastered"
                 --tie95-data "C:\...\STAR WARS Tie Fighter\collectors_cd"
   Windows may say "Windows protected your PC" (the program is not
   code-signed): "More info" -> "Run anyway".
3. Optional: X-Wing Alliance's three 3D TIE cockpits. Add a line with its
   FlightModels folder to the VR settings file (see below), e.g.
     TIE_XWA_COCKPITS=C:\GOG Games\Star Wars - X-Wing Alliance\FlightModels
   VR settings -> Cockpit then offers them (the default when they are
   there); otherwise X-Wing Revival's cockpits are the default.


Starting in VR
--------------
- Start your VR runtime (SteamVR, or connect the Quest with Link / Air Link)
  and put the headset on.
- Double-click "OpenTIE VR.bat" (it runs OpenTIE.exe --xr).
- Or run OpenTIE.exe --xr from a shortcut, or add the line AERON_XR=1 to
  %APPDATA%\TotallyOpen\OpenTIE\opentie-vr.cfg to always start in VR.
- Without --xr (plain double-click on OpenTIE.exe) it is the flat-screen game.
- If no OpenXR runtime or headset is found, the game says so in the log and
  starts on the monitor instead.
While in VR the desktop window shows a mirror of the game.


In the headset (Meta Quest Touch controllers; others below)
--------------
Menus: point with the right controller and pull the trigger to click.
B goes back. The menu button opens the OpenTIE settings (with a VR page);
during the intro films it skips straight to the TIE Fighter logo. X puts
the menu screen away, Y changes the scene around it.

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
Settings -> VR: Stick Response (Precise: small stick movements turn gently,
full deflection turns at full rate), Swap Thumbsticks, Left-Handed
Controls, Invert Thumbstick Pitch, Flight Stick (a stick right of the seat
to grab) and Invert Virtual Stick Pitch, Seat Position, Cockpit and more.
Look down: the lap console (energy, targeting, wingmen, ship). On your
right: the side panel (tactical map, voice commands, button list). Look at
a controller to see its labels.
Voice: double-tap a thumb rest, then speak (e.g. "next target", "full
throttle", "cover me").
Taking the headset off or opening the runtime's menu pauses the game.

Other controllers (bindings suggested by the game; SteamVR lets you rebind
them in its controller settings):
  Valve Index:  as Touch, left A/B = X/Y, game menu = press the LEFT
                trackpad, voice = double-tap a trackpad with the thumb.
  HTC Vive wands: trackpad = stick (press = stick click), grip buttons =
                grip, left menu button = game menu, right menu button = A.
                B / X / Y are not bound (use the keyboard or rebind).
  Windows Mixed Reality: thumbstick = stick, right trackpad press = A,
                right menu = B, left trackpad press = X, left menu = game
                menu. HP Reverb G2: as Touch.
  Keyboard, mouse, joysticks and gamepads work as in the flat-screen game.


Music
-----
TIE Fighter's adaptive iMUSE soundtrack plays through FluidSynth with the
included GeneralUser GS SoundFont (soundfonts folder). Settings -> Audio ->
Synthesizer switches to the original OPL3 (FM) sound; Music Source "CD
Music" plays the 1998 edition's recorded soundtrack. Your own .sf2 in the
soundfonts folder is used instead (first by name).


Settings and log files
----------------------
In %APPDATA%\TotallyOpen\OpenTIE (paste that into the Explorer address bar):
  opentie.log       the log of the last run (opentie-prev.log: the one before)
  opentie-vr.cfg    the VR settings (written by the VR settings page; a few
                    more are described in the file, e.g. SDL_GPU_DRIVER=vulkan
                    to use Vulkan instead of Direct3D 12)
  config.yaml       the game's settings; pilots are next to it


Voice commands
--------------
Speech recognition runs on the PC (no internet), with the model in the voice
folder and sherpa-onnx-c-api.dll / onnxruntime.dll. It uses the default
Windows microphone (with Quest Link: set the headset's microphone as the
default recording device). Turn it off in Settings -> VR -> Voice Commands.


Problems?
---------
Please send opentie.log (see above) with a short description: headset,
runtime (SteamVR / Quest Link / ...), GPU, what happened.


Licence
-------
OpenTIE and OpenTIE-VR are free software under the GNU GPL version 3
(LICENSE.txt). Third-party components and their licences:
THIRD-PARTY-NOTICES.txt and the licenses folder.
3D cockpits by Raptor007 (X-Wing Revival,
https://www.raptor007.com/x-wing-revival/) and the X-Wing Alliance Upgrade
team (https://xwaupgrade.com/).
Star Wars and TIE Fighter are trademarks of their owners; this is an
unofficial fan project and contains no game data.

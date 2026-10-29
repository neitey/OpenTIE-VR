# OpenTIE-VR

*Star Wars: TIE Fighter* in virtual reality, standalone on Meta Quest 3 / 3S
and Steam Frame (no PC needed while you play), flown from 3D TIE cockpits.
Pico is supported but not tested yet. A first PC VR build (Windows, OpenXR)
is available too.

OpenTIE-VR is a fork of [OpenTIE](https://github.com/elyosh/OpenTIE), the
open-source reimplementation of TIE Fighter, with the VR layer of
[Neitey's XW: Alliance VR](https://github.com/neitey/OpenXWA-Quest) (same
engine).

> [!IMPORTANT]
> No content from the original games is included. You need your own
> *TIE Fighter Special Edition* (Steam or GOG) on a Windows PC; OpenTIE-VR
> uses its 1998 edition and/or its Collector's CD (1995). 3D cockpits for
> every TIE craft come with the app (from X-Wing Revival, see Credits); with
> *X-Wing Alliance* (Steam, GOG or CD) on the PC, its own three TIE cockpit
> models are copied too and used instead (optional).

## Features

- 3D cockpits with TIE Fighter's instruments live on the cockpit's screens
  (CMD with the target picture, sensors, shields, throttle, clock). VR
  settings -> Cockpit: X-Wing Revival's (included), X-Wing Alliance's (when
  its files were copied; then the default), OpenTIE-VR's own simple pod, or
  TIE Fighter's original 2D cockpits
- Precise thumbstick steering (Stick Response), an optional flight stick to
  grab in the cockpit, Swap Thumbsticks, left-handed controls, inverted pitch
  for the thumbstick and the flight stick
- Lap touch console (energy, targeting, wingmen, ship), radial menu, side
  panel with a tactical map, voice commands and the button list, labels when
  you look at a controller
- Voice commands recognised on the headset (double-tap a thumb rest, then
  speak, e.g. "next target", "full throttle", "cover me")
- Menus, briefings and films on a screen inside a 3D scene built from your
  own TIE Fighter installation's ships: the TIE Fighter box art come to life
  (Vader's helmet), Defender of the Empire, Balance of Power, X-Wing, X-Wing
  vs. TIE Fighter, a random one of these, an Imperial fleet or just stars.
  X puts the screen away, Y changes the scene.
- The opening crawl as a 3D scene around you; the menu button skips the
  whole intro, on to the TIE Fighter logo
- TIE Fighter's adaptive iMUSE soundtrack through FluidSynth with the
  included GeneralUser GS SoundFont (or your own; the original OPL3 sound,
  the Roland SC-55 emulation with your own ROMs and the 1998 edition's
  recorded music are available too)
- Laser pointer and VR keyboard for the menus, a VR settings page, a launch
  prompt after loading, a briefing page in flight, pause when the headset is
  off

## Install

You need a headset in Developer Mode (Meta Quest: Meta Horizon phone app ->
Devices -> your headset -> Headset settings -> Developer Mode) and, for Quest
and Pico, a USB-C cable that carries data.

1. Download `OpenTIE-VR-<version>.zip` from
   [Releases](https://github.com/neitey/opentie-vr/releases/latest) and unzip
   it on your Windows PC.
2. Run `OpenTIE-VR-Launcher.exe`. It first asks how to reach the headset:
   "USB cable", "USB, then switch to Wi-Fi", "Wi-Fi", or "Steam Frame (into
   its Steam Library)".
3. The launcher finds TIE Fighter (and X-Wing Alliance, if installed),
   installs the app, copies the game files (about 1.1 GB) and checks them,
   and asks whether to bring your PC pilots along. Over USB, put the headset
   on once to allow USB debugging ("Always allow from this computer").
4. In the headset: Library -> "Unknown sources" -> OpenTIE-VR.

**Steam Frame:** turn on Developer Mode (Steam Settings > System; it asks
you to set a password) and choose "Steam Frame (into its Steam Library)" in
the launcher. It copies the game to the headset over Wi-Fi (SSH, with the
Developer Mode password; needs Windows' OpenSSH Client) and adds it to the
Steam Library, where it stays like any other game: no "Lepton Development"
needed, nothing is deleted when it closes. The older "Lepton Development"
way (USB or Wi-Fi) still works too.

Updating works the same way: run the new launcher; it updates the app and
copies only what changed. Your pilots and settings stay. To use your own
SoundFont or Roland SC-55 ROMs, put them in folders named `soundfonts` /
`sc55` next to the launcher before running it. `README.txt` in the zip has
the full details and the manual install steps.

Windows may warn that the launcher is not code-signed: "More info" -> "Run
anyway". If it cannot find Google's adb tool (SideQuest and Meta Quest
Developer Hub bring their own), it offers to download it from Google.

**PC VR (Windows):** unzip `OpenTIE-VR-PCVR-<version>-win64.zip`, start
`OpenTIE.exe` once on the monitor to point it at TIE Fighter, then start
`OpenTIE VR.bat` with your OpenXR runtime (SteamVR, Meta Quest Link, ...)
running. `README-PCVR.txt` in that zip has the details. Not tested on a PC
headset yet.

## Controls

| Control | Action |
|---|---|
| Right stick | Pitch and turn |
| Left stick left/right | Roll |
| Left stick forward/back | Throttle (stays where you leave it) |
| Right trigger | Fire |
| A / B | Next / previous target |
| X / Y | Nearest fighter / your attacker |
| Left trigger | Weapon (lasers, ions, warheads) |
| Left stick click | Firing mode (single / linked) |
| Right stick click | Match speed |
| Right grip | Target in your sights |
| Hold left grip + A / B | Shield / laser recharge rate |
| Hold left grip + X / Y | Energy lasers -> shields / shields -> lasers |
| Hold left grip + left trigger | Beam weapon on/off |
| Both triggers | Confirm (when the game asks, e.g. end mission) |
| Hold left grip still | Radial menu: point, pull the left trigger |
| Thumb rest (double-tap, then speak) | Voice command |
| Menu button | OpenTIE settings (with a VR page); skips the intro |

When a mission has loaded, it waits until you pull the trigger. In menus the
right controller is a laser pointer and B goes back.

## Problems

Run the launcher and choose **Collect logs** at the end (or start it with
`--logs`); it saves the game's log files and VR settings next to the
launcher. Please include them with a report, naming your headset.

## Building

The Android build uses the Android NDK with CMake (`packaging/quest`); the
PC launcher is a Go program in `tools/launcher` (`build.sh`); the PC VR
package is made by `packaging/pcvr/build.sh` (branch `pcvr`). The engine
submodule is the VR fork of aeron
([neitey/aeron-quest](https://github.com/neitey/aeron-quest), branch
`tie-vr`).

## Credits

- **OpenTIE:** elyosh and contributors
- **VR layer (from Neitey's XW: Alliance VR):** Neitey (AI assisted coding),
  MarkD (VR design, menu environments & QA)
- **3D cockpits:** [Raptor007](https://www.raptor007.com/x-wing-revival/)
  (X-Wing Revival) and the [X-Wing Alliance Upgrade team](https://xwaupgrade.com/).
  Not part of this repository or of its licence.
- **Music:** GeneralUser GS SoundFont by S. Christian Collins
- TIE Fighter: Totally Games / LucasArts

VR-specific code was generated using Claude AI. Third-party components and
their licences are listed in `packaging/quest/THIRD-PARTY-NOTICES-TIE.txt`.

## Licence

GPL-3.0, like OpenTIE (see [LICENSE](LICENSE)). Star Wars and TIE Fighter are
trademarks of their owners; this is an unofficial fan project and contains no
game data.

## About OpenTIE (upstream)

[![GitHub Release](https://img.shields.io/github/v/release/elyosh/OpenTIE)](https://github.com/elyosh/OpenTIE/releases/latest)
[![Visit our Discord server](https://img.shields.io/discord/1533001488391995442)](https://discord.gg/WBvYzczWfG)

![Screenshot of OpenTIE](assets/opentie-001.jpg)

OpenTIE is an open-source reimplementation of *Star Wars: TIE Fighter* for
Windows, macOS, and Linux. It runs the original game data natively on current
systems and supports both the 1995 Collector's CD-ROM and the 1998 Windows
release.

With both editions installed, OpenTIE can combine the 1995 menus, cutscenes,
and adaptive iMUSE soundtrack with the 1998 flight simulation and 3D assets.

> [!IMPORTANT]
> OpenTIE does not include any content from the original game. A complete
> installation of at least one supported edition is required. Install both
> editions to use the recommended combination.
>
> *Star Wars: TIE Fighter Special Edition* is available from
> [GOG](https://www.gog.com/en/game/star_wars_tie_fighter_special_edition) and
> [Steam](https://store.steampowered.com/app/355250/STAR_WARS_TIE_Fighter_Special_Edition/).

## The best of both editions

The 1995 and 1998 releases each have their own presentation, flight simulation,
and soundtrack. OpenTIE lets the menus and cutscenes, flight simulation, and
music be selected independently.

The recommended configuration combines:

- the 1995 menus and cutscenes
- the 1998 flight simulation and 3D assets
- the adaptive 1995 iMUSE soundtrack

This retains the 1998 flight presentation without giving up the interactive
music of the Collector's CD-ROM. Either supported edition can also be played
on its own.

## Adaptive music

OpenTIE reimplements the adaptive iMUSE soundtrack from the 1995 Collector's
CD-ROM. The music responds to mission events and moves seamlessly between
themes during flight.

OpenTIE can emulate Roland SC-55 Sound Canvas, the hardware for which the game’s
music was originally composed. This requires compatible SC-55 ROMs supplied by
the user.

Built-in Sound Blaster-compatible OPL3 emulation is available for the classic
FM-synth sound, while FluidSynth can play the soundtrack with a user-selected
SoundFont.

The original prerecorded TIE98 music remains available when the complete 1998
experience is preferred.

## Graphics

OpenTIE offers classic and modern graphics modes. Classic mode preserves the
original presentation, while modern mode combines the original models and
cockpit artwork with high-resolution rendering and substantially more advanced
lighting.

Modern graphics include:

- shadows, ambient occlusion, and bloom
- motion blur
- MSAA and AMD FidelityFX FSR 3.1.4 anti-aliasing and upscaling
- HDR output

Press `TAB` while the game is running to switch between classic and modern
graphics.

## Smoother flight

Flight simulation can update at up to 240 Hz to match modern high-refresh-rate
displays, providing smoother motion and more responsive controls. The original
TIE95 and TIE98 timing modes remain available when their exact behavior is
preferred.

Flight can be played with a mouse and keyboard, gamepad, or joystick. Mouse
flight offers both classic controls and a virtual stick, with adjustable
sensitivity and Y-axis inversion.

## Getting started

1. Download the latest package for your platform from
   [GitHub Releases](https://github.com/elyosh/OpenTIE/releases/latest).
2. Extract or install the package and launch OpenTIE.
3. Select the complete TIE95 and/or TIE98 installation folders when prompted.
4. If both editions are available, select the recommended configuration to
   combine the 1995 presentation and music with the 1998 flight experience.

OpenTIE validates the selected installations, remembers them for future
launches, and imports compatible existing pilot files during initial setup.

To access OpenTIE settings from the concourse, press `ESC` and select
**OpenTIE Settings**. During flight, press `ESC` to open them directly.

Useful shortcuts:

| Key | Action |
|---|---|
| `TAB` | Switch between classic and modern graphics |

## Supported platforms

| Platform | Target | Graphics backend |
|---|---|---|
| Windows | x86-64 | Direct3D 12 or Vulkan |
| macOS | macOS 13 or later; arm64 or x86-64 | Metal |
| Linux | x86-64; glibc 2.35 or later | Vulkan |

## Current state

OpenTIE remains under active development. Bugs and differences from the
original releases are still possible.

## OpenXWA

Fans of Totally Games’ space simulators may also be interested in
[OpenXWA](https://github.com/elyosh/OpenXWA), an open-source reimplementation
of *Star Wars: X-Wing Alliance* for Windows, macOS, and Linux.

## Community

Join the [TotallyOpen Discord server](https://discord.gg/WBvYzczWfG) to discuss
OpenTIE, OpenXWA, development, and the Totally Games flight simulators.

## System requirements

- a 64-bit system with a modern GPU
- a complete installation of the 1995 Collector's CD-ROM or the 1998 Windows
  release

Release packages include the required runtime libraries. Keep the executable,
libraries, resources, and shader directories together when moving an
installation.

## Building from source

The build requires CMake 3.23 or later, a C/C++ toolchain, SDL3 3.4, zstd,
FFmpeg, and SDL_shadercross. Release packaging pins its dependencies, including
SDL3 3.4.14, and provides the reference for reproducible builds.

Platform-specific instructions are available for
[Windows](packaging/windows/README.md),
[macOS](packaging/macos/README.md), and
[Linux](packaging/linux/README.md).

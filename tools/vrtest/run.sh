#!/bin/sh
# Headless OpenTIE run for tests (no display, no audio device, no headset).
#
#   tools/vrtest/run.sh <build-dir> <script|-> <capture-dir> [VAR=value ...]
#
#   build-dir    CMake build dir holding OpenTIE (Linux desktop build)
#   script       TIE_TEST_SCRIPT file (syntax: top of src/tie_app/test/test_harness.c;
#                examples in tools/vrtest/scripts), or - for none
#   capture-dir  PNG captures + run.log go here (created)
#   VAR=value    extra environment, e.g. TIE_TEST_XR=1 TIE_TEST_EXIT_FRAME=600
#                TIE_TEST_CAPTURE_FRAMES=every:100 TIE_TEST_XR_EYE=768x768
#
# Defaults (override on the command line or in the environment):
#   TIE_TEST_WINDOW=640x480     small window = fast SwiftShader frames
#   TIE_TEST_TIMEOUT=1800       wall-clock seconds before the run is killed
#   TIE98=".../STAR WARS Tie Fighter/remastered"
#   TIE_TEST_TIE95=1            also pass the TIE95 collectors CD folder
#   TIE_XWA_COCKPITS            XWA FlightModels folder (cockpit OPTs)
#   XWA_HANGAR_PROMPT=0         no VR launch hold before a mission ("wait flying" would
#                               never finish under TIE_TEST_XR=1); 1 tests the hold
#
# Every run gets a fresh HOME, so no pilot or config.yaml leaks between runs.
# Exit status: the game's (0 ok, 3 a script 'wait' timed out, 4 killed by a signal from
# outside, 124 wall timeout).
set -eu
if [ $# -lt 3 ]; then
	sed -n '2,24p' "$0"
	exit 2
fi
BUILD=$(cd "$1" && pwd)
SCRIPT=$2
OUT=$3
shift 3
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

GAME_ROOT="/mnt/user-data/uploads/STAR WARS Tie Fighter"
TIE98=${TIE98:-"$GAME_ROOT/remastered"}
TIE95=${TIE95:-"$GAME_ROOT/collectors_cd"}
XWA_MODELS="/mnt/user-data/uploads/Star Wars X-Wing Alliance/FlightModels"

RUN_HOME=$(mktemp -d "${TMPDIR:-/tmp}/opentie-test-home.XXXXXX")
trap 'rm -rf "$RUN_HOME"' EXIT INT TERM

set -- "$@"
for assignment in "$@"; do
	case "$assignment" in
		*=*) export "$assignment" ;;
		*) echo "run.sh: not VAR=value: $assignment" >&2; exit 2 ;;
	esac
done

export XWA_HANGAR_PROMPT=${XWA_HANGAR_PROMPT:-0}

if [ "${TIE_TEST_FAST:-1}" != 0 ]; then
	# Cheap flight rendering for SwiftShader: no MSAA/SSAO/shadows/motion blur.
	mkdir -p "$RUN_HOME/.local/share/TotallyOpen/OpenTIE"
	cat > "$RUN_HOME/.local/share/TotallyOpen/OpenTIE/config.yaml" <<'EOF'
version: 6
render:
  msaa_samples: 1
  ssao:
    quality: 0
  shadows:
    mode: "off"
  motion_blur:
    quality: 0
EOF
fi

export HOME="$RUN_HOME"
export XDG_DATA_HOME="$RUN_HOME/.local/share"
export XDG_CONFIG_HOME="$RUN_HOME/.config"
export LD_LIBRARY_PATH="/root/hostprefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SDL_VIDEO_DRIVER=offscreen
export SDL_AUDIO_DRIVER=dummy
export SDL_VULKAN_LIBRARY=${SDL_VULKAN_LIBRARY:-/root/ss/build/Linux/libvk_swiftshader.so}
export TIE_TEST_WINDOW=${TIE_TEST_WINDOW:-640x480}
export TIE_TEST_CAPTURE="$OUT"
if [ "$SCRIPT" != "-" ]; then
	export TIE_TEST_SCRIPT=$(cd "$(dirname "$SCRIPT")" && pwd)/$(basename "$SCRIPT")
fi
if [ -z "${TIE_XWA_COCKPITS:-}" ] && [ -d "$XWA_MODELS" ]; then
	export TIE_XWA_COCKPITS="$XWA_MODELS"
fi

set -- --tie98-data "$TIE98"
if [ "${TIE_TEST_TIE95:-0}" = 1 ]; then
	set -- "$@" --tie95-data "$TIE95"
fi

status=0
cd "$BUILD"
timeout -s TERM -k 30 "${TIE_TEST_TIMEOUT:-1800}" ${TIE_TEST_WRAPPER:-} ./OpenTIE "$@" > "$OUT/run.log" 2>&1 || status=$?
grep -E '^\[tie-test\]|FATAL|ERROR' "$OUT/run.log" | tail -n "${TIE_TEST_SUMMARY_LINES:-25}" || true
echo "run.sh: status $status, log $OUT/run.log, $(ls "$OUT" | grep -c '\.png$') PNG(s) in $OUT"
exit $status

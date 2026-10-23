#!/bin/bash
# package_tie_quest.sh CODE VERSION : build libmain.so for Android and repack it
# as the OpenTIE-VR APK (package org.opentie.quest), signed.
#
# The local Quest build repacks a known-good OpenXWA-VR APK (SDL's Java
# activity, SDL3, FFmpeg, OpenXR loader, sherpa-onnx) instead of running the
# Android SDK tools: the game library, shaders, resources and the manifest
# are replaced, XWA-only assets are dropped.
#
# Environment (defaults are the ones on the build machine):
#   BUILD    Android CMake build dir           (/root/build-tie-android)
#   BASE     OpenXWA-VR base APK               (/root/out/OpenXWA-quest-0.0.17-quest27.apk)
#   MANIFEST base binary AndroidManifest.xml    ($SP/q21/base/AndroidManifest.xml)
#   VKSHIM   libopenxwa_vkshim.so (shared)     (/root/build-android/libopenxwa_vkshim.so)
#   ASSETS   folder with icon/, vr_splash.png   ($SP/tieassets)
#   VOICE    speech model for voice commands    ($SP/vrassets/voice: encoder/decoder/joiner.onnx,
#            tokens.txt; never in the repo; skipped when absent)
#   SOUNDFONT bundled General MIDI/GS SoundFont for FluidSynth music
#            (/root/android/soundfonts/GeneralUser-GS.sf2, GeneralUser GS 2.0.3,
#            32 MB, freely redistributable; never in the repo; skipped when absent:
#            the app then defaults to OPL3 music). Needs a libmain.so built with
#            -DENABLE_FLUIDSYNTH=ON (and -DENABLE_NUKED_SC55=ON for the optional
#            SC-55 emulation): FluidSynth and SpeexDSP come from
#            /root/android/prefix-music, toolchain /root/android/toolchain-music.cmake.
#   FFMPEG   FFmpeg 8.0 shared libraries replacing the base APK's (which only
#            decode XWA's SMUSH films): adds the Ogg demuxer + Vorbis decoder
#            (TIE98's recorded flight music, MUSIC/Track0N.ogg) and WAV/PCM, same
#            ABI (/root/android/prefix-ffmpeg-ogg/lib; configure line in its
#            libavcodec.so; skipped when absent: then TIE98 music is silent)
#   APKTOOL  Apktool jar for sdl_clipboard_dex.py (/root/android/apktool_2.12.1.jar)
#   REVIVAL_DATA X-Wing Revival's TIE cockpits, staged out of a Revival download
#            ($SP/vrassets/revival_tie/data, made by
#            tools/revival_cockpits/gen_revival_cockpits.py stage <Revival zip> <dir>;
#            third-party models bundled with Raptor007's permission, never in the
#            repo). Converted here into REVIVAL_GLB ($SP/vrassets/revival_tie/glb)
#            with REVIVAL_COOK (a host aeron_gltf_cook,
#            $SP/vrassets/revival_tie/tools/aeron_gltf_cook) and packed as the
#            app resources cockpit/revival/*.glb. Skipped with a warning when
#            absent: the game then falls back to its own 3D cockpit.
#   KEY/CERT signing key and certificate (never in the repo)
set -e
CODE=$1
VER=${2:-0.1.$1}
HERE=$(cd "$(dirname "$0")" && pwd)
SP=${SP:-/tmp/claude-0/-home-claude/d5b75674-cc0b-5e76-88ee-fc1c5ab6c399/scratchpad}
BUILD=${BUILD:-/root/build-tie-android}
BASE=${BASE:-/root/out/OpenXWA-quest-0.0.17-quest27.apk}
MANIFEST=${MANIFEST:-$SP/q21/base/AndroidManifest.xml}
VKSHIM=${VKSHIM:-$(find /root/build-android -name libopenxwa_vkshim.so | head -1)}
ASSETS=${ASSETS:-$SP/tieassets}
VOICE=${VOICE:-$SP/vrassets/voice}
SOUNDFONT=${SOUNDFONT:-/root/android/soundfonts/GeneralUser-GS.sf2}
FFMPEG=${FFMPEG:-/root/android/prefix-ffmpeg-ogg/lib}
KEY=${KEY:-/root/android/signing/openxwa-quest-key.pem}
CERT=${CERT:-/root/android/signing/openxwa-quest-cert.der}
OUT=${OUT:-/root/out/OpenTIE-VR-$VER.apk}
WORK=${WORK:-$SP/tieapk/$CODE}
REVIVAL_DATA=${REVIVAL_DATA:-$SP/vrassets/revival_tie/data}
REVIVAL_GLB=${REVIVAL_GLB:-$SP/vrassets/revival_tie/glb}
REVIVAL_COOK=${REVIVAL_COOK:-$SP/vrassets/revival_tie/tools/aeron_gltf_cook}

cmake -DTIE_VERSION=$VER "$BUILD" > /dev/null
ninja -C "$BUILD" -j2 tie > "$WORK.build.log" 2>&1 || { mkdir -p "$(dirname "$WORK")"; tail -30 "$WORK.build.log"; exit 1; }

rm -rf "$WORK"; mkdir -p "$WORK/stage" /root/out/symbols
cp "$BUILD/libmain.so" /root/out/symbols/libmain-tie$CODE.so
cd "$WORK"
/usr/bin/llvm-strip -o stage/libmain.so "$BUILD/libmain.so"
/usr/bin/llvm-strip -o stage/libopenxwa_vkshim.so "$VKSHIM"

# Base without OpenXWA's own assets (shaders and resources are replaced
# wholesale; the XWA menu-scene pictures, logo and icon go).
python3 - "$BASE" base.apk <<'EOF'
import sys, zipfile
src = zipfile.ZipFile(sys.argv[1])
dst = zipfile.ZipFile(sys.argv[2], 'w')
drop = ('assets/shaders/', 'assets/resources/', 'assets/vr/', 'META-INF/')
for info in src.infolist():
    if info.filename.startswith(drop) or info.filename in ('assets/vr_splash.png', 'classes2.dex'):
        continue
    dst.writestr(info, src.read(info.filename))
dst.close()
EOF

# X-Wing Revival's TIE cockpits, converted from the staged data.
REVIVAL_FILES=()
if [ -d "$REVIVAL_DATA/Models" ] && [ -x "$REVIVAL_COOK" ]; then
  rm -rf "$REVIVAL_GLB"
  python3 "$HERE/../../../tools/revival_cockpits/gen_revival_cockpits.py" build "$REVIVAL_COOK" "$REVIVAL_DATA" "$REVIVAL_GLB"
  REVIVAL_FILES=("$REVIVAL_GLB"/*.glb)
else
  echo "warning: no X-Wing Revival cockpits ($REVIVAL_DATA, $REVIVAL_COOK): the 3D cockpit without X-Wing Alliance is OpenTIE-VR's own" >&2
fi

EXTRA=""
for f in "$BUILD"/shaders/*.spv; do EXTRA="$EXTRA assets/shaders/$(basename "$f")=$f"; done
# App resources, plus the list the app extracts them by (quest_app.c).
# First line: what makes a new APK re-extract them (version + content hash).
echo "# $VER $CODE $( (cd "$BUILD" && find resources -type f | sort | xargs sha256sum; \
  for f in "${REVIVAL_FILES[@]}"; do sha256sum "$f" | cut -c1-64; done) | sha256sum | cut -c1-16)" > stage/resources.lst
while IFS= read -r f; do
  rel=${f#$BUILD/}; EXTRA="$EXTRA assets/$rel=$f"; echo "$rel" >> stage/resources.lst
done < <(find "$BUILD/resources" -type f | sort)
for f in "${REVIVAL_FILES[@]}"; do
  rel=resources/cockpit/revival/$(basename "$f"); EXTRA="$EXTRA assets/$rel=$f"; echo "$rel" >> stage/resources.lst
done
EXTRA="$EXTRA assets/resources.lst=stage/resources.lst"

python3 "$HERE/manifest_version.py" "$MANIFEST" m1.xml "$CODE" "$VER"
python3 "$HERE/manifest_add_permission.py" m1.xml m2.xml android.permission.RECORD_AUDIO android.permission.INTERNET \
  android.permission.ACCESS_NETWORK_STATE com.oculus.permission.RENDER_MODEL
python3 "$HERE/manifest_add_feature.py" m2.xml m3.xml com.oculus.feature.RENDER_MODEL
python3 "$HERE/manifest_add_metadata.py" m3.xml m4.xml com.oculus.ossplash=true pvr.app.type=vr com.yvr.intent.category.VR=vr_only
# Launcher helper (SetupProvider): its own authority, so OpenXWA-VR can stay installed.
python3 "$HERE/manifest_add_provider.py" m4.xml m5.xml org.openxwa.quest.SetupProvider org.opentie.quest.setup
python3 "$HERE/manifest_rename.py" m5.xml AndroidManifest.xml org.opentie.quest "OpenTIE-VR"
python3 "$HERE/dexgen_setup_provider.py" classes2.dex
EXTRA="$EXTRA classes2.dex=classes2.dex"
# Steam Frame: SDL's clipboard handler made safe for a missing clipboard
# service (needs Java and Apktool, see sdl_clipboard_dex.py).
python3 "$HERE/sdl_clipboard_dex.py" "$BASE" classes.dex
EXTRA="$EXTRA classes.dex=classes.dex"

cp "$HERE/../THIRD-PARTY-NOTICES-TIE.txt" stage/THIRD-PARTY-NOTICES.txt
EXTRA="$EXTRA assets/THIRD-PARTY-NOTICES.txt=stage/THIRD-PARTY-NOTICES.txt"
if [ -f "$ASSETS/vr_splash.png" ]; then EXTRA="$EXTRA assets/vr_splash.png=$ASSETS/vr_splash.png"; fi
if [ -d "$ASSETS/icon" ]; then
  for d in mdpi hdpi xhdpi xxhdpi xxxhdpi; do EXTRA="$EXTRA res/mipmap-$d/ic_launcher.png=$ASSETS/icon/ic_launcher_$d.png"; done
fi
# Voice commands: the speech model (vr_voice.c copies it out on first use).
if [ -f "$VOICE/encoder.onnx" ]; then
  for f in encoder.onnx decoder.onnx joiner.onnx tokens.txt; do EXTRA="$EXTRA assets/voice/$f=$VOICE/$f"; done
else
  echo "warning: no speech model in $VOICE: voice commands will be off" >&2
fi
# Music: the SoundFont FluidSynth plays iMUSE with (quest_app.c copies it out;
# its name is fixed there).
if [ -f "$SOUNDFONT" ] && grep -q "HAVE_FLUIDSYNTH=1" "$BUILD/build.ninja" 2>/dev/null; then
  EXTRA="$EXTRA assets/soundfonts/GeneralUser-GS.sf2=$SOUNDFONT"
else
  echo "warning: no SoundFont ($SOUNDFONT) or FluidSynth is off in $BUILD: OPL3 music only" >&2
fi
if [ -d "$ASSETS/vr" ]; then
  for f in "$ASSETS"/vr/*; do EXTRA="$EXTRA assets/vr/$(basename "$f")=$f"; done
fi

# FFmpeg with Ogg Vorbis (see FFMPEG above).
if [ -f "$FFMPEG/libavcodec.so" ]; then
  for l in avcodec avformat avutil swresample swscale; do
    /usr/bin/llvm-strip -o stage/lib$l.so "$FFMPEG/lib$l.so"
    EXTRA="$EXTRA lib/arm64-v8a/lib$l.so=stage/lib$l.so"
  done
else
  echo "warning: no FFmpeg with Ogg in $FFMPEG: TIE98 recorded flight music will be silent" >&2
fi

python3 /root/android/apk_repack_sign.py base.apk "$OUT" "$KEY" "$CERT" \
  lib/arm64-v8a/libmain.so=stage/libmain.so lib/arm64-v8a/libopenxwa_vkshim.so=stage/libopenxwa_vkshim.so \
  AndroidManifest.xml=AndroidManifest.xml $EXTRA
java -jar /root/android/apksigner-0.4.3.jar verify "$OUT" 2>&1 | grep -v JAVA_TOOL || true
ls -la "$OUT"

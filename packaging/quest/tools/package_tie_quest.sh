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
KEY=${KEY:-/root/android/signing/openxwa-quest-key.pem}
CERT=${CERT:-/root/android/signing/openxwa-quest-cert.der}
OUT=${OUT:-/root/out/OpenTIE-VR-$VER.apk}
WORK=${WORK:-$SP/tieapk/$CODE}

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

EXTRA=""
for f in "$BUILD"/shaders/*.spv; do EXTRA="$EXTRA assets/shaders/$(basename "$f")=$f"; done
# App resources, plus the list the app extracts them by (quest_app.c).
: > stage/resources.lst
while IFS= read -r f; do
  rel=${f#$BUILD/}; EXTRA="$EXTRA assets/$rel=$f"; echo "$rel" >> stage/resources.lst
done < <(find "$BUILD/resources" -type f | sort)
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

cp "$HERE/../THIRD-PARTY-NOTICES-TIE.txt" stage/THIRD-PARTY-NOTICES.txt
EXTRA="$EXTRA assets/THIRD-PARTY-NOTICES.txt=stage/THIRD-PARTY-NOTICES.txt"
if [ -f "$ASSETS/vr_splash.png" ]; then EXTRA="$EXTRA assets/vr_splash.png=$ASSETS/vr_splash.png"; fi
if [ -d "$ASSETS/icon" ]; then
  for d in mdpi hdpi xhdpi xxhdpi xxxhdpi; do EXTRA="$EXTRA res/mipmap-$d/ic_launcher.png=$ASSETS/icon/ic_launcher_$d.png"; done
fi
if [ -d "$ASSETS/vr" ]; then
  for f in "$ASSETS"/vr/*; do EXTRA="$EXTRA assets/vr/$(basename "$f")=$f"; done
fi

python3 /root/android/apk_repack_sign.py base.apk "$OUT" "$KEY" "$CERT" \
  lib/arm64-v8a/libmain.so=stage/libmain.so lib/arm64-v8a/libopenxwa_vkshim.so=stage/libopenxwa_vkshim.so \
  AndroidManifest.xml=AndroidManifest.xml $EXTRA
java -jar /root/android/apksigner-0.4.3.jar verify "$OUT" 2>&1 | grep -v JAVA_TOOL || true
ls -la "$OUT"

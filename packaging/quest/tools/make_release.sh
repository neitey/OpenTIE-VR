#!/bin/bash
# make_release.sh <version>  e.g. 1.0.0-rc3
# Needs /root/out/OpenXWA-quest-<version>.apk and the launcher built with
# tools/launcher/build.sh <version>. Output: /root/out/release/
set -euo pipefail
VER=$1
R=/root/OpenXWA
OUT=/root/out/release
NAME=OpenXWA-Quest-$VER
APK=/root/out/OpenXWA-quest-$VER.apk
EXE=$R/tools/launcher/dist/OpenXWA-VR-Launcher.exe
test -f "$APK" && test -f "$EXE"
rm -rf "$OUT/$NAME" "$OUT/$NAME.zip"
mkdir -p "$OUT/$NAME"
cp "$EXE" "$OUT/$NAME/OpenXWA-VR-Launcher.exe"
cp "$APK" "$OUT/$NAME/OpenXWA-quest-$VER.apk"
sed "s/@VERSION@/$VER/g" $R/packaging/quest/README-RELEASE.txt | sed 's/$/\r/' > "$OUT/$NAME/README.txt"
sed 's/$/\r/' $R/packaging/quest/THIRD-PARTY-NOTICES.txt > "$OUT/$NAME/THIRD-PARTY-NOTICES.txt"
sed 's/$/\r/' $R/LICENSE > "$OUT/$NAME/LICENSE.txt"
cd "$OUT"
# Flat zip (Windows "Extract All" makes the folder; the launcher's update
# check extracts it into a folder named after the zip), fixed timestamps.
find "$NAME" -exec touch -d "2026-10-18 12:00:00" {} +
python3 - "$NAME" <<'EOF'
import os, sys, zipfile
name = sys.argv[1]
with zipfile.ZipFile(name + '.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for f in sorted(os.listdir(name)):
        z.write(os.path.join(name, f), f)
EOF
sha256sum "$NAME.zip" > SHA256SUMS.txt
cat SHA256SUMS.txt
unzip -l "$NAME.zip"

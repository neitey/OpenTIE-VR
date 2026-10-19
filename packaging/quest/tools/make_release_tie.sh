#!/bin/bash
# make_release_tie.sh <version>  e.g. 0.1.0
# Builds the OpenTIE-VR release zip from
#   /root/out/OpenTIE-VR-<version>.apk          (package_tie_quest.sh)
#   tools/launcher/dist/OpenTIE-VR-Launcher.exe  (tools/launcher/build.sh <version>)
# Output in /root/out/release-tie/ (OUT=...):
#   OpenTIE-VR-<version>.zip   flat zip: the APK, the launcher, README.txt,
#                              LICENSE.txt, SHA256SUMS.txt (of the files inside)
#                              [+ THIRD-PARTY-NOTICES.txt]
#   SHA256SUMS.txt             of the zip: upload it next to the zip, the
#                              launcher's update check verifies the download
#                              with it (asset names opentie-vr-*.zip and
#                              SHA256SUMS.txt, repo neitey/OpenTIE-VR).
set -euo pipefail
VER=${1:?usage: make_release_tie.sh <version>}
VER=${VER#v}
HERE="$(cd "$(dirname "$0")" && pwd)"
R="$(cd "$HERE/../../.." && pwd)"
OUT=${OUT:-/root/out/release-tie}
NAME=OpenTIE-VR-$VER
APK=${APK:-/root/out/OpenTIE-VR-$VER.apk}
EXE=${EXE:-$R/tools/launcher/dist/OpenTIE-VR-Launcher.exe}
for f in "$APK" "$EXE"; do
	[ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }
done
# The launcher installs the APK next to it; it must carry the version.
case "$(basename "$APK")" in
OpenTIE-VR-*.apk) ;;
*) echo "the APK must be named OpenTIE-VR-<version>.apk (the launcher looks for that name)" >&2; exit 1 ;;
esac
rm -rf "${OUT:?}/$NAME" "$OUT/$NAME.zip"
mkdir -p "$OUT/$NAME"
cp "$EXE" "$OUT/$NAME/OpenTIE-VR-Launcher.exe"
cp "$APK" "$OUT/$NAME/OpenTIE-VR-$VER.apk"
# Text files with Windows line ends.
sed "s/@VERSION@/$VER/g" "$R/packaging/quest/README-RELEASE-TIE.txt" | sed 's/$/\r/' > "$OUT/$NAME/README.txt"
sed 's/$/\r/' "$R/LICENSE" > "$OUT/$NAME/LICENSE.txt"
if [ -f "$R/packaging/quest/THIRD-PARTY-NOTICES-TIE.txt" ]; then
	sed 's/$/\r/' "$R/packaging/quest/THIRD-PARTY-NOTICES-TIE.txt" > "$OUT/$NAME/THIRD-PARTY-NOTICES.txt"
else
	echo "WARNING: packaging/quest/THIRD-PARTY-NOTICES-TIE.txt is missing; the zip has no third-party notices" >&2
fi
(cd "$OUT/$NAME" && sha256sum "OpenTIE-VR-$VER.apk" OpenTIE-VR-Launcher.exe | sed 's/$/\r/' > SHA256SUMS.txt)
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

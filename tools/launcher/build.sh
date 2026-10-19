#!/usr/bin/env bash
# Builds the OpenTIE-VR Launcher.
#   ./build.sh 1.0.0-rc3
# Produces dist/OpenTIE-VR-Launcher.exe (Windows GUI program, the release build) and
# dist/opentie-vr-launcher-linux (for testing).
set -euo pipefail

if [ $# -ne 1 ] || [ -z "$1" ]; then
	echo "usage: $0 <version>   (for example 1.0.0 or 1.0.0-rc3)" >&2
	exit 2
fi
VER="${1#v}"

cd "$(dirname "$0")"
GO="${GO:-go}"
if ! command -v "$GO" >/dev/null 2>&1 && [ -x /usr/local/go/bin/go ]; then
	GO=/usr/local/go/bin/go
fi

mkdir -p dist
LDFLAGS="-s -w -X main.version=$VER"

# -H windowsgui: a GUI program, so no console window opens (--console makes one).
CGO_ENABLED=0 GOOS=windows GOARCH=amd64 "$GO" build -trimpath -ldflags "$LDFLAGS -H windowsgui" -o dist/OpenTIE-VR-Launcher.exe .
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 "$GO" build -trimpath -ldflags "$LDFLAGS" -o dist/opentie-vr-launcher-linux .

echo "Built version $VER:"
ls -l dist/OpenTIE-VR-Launcher.exe dist/opentie-vr-launcher-linux

#!/bin/bash
# check_imports.sh <package-dir>
# Every DLL that OpenTIE.exe and the packaged DLLs import must either be in
# the package or be a Windows system DLL. Also lists what each module needs.
set -euo pipefail
PKG=$1
OBJDUMP=${OBJDUMP:-x86_64-w64-mingw32-objdump}
# Windows system DLLs (present on every Windows 10/11 x64 install).
SYSTEM_DLLS="advapi32.dll bcrypt.dll cfgmgr32.dll comdlg32.dll crypt32.dll d3d11.dll d3d12.dll
dbghelp.dll dinput8.dll dwmapi.dll dxgi.dll gdi32.dll hid.dll imm32.dll kernel32.dll msvcrt.dll
ntdll.dll ole32.dll oleaut32.dll opengl32.dll powrprof.dll setupapi.dll shell32.dll shlwapi.dll
user32.dll uuid.dll version.dll winmm.dll ws2_32.dll xinput1_4.dll dsound.dll secur32.dll
userenv.dll mfplat.dll mfreadwrite.dll psapi.dll iphlpapi.dll comctl32.dll pathcch.dll
d3dcompiler_47.dll dxcore.dll"
is_system() {
	case "$1" in
		api-ms-win-*|ext-ms-win-*) return 0 ;;
	esac
	for s in $SYSTEM_DLLS; do [ "$s" = "$1" ] && return 0; done
	return 1
}
status=0
shopt -s nullglob
for m in "$PKG"/*.exe "$PKG"/*.dll; do
	deps=$("$OBJDUMP" -p "$m" | sed -n 's/^\s*DLL Name: //p' | tr 'A-Z' 'a-z' | sort -u)
	echo "$(basename "$m"): $(echo $deps)"
	for d in $deps; do
		if is_system "$d"; then
			continue
		fi
		found=0
		for f in "$PKG"/*.dll; do
			if [ "$(basename "$f" | tr 'A-Z' 'a-z')" = "$d" ]; then found=1; fi
		done
		if [ $found = 0 ]; then
			echo "  MISSING: $d (imported by $(basename "$m"))"
			status=1
		fi
	done
done
[ $status = 0 ] && echo "imports: all non-system DLLs are packaged"
exit $status

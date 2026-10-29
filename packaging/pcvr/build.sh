#!/bin/bash
# OpenTIE-VR PCVR: Windows x86-64 build with OpenXR (VR) enabled, cross-compiled
# on a Linux x86-64 host with llvm-mingw, packaged as a zip. The PC sibling of
# the Quest APK (packaging/quest/tools/package_tie_quest.sh), made the way
# OpenXWA's PCVR package is (its packaging/pcvr/build.sh: same toolchain and
# shared dependencies, same --deps folder).
#
#   packaging/pcvr/build.sh [--version X.Y.Z] [--out DIR] [--deps DIR]
#                           [--vr-assets DIR] [--voice-model DIR] [--no-voice]
#                           [--revival-cockpits DIR] [--no-revival-cockpits]
#                           [--soundfont FILE] [--worktree] [--deps-only]
#
# By default the COMMITTED state is exported and built (HEAD plus the aeron,
# imuse and landru commits its gitlinks name); --worktree builds the working
# tree as it is instead (development).
#
# Dependencies (downloaded / built once into --deps, default /root/pcvr-deps,
# shared with OpenXWA's PCVR build): llvm-mingw, DXC + SPIRV-Cross +
# SDL_shadercross, SDL3 with OpenXR, zstd, the OpenXR loader, sherpa-onnx;
# for TIE also FFmpeg with Ogg Vorbis and WAV (TIE98's recorded music) next to
# the SMUSH decoders, FluidSynth (static, no glib) and the SpeexDSP resampler,
# in their own prefix (--deps/windows-tie).
#
# Not in the repository, taken from the build machine: the VR art (--vr-assets,
# default $SP/tieassets/vr), the speech model (--voice-model), X-Wing
# Revival's TIE cockpits as GLBs (--revival-cockpits DIR/glb/*.glb) and the
# GeneralUser GS SoundFont (--soundfont). Missing ones are reported; the game
# runs without them.
#
# Output: <out>/OpenTIE-VR-PCVR-<version>-win64.zip
set -euo pipefail

VERSION=0.0.0-dev
OUT=/root/out/release-tie
DEPS=/root/pcvr-deps
SP_DEFAULT=/tmp/claude-0/-home-claude/d5b75674-cc0b-5e76-88ee-fc1c5ab6c399/scratchpad
VR_ASSETS=${PCVR_VR_ASSETS:-$SP_DEFAULT/tieassets/vr}
VOICE_MODEL=${PCVR_VOICE_MODEL:-$SP_DEFAULT/vrassets/voice}
REVIVAL=${PCVR_REVIVAL_COCKPITS:-$SP_DEFAULT/vrassets/revival_tie}
SOUNDFONT=${PCVR_SOUNDFONT:-/root/android/soundfonts/GeneralUser-GS.sf2}
WITH_VOICE=1
WITH_REVIVAL=1
FROM_WORKTREE=0
DEPS_ONLY=0
JOBS=$(nproc)

while [ $# -gt 0 ]; do
	case "$1" in
		--version) VERSION=$2; shift 2 ;;
		--out) OUT=$2; shift 2 ;;
		--deps) DEPS=$2; shift 2 ;;
		--vr-assets) VR_ASSETS=$2; shift 2 ;;
		--voice-model) VOICE_MODEL=$2; shift 2 ;;
		--no-voice) WITH_VOICE=0; shift ;;
		--revival-cockpits) REVIVAL=$2; shift 2 ;;
		--no-revival-cockpits) WITH_REVIVAL=0; shift ;;
		--soundfont) SOUNDFONT=$2; shift 2 ;;
		--worktree) FROM_WORKTREE=1; shift ;;
		--deps-only) DEPS_ONLY=1; shift ;;
		-h|--help) sed -n '2,31p' "$0"; exit 0 ;;
		*) echo "build.sh: unknown argument $1" >&2; exit 2 ;;
	esac
done

REPO=$(cd "$(dirname "$0")/../.." && pwd)

# ---- pinned dependency versions ------------------------------------------
LLVM_MINGW_VERSION=20260922
LLVM_MINGW_SHA256=
# SDL main with SDL_openxr.h (the OpenXR GPU API is not in 3.4.x); the same
# commit the Quest build uses.
SDL_COMMIT=ce3af25d40d8d718cb5f04eea417ef111b693413
ZSTD_VERSION=1.5.7
FFMPEG_VERSION=8.0.1
SHADERCROSS_COMMIT=6b06e55c7c5d7e7a09a8a14f76e866dcfad5ab99
DXC_VERSION=1.8.2502
DXC_ARCHIVE=linux_dxc_2025_02_20.x86_64.tar.gz
DXC_SHA256=e0580d90dbf6053a783ddd8d5153285f0606e5deaad17a7a6452f03acdf88c71
OPENXR_SDK_TAG=release-1.1.63
SHERPA_VERSION=1.13.8
SHERPA_ARCHIVE=sherpa-onnx-v${SHERPA_VERSION}-win-x64-shared-MT-Release-no-tts.tar.bz2

VR_ASSET_FILES="logo.rgba frame.rgba vader.rgba xvt_tie_pilot.rgba xvt_rebel_pilot.rgba
starwars_logo.rgba LiberationSans-BoldItalic.ttf"
SPEEXDSP_TAG=SpeexDSP-1.2.1
FLUIDSYNTH_TAG=v2.5.7
VOICE_FILES="encoder.onnx decoder.onnx joiner.onnx tokens.txt"

TOOLCHAIN=$DEPS/llvm-mingw
HOST=$DEPS/host        # Linux tools (shadercross, SDL for it, fsr3 shadergen)
WIN=$DEPS/windows      # Windows libraries (MinGW prefix)
SRC=$DEPS/src
BUILD=$DEPS/build
DL=$DEPS/dl
mkdir -p "$DEPS" "$HOST" "$WIN" "$SRC" "$BUILD" "$DL" "$OUT"
export PATH="$TOOLCHAIN/bin:$PATH"
log() { printf '\n== %s\n' "$*"; }

fetch() { # url file [sha256]
	if [ ! -s "$DL/$2" ]; then
		curl -sSL --fail -o "$DL/$2.part" "$1"
		mv "$DL/$2.part" "$DL/$2"
	fi
	if [ -n "${3:-}" ]; then
		echo "$3  $DL/$2" | sha256sum -c --quiet -
	fi
}

git_at() { # url commit-or-tag dir
	if [ ! -d "$3/.git" ]; then
		rm -rf "$3"
		git init -q "$3"
		git -C "$3" remote add origin "$1"
	fi
	if ! git -C "$3" checkout -q --force "$2" 2>/dev/null; then
		git -C "$3" fetch -q --depth 1 origin "$2"
		git -C "$3" checkout -q --force FETCH_HEAD
	fi
	git -C "$3" clean -qfdx
}

stamp() { [ -f "$DEPS/stamp-$1" ]; }
done_stamp() { touch "$DEPS/stamp-$1"; }

TOOLCHAIN_FILE=$DEPS/mingw-w64-x86_64.cmake
write_toolchain_file() {
	cat > "$TOOLCHAIN_FILE" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $TOOLCHAIN/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER $TOOLCHAIN/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER $TOOLCHAIN/bin/x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH "$WIN")
set(CMAKE_PREFIX_PATH "$WIN")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "$WIN/lib/pkgconfig:$WIN/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
EOF
}

# ---- toolchain -------------------------------------------------------------
if ! stamp llvm-mingw-$LLVM_MINGW_VERSION; then
	log "llvm-mingw $LLVM_MINGW_VERSION"
	name=llvm-mingw-$LLVM_MINGW_VERSION-ucrt-ubuntu-22.04-x86_64
	fetch "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_VERSION/$name.tar.xz" "$name.tar.xz" "$LLVM_MINGW_SHA256"
	rm -rf "$TOOLCHAIN" "$DEPS/$name"
	tar -xf "$DL/$name.tar.xz" -C "$DEPS"
	mv "$DEPS/$name" "$TOOLCHAIN"
	done_stamp llvm-mingw-$LLVM_MINGW_VERSION
fi
write_toolchain_file

# ---- host shader tools -----------------------------------------------------
if ! stamp host-sdl-$SDL_COMMIT; then
	log "SDL3 (host, for shadercross)"
	git_at https://github.com/libsdl-org/SDL.git "$SDL_COMMIT" "$SRC/SDL-host"
	rm -rf "$BUILD/sdl-host"
	cmake -S "$SRC/SDL-host" -B "$BUILD/sdl-host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$HOST" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF \
		-DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF > "$BUILD/sdl-host.log"
	cmake --build "$BUILD/sdl-host" --parallel "$JOBS" >> "$BUILD/sdl-host.log"
	cmake --install "$BUILD/sdl-host" >> "$BUILD/sdl-host.log"
	done_stamp host-sdl-$SDL_COMMIT
fi

if ! stamp shadercross-$SHADERCROSS_COMMIT; then
	log "SDL_shadercross + SPIRV-Cross + DXC $DXC_VERSION (host)"
	fetch "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v$DXC_VERSION/$DXC_ARCHIVE" "$DXC_ARCHIVE" "$DXC_SHA256"
	git_at https://github.com/libsdl-org/SDL_shadercross.git "$SHADERCROSS_COMMIT" "$SRC/SDL_shadercross"
	git -C "$SRC/SDL_shadercross" apply "$REPO/packaging/common/shadercross-fsr3.patch"
	git -C "$SRC/SDL_shadercross" submodule update -q --init --depth 1 external/SPIRV-Cross
	dxc=$SRC/SDL_shadercross/external/DirectXShaderCompiler-binaries
	mkdir -p "$dxc/linux"
	tar -xzf "$DL/$DXC_ARCHIVE" -C "$dxc/linux"
	rm -rf "$BUILD/spirv-cross" "$BUILD/shadercross"
	cmake -S "$SRC/SDL_shadercross/external/SPIRV-Cross" -B "$BUILD/spirv-cross" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOST" -DSPIRV_CROSS_CLI=OFF \
		-DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_SHARED=ON -DSPIRV_CROSS_STATIC=OFF > "$BUILD/shadercross.log"
	cmake --build "$BUILD/spirv-cross" --parallel "$JOBS" >> "$BUILD/shadercross.log"
	cmake --install "$BUILD/spirv-cross" >> "$BUILD/shadercross.log"
	cmake -S "$SRC/SDL_shadercross" -B "$BUILD/shadercross" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$HOST" -DCMAKE_PREFIX_PATH="$HOST" \
		-DDirectXShaderCompiler_ROOT="$dxc" -DSDLSHADERCROSS_CLI=ON -DSDLSHADERCROSS_DXC=ON \
		-DSDLSHADERCROSS_INSTALL=ON -DSDLSHADERCROSS_INSTALL_RUNTIME=OFF -DSDLSHADERCROSS_SHARED=ON \
		-DSDLSHADERCROSS_STATIC=OFF -DSDLSHADERCROSS_SPIRVCROSS_SHARED=ON -DSDLSHADERCROSS_VENDORED=OFF \
		-DCMAKE_INSTALL_RPATH="$HOST/lib" -DCMAKE_BUILD_WITH_INSTALL_RPATH=OFF >> "$BUILD/shadercross.log"
	cmake --build "$BUILD/shadercross" --parallel "$JOBS" >> "$BUILD/shadercross.log"
	cmake --install "$BUILD/shadercross" >> "$BUILD/shadercross.log"
	cp "$dxc/linux/lib/libdxcompiler.so" "$dxc/linux/lib/libdxil.so" "$HOST/lib/"
	done_stamp shadercross-$SHADERCROSS_COMMIT
fi
export LD_LIBRARY_PATH="$HOST/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# ---- Windows libraries -----------------------------------------------------
if ! stamp win-sdl-$SDL_COMMIT-xrlog1; then
	log "SDL3 (Windows, OpenXR GPU support)"
	git_at https://github.com/libsdl-org/SDL.git "$SDL_COMMIT" "$SRC/SDL-win"
	git -C "$SRC/SDL-win" apply "$REPO/packaging/common/sdl-gpu-d3d12-hdr.patch"
	# OpenXR failures name their XrResult (instance vs. no headset).
	git -C "$SRC/SDL-win" apply "$REPO/packaging/common/sdl-openxr-results.patch"
	rm -rf "$BUILD/sdl-win"
	cmake -S "$SRC/SDL-win" -B "$BUILD/sdl-win" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$WIN" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
		-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_GPU_OPENXR=ON > "$BUILD/sdl-win.log"
	grep -q "HAVE_GPU_OPENXR 1\|#define HAVE_GPU_OPENXR 1" "$BUILD/sdl-win/include-config-release/build_config/SDL_build_config.h" 2>/dev/null \
		|| grep -rq "define HAVE_GPU_OPENXR 1" "$BUILD/sdl-win" || { echo "SDL: no OpenXR GPU support" >&2; exit 1; }
	cmake --build "$BUILD/sdl-win" --parallel "$JOBS" >> "$BUILD/sdl-win.log"
	cmake --install "$BUILD/sdl-win" >> "$BUILD/sdl-win.log"
	mkdir -p "$WIN/licenses" && cp "$SRC/SDL-win/LICENSE.txt" "$WIN/licenses/SDL3.txt"
	done_stamp win-sdl-$SDL_COMMIT-xrlog1
fi

if ! stamp win-zstd-$ZSTD_VERSION; then
	log "zstd $ZSTD_VERSION (Windows)"
	git_at https://github.com/facebook/zstd.git "v$ZSTD_VERSION" "$SRC/zstd"
	rm -rf "$BUILD/zstd-win"
	cmake -S "$SRC/zstd/build/cmake" -B "$BUILD/zstd-win" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$WIN" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
		-DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_SHARED=ON -DZSTD_BUILD_STATIC=OFF -DZSTD_BUILD_TESTS=OFF > "$BUILD/zstd-win.log"
	cmake --build "$BUILD/zstd-win" --parallel "$JOBS" >> "$BUILD/zstd-win.log"
	cmake --install "$BUILD/zstd-win" >> "$BUILD/zstd-win.log"
	mkdir -p "$WIN/licenses" && cp "$SRC/zstd/LICENSE" "$WIN/licenses/zstd.txt"
	done_stamp win-zstd-$ZSTD_VERSION
fi

if ! stamp win-ffmpeg-$FFMPEG_VERSION; then
	log "FFmpeg $FFMPEG_VERSION (Windows, film decoders only)"
	git_at https://github.com/FFmpeg/FFmpeg.git "n$FFMPEG_VERSION" "$SRC/FFmpeg"
	rm -rf "$BUILD/ffmpeg-win" && mkdir -p "$BUILD/ffmpeg-win"
	(cd "$BUILD/ffmpeg-win" && "$SRC/FFmpeg/configure" --prefix="$WIN" --target-os=mingw32 --arch=x86_64 \
		--cross-prefix=x86_64-w64-mingw32- --cc=x86_64-w64-mingw32-clang --cxx=x86_64-w64-mingw32-clang++ \
		--disable-autodetect --disable-avdevice --disable-avfilter --disable-debug --disable-doc \
		--disable-everything --disable-network --disable-programs --disable-static --enable-shared \
		--enable-decoder=adpcm_vima --enable-decoder=sanm --enable-demuxer=smush --enable-w32threads \
		> "$BUILD/ffmpeg-win.log" && make -j"$JOBS" >> "$BUILD/ffmpeg-win.log" 2>&1 && make install >> "$BUILD/ffmpeg-win.log" 2>&1)
	mkdir -p "$WIN/licenses" && cp "$SRC/FFmpeg/COPYING.LGPLv2.1" "$WIN/licenses/FFmpeg-LGPL-2.1.txt"
	done_stamp win-ffmpeg-$FFMPEG_VERSION
fi

if ! stamp win-openxr-$OPENXR_SDK_TAG; then
	log "OpenXR loader $OPENXR_SDK_TAG (Windows)"
	git_at https://github.com/KhronosGroup/OpenXR-SDK.git "$OPENXR_SDK_TAG" "$SRC/OpenXR-SDK"
	# MinGW: lld takes no GNU version script; export with the MSVC .def instead.
	sed -i 's|"-Wl,--version-script=\\"${CMAKE_CURRENT_SOURCE_DIR}/openxr-loader.map\\""|""|' \
		"$SRC/OpenXR-SDK/src/loader/CMakeLists.txt"
	if grep -q -- "--version-script" "$SRC/OpenXR-SDK/src/loader/CMakeLists.txt"; then echo "OpenXR loader patch failed" >&2; exit 1; fi
	rm -rf "$BUILD/openxr-win"
	cmake -S "$SRC/OpenXR-SDK" -B "$BUILD/openxr-win" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$WIN" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
		-DDYNAMIC_LOADER=ON -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF -DBUILD_CONFORMANCE_TESTS=OFF \
		-DBUILD_WITH_SYSTEM_JSONCPP=OFF \
		-DCMAKE_SHARED_LINKER_FLAGS="-static $SRC/OpenXR-SDK/src/loader/openxr-loader.def" > "$BUILD/openxr-win.log"
	cmake --build "$BUILD/openxr-win" --parallel "$JOBS" >> "$BUILD/openxr-win.log"
	cmake --install "$BUILD/openxr-win" >> "$BUILD/openxr-win.log"
	# SDL loads it by the name the Khronos Windows builds use.
	cp "$BUILD/openxr-win/src/loader/libopenxr_loader.dll" "$WIN/bin/openxr_loader.dll"
	mkdir -p "$WIN/licenses" && cp "$SRC/OpenXR-SDK/LICENSE" "$WIN/licenses/OpenXR-SDK.txt"
	done_stamp win-openxr-$OPENXR_SDK_TAG
fi

if [ "$WITH_VOICE" = 1 ] && ! stamp win-sherpa-$SHERPA_VERSION; then
	log "sherpa-onnx $SHERPA_VERSION (prebuilt Windows DLLs, static CRT)"
	fetch "https://github.com/k2-fsa/sherpa-onnx/releases/download/v$SHERPA_VERSION/$SHERPA_ARCHIVE" "$SHERPA_ARCHIVE"
	rm -rf "$DEPS/sherpa" && mkdir -p "$DEPS/sherpa"
	tar -xjf "$DL/$SHERPA_ARCHIVE" -C "$DEPS/sherpa" --strip-components=1
	done_stamp win-sherpa-$SHERPA_VERSION
fi

# Sources and build trees of the dependencies are no longer needed (disk).
[ "${PCVR_KEEP_DEP_SOURCES:-0}" = 1 ] || rm -rf "$SRC" "$BUILD"/sdl-* "$BUILD"/zstd-win "$BUILD"/ffmpeg-win \
	"$BUILD"/openxr-win "$BUILD"/spirv-cross "$BUILD"/shadercross "$DL"/*.tar.* "$DL"/*.zip
mkdir -p "$SRC"

# ---- TIE's own Windows libraries (their own prefix) ----------------------------
WINTIE=$DEPS/windows-tie
TIE_TOOLCHAIN_FILE=$DEPS/mingw-w64-x86_64-tie.cmake
sed "s|$WIN|$WINTIE|g" "$TOOLCHAIN_FILE" > "$TIE_TOOLCHAIN_FILE"
if ! stamp tie-win-prefix-$SDL_COMMIT-$OPENXR_SDK_TAG; then
	log "TIE prefix: the shared Windows libraries (SDL3, zstd, OpenXR loader)"
	rm -rf "${WINTIE:?}"
	cp -a "$WIN" "$WINTIE"
	# FFmpeg comes below, with TIE's decoders.
	rm -rf "${WINTIE:?}"/bin/av*.dll "${WINTIE:?}"/bin/sw*.dll "${WINTIE:?}"/lib/libav* "${WINTIE:?}"/lib/libsw* \
		"${WINTIE:?}"/lib/pkgconfig/libav* "${WINTIE:?}"/lib/pkgconfig/libsw* "${WINTIE:?}"/include/libav* \
		"${WINTIE:?}"/include/libsw*
	rm -f "${DEPS:?}"/stamp-tie-win-ffmpeg-*
	done_stamp tie-win-prefix-$SDL_COMMIT-$OPENXR_SDK_TAG
fi

if ! stamp tie-win-ffmpeg-$FFMPEG_VERSION; then
	log "FFmpeg $FFMPEG_VERSION (Windows, SMUSH films + Ogg Vorbis / WAV music)"
	git_at https://github.com/FFmpeg/FFmpeg.git "n$FFMPEG_VERSION" "$SRC/FFmpeg"
	rm -rf "${BUILD:?}/ffmpeg-tie"
	mkdir -p "$BUILD/ffmpeg-tie"
	(cd "$BUILD/ffmpeg-tie" && "$SRC/FFmpeg/configure" --prefix="$WINTIE" --target-os=mingw32 --arch=x86_64 \
		--cross-prefix=x86_64-w64-mingw32- --cc=x86_64-w64-mingw32-clang --cxx=x86_64-w64-mingw32-clang++ \
		--disable-autodetect --disable-avdevice --disable-avfilter --disable-debug --disable-doc \
		--disable-everything --disable-network --disable-programs --disable-static --enable-shared \
		--enable-decoder=adpcm_vima --enable-decoder=sanm --enable-demuxer=smush \
		--enable-decoder=vorbis --enable-demuxer=ogg --enable-demuxer=wav \
		--enable-decoder=pcm_u8 --enable-decoder=pcm_s16le --enable-w32threads \
		> "$BUILD/ffmpeg-tie.log" && make -j"$JOBS" >> "$BUILD/ffmpeg-tie.log" 2>&1 && make install >> "$BUILD/ffmpeg-tie.log" 2>&1)
	mkdir -p "$WINTIE/licenses" && cp "$SRC/FFmpeg/COPYING.LGPLv2.1" "$WINTIE/licenses/FFmpeg-LGPL-2.1.txt"
	rm -rf "${SRC:?}/FFmpeg" "${BUILD:?}/ffmpeg-tie"
	done_stamp tie-win-ffmpeg-$FFMPEG_VERSION
fi

if ! stamp tie-win-speexdsp-$SPEEXDSP_TAG; then
	log "SpeexDSP resampler $SPEEXDSP_TAG (Windows, static)"
	git_at https://github.com/xiph/speexdsp.git "$SPEEXDSP_TAG" "$SRC/speexdsp"
	S=$SRC/speexdsp
	mkdir -p "$WINTIE/include/speex" "$WINTIE/lib/pkgconfig" "$BUILD/speexdsp"
	sed -e 's/@INCLUDE_STDINT@/#include <stdint.h>/' -e 's/@SIZE16@/int16_t/' -e 's/@USIZE16@/uint16_t/' \
		-e 's/@SIZE32@/int32_t/' -e 's/@USIZE32@/uint32_t/' "$S/include/speex/speexdsp_config_types.h.in" \
		> "$WINTIE/include/speex/speexdsp_config_types.h"
	cp "$S/include/speex/speexdsp_types.h" "$S/include/speex/speex_resampler.h" "$WINTIE/include/speex/"
	x86_64-w64-mingw32-clang -O2 -DFLOATING_POINT -DEXPORT= -I"$WINTIE/include" -I"$S/libspeexdsp" -I"$S/include" \
		-c "$S/libspeexdsp/resample.c" -o "$BUILD/speexdsp/resample.o"
	rm -f "$WINTIE/lib/libspeexdsp.a"
	x86_64-w64-mingw32-ar rcs "$WINTIE/lib/libspeexdsp.a" "$BUILD/speexdsp/resample.o"
	cat > "$WINTIE/lib/pkgconfig/speexdsp.pc" <<PC
prefix=$WINTIE
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: speexdsp
Description: SpeexDSP resampler only (static)
Version: 1.2.1
Libs: -L\${libdir} -lspeexdsp
Cflags: -I\${includedir}
PC
	mkdir -p "$WINTIE/licenses" && cp "$S/COPYING" "$WINTIE/licenses/SpeexDSP-BSD-3-Clause.txt"
	rm -rf "${SRC:?}/speexdsp" "${BUILD:?}/speexdsp"
	done_stamp tie-win-speexdsp-$SPEEXDSP_TAG
fi

if ! stamp tie-win-fluidsynth-$FLUIDSYNTH_TAG; then
	log "FluidSynth $FLUIDSYNTH_TAG (Windows, static, osal=cpp11, no glib)"
	git_at https://github.com/FluidSynth/fluidsynth.git "$FLUIDSYNTH_TAG" "$SRC/fluidsynth"
	# gcem (constexpr math, header only): a submodule, given by path (the
	# cross toolchain's find root would not look in the source tree, and
	# FluidSynth's fallback download is not reachable here).
	git -C "$SRC/fluidsynth" submodule update -q --init gcem
	rm -rf "${BUILD:?}/fluidsynth-win"
	cmake -S "$SRC/fluidsynth" -B "$BUILD/fluidsynth-win" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$WINTIE" -DCMAKE_TOOLCHAIN_FILE="$TIE_TOOLCHAIN_FILE" \
		-DGCEM_INCLUDE_DIR="$SRC/fluidsynth/gcem/include" \
		-DBUILD_SHARED_LIBS=OFF -Dosal=cpp11 -Denable-threads=OFF -Denable-openmp=OFF -Denable-aufile=OFF \
		-Denable-dbus=OFF -Denable-ipv6=OFF -Denable-jack=OFF -Denable-ladspa=OFF -Denable-libinstpatch=OFF \
		-Denable-libsndfile=OFF -Denable-midishare=OFF -Denable-network=OFF -Denable-oss=OFF -Denable-sdl3=OFF \
		-Denable-pulseaudio=OFF -Denable-pipewire=OFF -Denable-readline=OFF -Denable-systemd=OFF \
		-Denable-native-dls=OFF -Denable-portaudio=OFF -Denable-dsound=OFF -Denable-wasapi=OFF \
		-Denable-waveout=OFF -Denable-winmidi=OFF -Denable-alsa=OFF -Denable-opensles=OFF -Denable-oboe=OFF \
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON > "$BUILD/fluidsynth-win.log"
	cmake --build "$BUILD/fluidsynth-win" --target libfluidsynth --parallel "$JOBS" >> "$BUILD/fluidsynth-win.log"
	lib=$(find "$BUILD/fluidsynth-win" -name 'libfluidsynth*.a' | head -1)
	test -n "$lib"
	cp "$lib" "$WINTIE/lib/libfluidsynth.a"
	mkdir -p "$WINTIE/include/fluidsynth"
	cp "$SRC/fluidsynth/include/fluidsynth.h" "$WINTIE/include/" 2>/dev/null \
		|| cp "$BUILD/fluidsynth-win/include/fluidsynth.h" "$WINTIE/include/"
	cp "$SRC/fluidsynth/include/fluidsynth/"*.h "$WINTIE/include/fluidsynth/"
	cp "$BUILD/fluidsynth-win/include/fluidsynth/"*.h "$WINTIE/include/fluidsynth/" 2>/dev/null || true
	cat > "$WINTIE/lib/pkgconfig/fluidsynth.pc" <<PC
prefix=$WINTIE
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: FluidSynth
Description: Software SoundFont synth (static, osal=cpp11, no glib)
Version: ${FLUIDSYNTH_TAG#v}
Libs: -L\${libdir} -lfluidsynth -lc++
Cflags: -I\${includedir} -DFLUIDSYNTH_NOT_A_DLL
PC
	mkdir -p "$WINTIE/licenses" && cp "$SRC/fluidsynth/LICENSE" "$WINTIE/licenses/FluidSynth-LGPL-2.1.txt"
	rm -rf "${SRC:?}/fluidsynth" "${BUILD:?}/fluidsynth-win"
	done_stamp tie-win-fluidsynth-$FLUIDSYNTH_TAG
fi
mkdir -p "$SRC"
[ "$DEPS_ONLY" = 0 ] || { log "dependencies ready in $DEPS"; exit 0; }

# ---- the game ----------------------------------------------------------------
STAGE=$BUILD/src-opentie
archive_tree() { # repo-dir commit stage-dir : the commit's files, nested submodules too
	local repo=${1:?} sha=${2:?} dest=${3:?} mode type subsha path
	mkdir -p "$dest"
	git -C "$repo" archive "$sha" | tar -x -C "$dest"
	git -C "$repo" ls-tree -r "$sha" | while read -r mode type subsha path; do
		[ "$type" = commit ] || continue
		archive_tree "$repo/$path" "$subsha" "$dest/$path"
	done
}
archive_sub() { # superproject-path
	local sub=${1:?} sha
	sha=$(git -C "$REPO" ls-tree HEAD "$sub" | awk '{print $3}')
	archive_tree "$REPO/$sub" "$sha" "$STAGE/$sub"
	echo "$sub $(echo "$sha" | cut -c1-7)"
}
if [ "$FROM_WORKTREE" = 1 ]; then
	SOURCE=$REPO
	DESCRIBE="$(git -C "$REPO" rev-parse --short HEAD) (working tree)"
else
	log "exporting $(git -C "$REPO" rev-parse --short HEAD) (+ aeron, imuse, landru at their gitlinks)"
	rm -rf "${STAGE:?}"
	mkdir -p "$STAGE"
	git -C "$REPO" archive HEAD | tar -x -C "$STAGE"
	subs=$(for s in aeron src/imuse src/landru; do archive_sub "$s"; done | paste -sd, - | sed 's/,/, /g')
	SOURCE=$STAGE
	DESCRIBE="$(git -C "$REPO" rev-parse --short HEAD), $subs"
fi

if ! stamp host-aeron-tools; then
	log "aeron host tools (FSR3 shader generator)"
	rm -rf "${BUILD:?}/aeron-host"
	cmake -S "$SOURCE/aeron" -B "$BUILD/aeron-host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$HOST" -DCMAKE_PREFIX_PATH="$HOST" -DAERON_HOST_TOOLS_ONLY=ON > "$BUILD/aeron-host.log"
	cmake --build "$BUILD/aeron-host" --parallel "$JOBS" >> "$BUILD/aeron-host.log"
	cmake --install "$BUILD/aeron-host" >> "$BUILD/aeron-host.log"
	done_stamp host-aeron-tools
fi

log "OpenTIE-VR $VERSION ($DESCRIBE)"
GAME_BUILD=$BUILD/opentie-win
# One build tree; start it afresh when the source tree changes (export vs. --worktree).
if [ -f "$GAME_BUILD/CMakeCache.txt" ] && ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$SOURCE" "$GAME_BUILD/CMakeCache.txt"; then
	rm -rf "${GAME_BUILD:?}"
fi
cmake -S "$SOURCE" -B "$GAME_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_TOOLCHAIN_FILE="$TIE_TOOLCHAIN_FILE" \
	-DAERON_FSR3_SHADERGEN_EXECUTABLE="$HOST/bin/aeron_fsr3_shadergen" \
	-DAERON_SHADERCROSS_EXECUTABLE="$HOST/bin/shadercross" \
	-DAERON_SHADER_FORMATS="DXIL;SPIRV" \
	-DAERON_ENABLE_OPENXR=ON \
	-DAERON_SDL_OPENXR_HEADER="$WINTIE/include/SDL3/SDL_openxr.h" \
	-DENABLE_ASAN=OFF -DENABLE_FLUIDSYNTH=ON -DENABLE_NUKED_SC55=ON \
	-DTIE_BUILD_TOOLS=OFF -DTIE_ENABLE_DEBUG_UI=OFF \
	-DCMAKE_C_FLAGS="-Wno-error=incompatible-pointer-types -Wno-error=incompatible-function-pointer-types" \
	-DTIE_VERSION="$VERSION" > "$BUILD/opentie-configure.log"
grep -q "OpenXR presentation enabled" "$BUILD/opentie-configure.log" || { echo "OpenXR is not enabled" >&2; exit 1; }
grep -q "FluidSynth found" "$BUILD/opentie-configure.log" || { echo "FluidSynth was not found" >&2; exit 1; }
cmake --build "$GAME_BUILD" --target tie --parallel "$JOBS"

# ---- package -----------------------------------------------------------------
NAME=OpenTIE-VR-PCVR-$VERSION-win64
PKGROOT=$BUILD/package-tie
PKG=$PKGROOT/$NAME
log "packaging $NAME"
rm -rf "${PKGROOT:?}"
mkdir -p "$PKG/licenses" "$PKG/shaders" "$PKG/vr"
cp "$GAME_BUILD/OpenTIE.exe" "$PKG/"
"$TOOLCHAIN/bin/x86_64-w64-mingw32-strip" "$PKG/OpenTIE.exe"
cp "$GAME_BUILD"/shaders/*.dxil "$GAME_BUILD"/shaders/*.spv "$PKG/shaders/"
cp -a "$GAME_BUILD/resources" "$PKG/"
for dll in SDL3.dll zstd.dll libzstd.dll; do [ -f "$WINTIE/bin/$dll" ] && cp "$WINTIE/bin/$dll" "$PKG/"; done
cp "$WINTIE"/bin/av*.dll "$WINTIE"/bin/sw*.dll "$PKG/"
cp "$WINTIE/bin/openxr_loader.dll" "$PKG/"
# The llvm-mingw C++/unwind runtimes, when something imports them.
for dll in libc++.dll libunwind.dll libwinpthread-1.dll; do
	cp "$TOOLCHAIN/x86_64-w64-mingw32/bin/$dll" "$PKG/" 2>/dev/null || true
done
if [ "$WITH_VOICE" = 1 ]; then
	cp "$DEPS/sherpa/lib/sherpa-onnx-c-api.dll" "$DEPS/sherpa/lib/onnxruntime.dll" "$PKG/"
	missing=""
	mkdir -p "$PKG/voice"
	for f in $VOICE_FILES; do
		if [ -f "$VOICE_MODEL/$f" ]; then cp "$VOICE_MODEL/$f" "$PKG/voice/"; else missing="$missing $f"; fi
	done
	[ -z "$missing" ] || echo "WARNING: voice model files missing from $VOICE_MODEL:$missing" >&2
fi
if [ "$WITH_REVIVAL" = 1 ]; then
	# X-Wing Revival's TIE cockpits (tiefighter tieinterceptor tiebomber
	# gunboat), where the APK has them: resources/cockpit/revival/*.glb.
	mkdir -p "$PKG/resources/cockpit/revival"
	cp "$REVIVAL"/glb/*.glb "$PKG/resources/cockpit/revival/" 2>/dev/null || true
	count=$( (ls "$PKG"/resources/cockpit/revival/*.glb 2>/dev/null || true) | wc -l)
	if [ "$count" -ne 4 ]; then
		echo "ERROR: $count of 4 X-Wing Revival TIE cockpits in $REVIVAL/glb (--no-revival-cockpits builds without them)" >&2
		exit 1
	fi
fi
# Music: the SoundFont FluidSynth plays iMUSE with (midi_resources.c looks in
# <resource root>/soundfonts, the folder of OpenTIE.exe).
if [ -f "$SOUNDFONT" ]; then
	mkdir -p "$PKG/soundfonts"
	cp "$SOUNDFONT" "$PKG/soundfonts/GeneralUser-GS.sf2"
	cp "$(dirname "$SOUNDFONT")/LICENSE.txt" "$PKG/licenses/GeneralUser-GS.txt" 2>/dev/null || true
else
	echo "WARNING: no SoundFont ($SOUNDFONT): OPL3 music only" >&2
fi
missing=""
for f in $VR_ASSET_FILES; do
	if [ -f "$VR_ASSETS/$f" ]; then cp "$VR_ASSETS/$f" "$PKG/vr/$f"; else missing="$missing $f"; fi
done
[ -z "$missing" ] || echo "WARNING: VR assets missing:$missing" >&2

# Drop runtime DLLs nothing imports (the exe and the other DLLs).
imports() { "$TOOLCHAIN/bin/x86_64-w64-mingw32-objdump" -p "$1" | sed -n 's/^\s*DLL Name: //p'; }
needed=$( { imports "$PKG/OpenTIE.exe"; for d in "$PKG"/*.dll; do imports "$d"; done; } | tr 'A-Z' 'a-z' | sort -u)
for d in libc++.dll libunwind.dll libwinpthread-1.dll; do
	if [ -f "$PKG/$d" ] && ! echo "$needed" | grep -qx "$d"; then rm "$PKG/$d"; fi
done

# Licences and notices.
cp "$REPO/LICENSE" "$PKG/LICENSE.txt" 2>/dev/null || cp "$REPO/COPYING" "$PKG/LICENSE.txt" 2>/dev/null || true
cp "$REPO/packaging/quest/THIRD-PARTY-NOTICES-TIE.txt" "$PKG/THIRD-PARTY-NOTICES.txt"
cat "$REPO/packaging/pcvr/THIRD-PARTY-NOTICES-PCVR.txt" >> "$PKG/THIRD-PARTY-NOTICES.txt"
cp "$WINTIE"/licenses/*.txt "$PKG/licenses/"
cp "$TOOLCHAIN/LICENSE.TXT" "$PKG/licenses/llvm-mingw.txt" 2>/dev/null || true
cp "$SOURCE/src/landru/LICENSE" "$PKG/licenses/Landru-GPL-3.0.txt"
cp "$SOURCE/src/imuse/LICENSE" "$PKG/licenses/iMUSE-GPL-3.0.txt"
cp "$SOURCE/src/imuse/third_party/nuked-opl3-fast/LICENSE" "$PKG/licenses/Nuked-OPL3-LGPL-2.1.txt" 2>/dev/null || true
cp "$SOURCE/src/imuse/third_party/nuked-sc55/LICENSE" "$PKG/licenses/Nuked-SC55.txt" 2>/dev/null || true
if [ "$WITH_VOICE" = 1 ]; then
	cp "$SOURCE/third_party/sherpa-onnx/LICENSE" "$PKG/licenses/sherpa-onnx.txt" 2>/dev/null || true
fi
sed -e "s/@VERSION@/$VERSION/g" "$REPO/packaging/pcvr/README-PCVR.txt" > "$PKG/README-PCVR.txt"
# Windows line endings for the text files people open in Notepad.
for f in "$PKG"/*.txt "$PKG"/licenses/*.txt; do sed -i 's/\r\?$/\r/' "$f"; done
echo "OpenTIE-VR (PCVR) $VERSION, built from $DESCRIBE on $(date -u +%Y-%m-%d)" > "$PKG/BUILD-INFO.txt"
printf '@echo off\r\nrem Starts OpenTIE-VR in VR (OpenXR). Extra arguments are passed on.\r\nstart "" "%%~dp0OpenTIE.exe" --xr %%*\r\n' > "$PKG/OpenTIE VR.bat"

# ---- checks ------------------------------------------------------------------
log "checks"
OBJDUMP="$TOOLCHAIN/bin/x86_64-w64-mingw32-objdump" "$REPO/packaging/pcvr/check_imports.sh" "$PKG"
ls "$PKG"/shaders/*.dxil > /dev/null
ls "$PKG"/shaders/*.spv > /dev/null
test -f "$PKG/resources/aeron/scene3d_defaults.yaml"
grep -a -q "openxr_loader.dll" "$PKG/SDL3.dll"

mkdir -p "$OUT"
rm -f "$OUT/$NAME.zip"
(cd "$PKGROOT" && find "$NAME" -exec touch -h -d "@315532800" {} + && find "$NAME" -print | LC_ALL=C sort | TZ=UTC zip -X -9 -q "$OUT/$NAME.zip" -@)
unzip -tq "$OUT/$NAME.zip"
rm -rf "${PKGROOT:?}"
log "done: $OUT/$NAME.zip ($(du -h "$OUT/$NAME.zip" | cut -f1))"

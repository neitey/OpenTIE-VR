#!/usr/bin/env python3
"""Package an OpenXWA Android (arm64) build as a sideloadable Meta Quest APK.

Usage:
  build_apk.py --build-dir <cmake build with libmain.so> --sdl-source <SDL checkout>
               --android-jar <platforms/android-NN/android.jar> --d8-jar <build-tools/lib/d8.jar>
               --apksigner-jar <build-tools/lib/apksigner.jar> --aapt2 <aapt2 for this host>
               --lib-dir <dir with dependency .so files> --ndk <NDK root>
               [--keystore debug.keystore] [--out OpenXWA-quest.apk]

Only the JDK (javac, keytool, java) plus the listed SDK pieces are needed: no
Gradle. Native libraries are stored compressed (extractNativeLibs=true);
resources.arsc is stored and 4-byte aligned as Android R+ requires.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

# Steam Frame: its Android layer (Lepton) has no clipboard service, so
# getSystemService(CLIPBOARD_SERVICE) returns null and SDL's activity crashes
# in SDLClipboardHandler's constructor at startup. The copy of SDLActivity.java
# compiled here tolerates a null ClipboardManager (no clipboard then); with a
# clipboard (Quest, Pico) nothing changes. tools/sdl_clipboard_dex.py does the
# same to the classes.dex of the repack builds; keep the two in step.
SDL_CLIPBOARD_PATCHES = [
    ("       mClipMgr = (ClipboardManager) SDL.getContext().getSystemService(Context.CLIPBOARD_SERVICE);\n"
     "       mClipMgr.addPrimaryClipChangedListener(this);\n",
     "       mClipMgr = (ClipboardManager) SDL.getContext().getSystemService(Context.CLIPBOARD_SERVICE);\n"
     "       if (mClipMgr != null) {\n"
     "           mClipMgr.addPrimaryClipChangedListener(this);\n"
     "       }\n"),
    ("    public boolean clipboardHasText() {\n",
     "    public boolean clipboardHasText() {\n"
     "        if (mClipMgr == null) {\n"
     "            return false;\n"
     "        }\n"),
    ("    public String clipboardGetText() {\n",
     "    public String clipboardGetText() {\n"
     "        if (mClipMgr == null) {\n"
     "            return null;\n"
     "        }\n"),
    ("    public void clipboardSetText(String string) {\n",
     "    public void clipboardSetText(String string) {\n"
     "        if (mClipMgr == null) {\n"
     "            return;\n"
     "        }\n"),
]


def patch_sdl_clipboard(text):
    """SDLActivity.java with the null-clipboard guards. Stops the build if SDL's
    source no longer has the expected text (an SDL update must not silently
    drop the patch)."""
    for old, new in SDL_CLIPBOARD_PATCHES:
        n = text.count(old)
        if n != 1:
            sys.exit("SDLActivity.java: the Steam Frame clipboard patch expects %r once, found it %d times; "
                     "update SDL_CLIPBOARD_PATCHES in build_apk.py" % (old.strip().splitlines()[-1], n))
        text = text.replace(old, new)
    return text


def run(cmd, **kwargs):
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kwargs)


def align_zip(src, dst, alignment=4, so_alignment=16384):
    """zipalign equivalent: STORED entries start on `alignment` bytes
    (uncompressed .so on `so_alignment`) by padding the local header extra
    field. Entry order and compression are preserved."""
    with zipfile.ZipFile(src) as zin, open(dst, "wb") as out:
        central = []
        for info in zin.infolist():
            data = zin.read(info.filename)
            if info.compress_type == zipfile.ZIP_STORED:
                raw = data
                crc = zipfile.crc32(data) & 0xFFFFFFFF
                csize = len(data)
            else:
                # Re-compress with the same method to obtain the raw stream.
                comp = zipfile._get_compressor(info.compress_type)
                raw = comp.compress(data) + comp.flush()
                crc = zipfile.crc32(data) & 0xFFFFFFFF
                csize = len(raw)
            name = info.filename.encode("utf-8")
            offset = out.tell()
            header_len = 30 + len(name)
            extra = b""
            if info.compress_type == zipfile.ZIP_STORED:
                want = so_alignment if info.filename.endswith(".so") else alignment
                pad = (-(offset + header_len)) % want
                if pad and pad < 4:
                    pad += want  # an extra-field record needs at least 4 bytes
                if pad:
                    extra = struct.pack("<HH", 0xD935, pad - 4) + b"\0" * (pad - 4)
            dos_time, dos_date = _dos_datetime(info.date_time)
            flags = 0x800  # UTF-8 names
            out.write(struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, flags, info.compress_type, dos_time, dos_date,
                                  crc, csize, len(data), len(name), len(extra)))
            out.write(name)
            out.write(extra)
            out.write(raw)
            central.append((info, name, crc, csize, len(data), offset, dos_time, dos_date, flags))
        cd_start = out.tell()
        for info, name, crc, csize, usize, offset, dos_time, dos_date, flags in central:
            out.write(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, flags, info.compress_type, dos_time,
                                  dos_date, crc, csize, usize, len(name), 0, 0, 0, 0, info.external_attr, offset))
            out.write(name)
        cd_size = out.tell() - cd_start
        out.write(struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(central), len(central), cd_size, cd_start, 0))


def _dos_datetime(dt):
    year, month, day, hour, minute, second = dt
    year = max(year, 1980)
    return (hour << 11) | (minute << 5) | (second // 2), ((year - 1980) << 9) | (month << 5) | day


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--build-dir", required=True)
    p.add_argument("--sdl-source", required=True)
    p.add_argument("--android-jar", required=True)
    p.add_argument("--d8-jar", required=True)
    p.add_argument("--apksigner-jar", required=True)
    p.add_argument("--aapt2", required=True)
    p.add_argument("--lib-dir", required=True, help="dependency .so files (SDL3, FFmpeg, zstd, openxr_loader)")
    p.add_argument("--ndk", required=True)
    p.add_argument("--package", default="org.openxwa.quest")
    p.add_argument("--version-name", default="0.0.17-quest1")
    p.add_argument("--version-code", default="1")
    p.add_argument("--keystore", default=os.path.join(HERE, "debug.keystore"))
    p.add_argument("--strip", default=None, help="llvm-strip for the native libraries")
    p.add_argument("--out", default="OpenXWA-quest.apk")
    a = p.parse_args()

    work = tempfile.mkdtemp(prefix="openxwa-apk-")
    try:
        # 1. Manifest
        with open(os.path.join(HERE, "AndroidManifest.xml")) as f:
            manifest = (f.read().replace("@PACKAGE@", a.package).replace("@VERSION_NAME@", a.version_name)
                        .replace("@VERSION_CODE@", a.version_code))
        manifest_path = os.path.join(work, "AndroidManifest.xml")
        with open(manifest_path, "w") as f:
            f.write(manifest)

        # 2. Java: SDL's activity sources (copied, SDLActivity.java with the
        #    clipboard patch above) + OpenXwaActivity -> classes.dex
        sdl_java = os.path.join(work, "sdl-java")
        shutil.copytree(os.path.join(a.sdl_source, "android-project/app/src/main/java"), sdl_java)
        activity = os.path.join(sdl_java, "org/libsdl/app/SDLActivity.java")
        with open(activity, encoding="utf-8", newline="") as f:
            text = f.read()
        with open(activity, "w", encoding="utf-8", newline="") as f:
            f.write(patch_sdl_clipboard(text))
        java_files = []
        for root in (sdl_java, os.path.join(HERE, "java")):
            for d, _, files in os.walk(root):
                java_files += [os.path.join(d, n) for n in files if n.endswith(".java")]
        classes = os.path.join(work, "classes")
        os.makedirs(classes)
        run(["javac", "-nowarn", "-encoding", "UTF-8", "--release", "11", "-classpath", a.android_jar, "-d", classes]
            + java_files)
        class_files = []
        for d, _, files in os.walk(classes):
            class_files += [os.path.join(d, n) for n in files if n.endswith(".class")]
        dex_dir = os.path.join(work, "dex")
        os.makedirs(dex_dir)
        run(["java", "-cp", a.d8_jar, "com.android.tools.r8.D8", "--release", "--min-api", "29", "--lib",
             a.android_jar, "--output", dex_dir] + class_files)

        # 3. Resources + manifest -> base APK (aapt2)
        compiled = os.path.join(work, "res.zip")
        run([a.aapt2, "compile", "--dir", os.path.join(HERE, "res"), "-o", compiled])
        base = os.path.join(work, "base.apk")
        run([a.aapt2, "link", "-o", base, "--manifest", manifest_path, "-I", a.android_jar, compiled,
             "--min-sdk-version", "29", "--target-sdk-version", "32", "--version-code", a.version_code,
             "--version-name", a.version_name])

        # 4. Add dex, native libs and assets
        unaligned = os.path.join(work, "unaligned.apk")
        shutil.copy(base, unaligned)
        libs = [os.path.join(a.build_dir, "libmain.so")]
        for name in ("libSDL3.so", "libzstd.so", "libavcodec.so", "libavformat.so", "libavutil.so",
                     "libswresample.so", "libswscale.so", "libopenxr_loader.so"):
            libs.append(os.path.join(a.lib_dir, name))
        cxx = os.path.join(a.ndk, "toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/"
                           "libc++_shared.so")
        libs.append(cxx)
        stripped_dir = os.path.join(work, "lib")
        os.makedirs(stripped_dir)
        with zipfile.ZipFile(unaligned, "a", compression=zipfile.ZIP_DEFLATED) as z:
            z.write(os.path.join(dex_dir, "classes.dex"), "classes.dex")
            for lib in libs:
                if not os.path.exists(lib):
                    sys.exit("missing native library: " + lib)
                target = os.path.join(stripped_dir, os.path.basename(lib))
                shutil.copy(lib, target)
                if a.strip:
                    run([a.strip, "--strip-unneeded", target])
                z.write(target, "lib/arm64-v8a/" + os.path.basename(lib))
            for sub in ("shaders", "resources"):
                root = os.path.join(a.build_dir, sub)
                for d, _, files in os.walk(root):
                    for n in sorted(files):
                        full = os.path.join(d, n)
                        rel = os.path.relpath(full, a.build_dir).replace(os.sep, "/")
                        z.write(full, "assets/" + rel)

        # 5. Align, then sign (v1+v2+v3)
        aligned = os.path.join(work, "aligned.apk")
        align_zip(unaligned, aligned)
        if not os.path.exists(a.keystore):
            run(["keytool", "-genkeypair", "-keystore", a.keystore, "-storepass", "android", "-keypass", "android",
                 "-alias", "openxwa", "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000", "-dname",
                 "CN=OpenXWA Quest test build"])
        run(["java", "-jar", a.apksigner_jar, "sign", "--ks", a.keystore, "--ks-pass", "pass:android",
             "--ks-key-alias", "openxwa", "--min-sdk-version", "29", "--v1-signing-enabled", "false",
             "--v2-signing-enabled", "true", "--v3-signing-enabled", "true", "--out", a.out, aligned])
        run(["java", "-jar", a.apksigner_jar, "verify", "--min-sdk-version", "29", "-v", a.out])
        print("APK:", os.path.abspath(a.out), os.path.getsize(a.out), "bytes")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()

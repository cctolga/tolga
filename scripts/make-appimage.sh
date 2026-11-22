#!/usr/bin/env bash
# Builds Tolga Player as an AppImage for Linux x86_64, with the LGPL libraries from
# scripts/build-deps.sh inside.
#
#   scripts/build-deps.sh                 # once (takes a while)
#   scripts/make-appimage.sh              # -> dist/Tolga-Player-Linux.AppImage
#
# AirPlay comes from the closed-source module: built from ../tolga-airplay (or
# TOLGA_AIRPLAY_DIR) if that folder exists, otherwise the prebuilt library that CMake
# downloads (cmake/AirPlayPrebuilt.cmake); TOLGA_AIRPLAY_PREBUILT_FILE=<package.tar.gz> uses a
# local package instead. Without any of these the app has no AirPlay.
#
# The AppImage runs on distributions with the same or a newer glibc than the build machine:
# release builds are made on Ubuntu 22.04 (.github/workflows/linux.yml).
# Needs: cmake, ninja, patchelf, and network access the first time (appimagetool).
set -eo pipefail

[ "$(uname -s)-$(uname -m)" = Linux-x86_64 ] || { echo "make-appimage.sh runs on Linux x86_64"; exit 1; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH=x86_64
DEPS="${TOLGA_DEPS:-$ROOT/deps/linux-$ARCH}"
PREFIX="$DEPS/prefix"
BUILD="$ROOT/build-appimage"
DIST="$ROOT/dist"
APPDIR="$BUILD/AppDir"
APP_ID=app.tolga.player
VERSION="$(sed -nE 's/^project\(Tolga VERSION ([0-9.]+).*/\1/p' "$ROOT/CMakeLists.txt")"
TOOLS="$ROOT/deps/tools"
AIRPLAY_DIR="${TOLGA_AIRPLAY_DIR:-$ROOT/../tolga-airplay}"
if [ -f "$AIRPLAY_DIR/CMakeLists.txt" ]; then
    AIRPLAY_DIR="$(cd "$AIRPLAY_DIR" && pwd)"
else
    AIRPLAY_DIR=""
fi
# AppImageUpdate / Gear Lever find new versions through this
UPDATE_INFO="${TOLGA_UPDATE_INFO:-gh-releases-zsync|cctolga|tolga|latest|Tolga-Player-Linux.AppImage.zsync}"

log() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\033[31merror:\033[0m %s\n' "$*"; exit 1; }

for tool in cmake patchelf; do command -v "$tool" >/dev/null || die "missing: $tool"; done
[ -f "$PREFIX/lib/libmpv.so" ] || die "LGPL libraries not found in $PREFIX - run scripts/build-deps.sh first"
[ -x "$PREFIX/bin/ffmpeg" ] || die "$PREFIX/bin/ffmpeg missing - run scripts/build-deps.sh first"
[ -f "$PREFIX/lib/libcrypto.a" ] && [ -f "$PREFIX/lib/libcurl.a" ] || die "static OpenSSL/curl missing - run scripts/build-deps.sh again"

# ---------------------------------------------------------------- 0. folder moved?
moved=""
for pc in "$PREFIX"/lib/pkgconfig/*.pc "$PREFIX"/share/pkgconfig/*.pc "$PREFIX"/lib/cmake/*/*.cmake; do
    [ -f "$pc" ] || continue
    if grep -Eo '/[^ :"=;]*/deps/linux-[A-Za-z0-9_]+/prefix' "$pc" | grep -qvx "$PREFIX"; then
        sed -E -i "s#/[^ :\"=;]*/deps/linux-[A-Za-z0-9_]+/prefix#$PREFIX#g" "$pc"
        moved=1
    fi
done
if [ -n "$moved" ]; then
    echo "project folder moved: library paths updated, starting a clean build"
    rm -rf "$BUILD/cmake"
fi

# ---------------------------------------------------------------- 1. compile
log "build Tolga Player $VERSION (linux-$ARCH)"
GEN=(); command -v ninja >/dev/null && GEN=(-G Ninja)
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
cmake -S "$ROOT" -B "$BUILD/cmake" ${GEN[@]+"${GEN[@]}"} \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DTOLGA_SYSTEM_SDL=OFF \
    -DSDL_X11_XTEST=OFF \
    -DOPENSSL_ROOT_DIR="$PREFIX" -DOPENSSL_USE_STATIC_LIBS=TRUE \
    -DCURL_NO_CURL_CMAKE=OFF -DCURL_DIR="$PREFIX/lib/cmake/CURL" \
    -DTOLGA_AIRPLAY_DIR="$AIRPLAY_DIR" \
    -DTOLGA_AIRPLAY_PREBUILT_FILE="${TOLGA_AIRPLAY_PREBUILT_FILE:-}" \
    ${TOLGA_BUY_URL:+-DTOLGA_BUY_URL="$TOLGA_BUY_URL"}
cmake --build "$BUILD/cmake" --target tolga -j"$(nproc)"
BIN="$BUILD/cmake/tolga"
readelf -d "$BIN" | grep -q 'libcurl' && die "the app links a shared libcurl (expected the static one from $PREFIX)"

PREBUILT_LIB=""
if [ -z "$AIRPLAY_DIR" ] && readelf -d "$BIN" | grep -q 'libtolga_airplay\.so'; then
    PREBUILT_LIB="$(sed -n 's/^TOLGA_AIRPLAY_PREBUILT_DIR:INTERNAL=//p' "$BUILD/cmake/CMakeCache.txt")/lib"
    [ -f "$PREBUILT_LIB/libtolga_airplay.so" ] || die "the prebuilt AirPlay library is missing ($PREBUILT_LIB)"
fi
HAS_AIRPLAY=""; [ -n "$AIRPLAY_DIR$PREBUILT_LIB" ] && HAS_AIRPLAY=1
[ -n "$TOLGA_REQUIRE_AIRPLAY" ] && [ -z "$HAS_AIRPLAY" ] &&
    die "no AirPlay module (TOLGA_REQUIRE_AIRPLAY): publish the Linux module first (tolga-airplay: Prebuilt Linux module)"
if [ -n "$PREBUILT_LIB" ]; then
    echo "AirPlay from the prebuilt module (14-day trial, then a license)"
elif [ -z "$AIRPLAY_DIR" ]; then
    echo "built without AirPlay (no ../tolga-airplay and no prebuilt module)"
elif [ -f "$AIRPLAY_DIR/license-public.key" ]; then
    echo "official build with AirPlay: a license is needed after the 14-day trial"
else
    echo "AirPlay unlocked: no license-public.key in $AIRPLAY_DIR (do not publish this build)"
fi

# ---------------------------------------------------------------- 2. AppDir layout
log "assemble AppDir"
rm -rf "$APPDIR"
U="$APPDIR/usr"
SHARE="$U/share/tolga-player"
mkdir -p "$U/bin" "$U/lib" "$SHARE/Licenses" "$U/share/applications" "$U/share/metainfo"
cp "$BIN" "$U/bin/tolga-player"
cp "$PREFIX/bin/ffmpeg" "$PREFIX/bin/ffprobe" "$U/bin/"   # used for AirPlay HLS + seek previews
strip --strip-unneeded "$U/bin/"*
cp "$ROOT/resources/linux/$APP_ID.desktop" "$U/share/applications/"
sed -e "s/@VERSION@/$VERSION/" -e "s/@DATE@/$(date +%Y-%m-%d)/" \
    "$ROOT/resources/linux/$APP_ID.metainfo.xml.in" > "$U/share/metainfo/$APP_ID.metainfo.xml"
for s in 32 48 64 128 256 512; do
    mkdir -p "$U/share/icons/hicolor/${s}x${s}/apps"
    cp "$ROOT/resources/linux/icon_$s.png" "$U/share/icons/hicolor/${s}x${s}/apps/$APP_ID.png"
done
mkdir -p "$U/share/icons/hicolor/scalable/apps"
cp "$ROOT/resources/linux/$APP_ID.svg" "$U/share/icons/hicolor/scalable/apps/"
# AppImage root: desktop file, icon and the start script
cp "$ROOT/resources/linux/$APP_ID.desktop" "$APPDIR/"
cp "$ROOT/resources/linux/icon_256.png" "$APPDIR/$APP_ID.png"
ln -s "$APP_ID.png" "$APPDIR/.DirIcon"
cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
# VA-API drivers come from the system; their folder differs between distributions
if [ -z "$LIBVA_DRIVERS_PATH" ]; then
    LIBVA_DRIVERS_PATH="/usr/lib/x86_64-linux-gnu/dri:/usr/lib64/dri:/usr/lib/dri:/usr/lib/x86_64-linux-gnu/dri-nonfree"
    export LIBVA_DRIVERS_PATH
fi
exec "$HERE/usr/bin/tolga-player" "$@"
EOF
chmod +x "$APPDIR/AppRun"

# ---------------------------------------------------------------- 3. libraries
# Everything a binary needs goes into usr/lib, except what has to match the user's system
# (C/C++ runtime, graphics drivers, display server, sound server). Those are on every desktop.
is_system() {
    case "$1" in
        ld-linux*|libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*|libutil.so*) return 0 ;;
        libstdc++.so*|libgcc_s.so*) return 0 ;;
        libGL.so*|libGLX*|libEGL.so*|libGLdispatch*|libOpenGL*|libGLESv2*|libdrm.so*|libgbm.so*) return 0 ;;
        libX11.so*|libX11-xcb*|libxcb*|libXext*|libXrandr*|libXrender*|libXfixes*|libXss*|libXpresent*) return 0 ;;
        libXinerama*|libXi.so*|libXcursor*|libXau*|libXdmcp*|libxkbcommon*|libwayland-*) return 0 ;;
        libasound.so*|libpulse*|libfontconfig.so*|libexpat.so*|libz.so*|libdbus-1*|libsystemd*|libudev*) return 0 ;;
        *) return 1 ;;
    esac
}

find_lib() {  # find_lib <soname> -> path: our prefix first, then the AirPlay module, then the system
    local name="$1" d
    for d in "$PREFIX/lib" ${PREBUILT_LIB:+"$PREBUILT_LIB"}; do
        [ -e "$d/$name" ] && { readlink -f "$d/$name"; return; }
    done
    ldconfig -p | awk -v n="$name" '$1 == n && /x86-64/ {print $NF; exit}' | grep . && return
    return 1
}

queue=("$U/bin/tolga-player" "$U/bin/ffmpeg" "$U/bin/ffprobe")
while [ ${#queue[@]} -gt 0 ]; do
    f="${queue[0]}"; queue=("${queue[@]:1}")
    for name in $(readelf -d "$f" | sed -nE 's/.*\(NEEDED\).*\[(.*)\]/\1/p'); do
        is_system "$name" && continue
        [ -f "$U/lib/$name" ] && continue
        src="$(find_lib "$name")" || die "$(basename "$f") needs $name, which was not found"
        cp "$src" "$U/lib/$name"
        chmod u+w "$U/lib/$name"
        queue+=("$U/lib/$name")
    done
done
# GPL libraries must not be shipped next to the closed AirPlay module
for l in "$U/lib/"*; do
    case "$(basename "$l")" in libpostproc*|libx264*|libx265*|libxvid*|librubberband*|libfdk*) die "$(basename "$l") is GPL/nonfree";; esac
done
strip --strip-unneeded "$U/lib/"*.so* 2>/dev/null || true
patchelf --set-rpath '$ORIGIN/../lib' "$U/bin/tolga-player" "$U/bin/ffmpeg" "$U/bin/ffprobe"
for l in "$U/lib/"*; do patchelf --set-rpath '$ORIGIN' "$l"; done

# verify: every library that is needed is bundled or comes from the system
for f in "$U/bin/"* "$U/lib/"*; do
    for name in $(readelf -d "$f" | sed -nE 's/.*\(NEEDED\).*\[(.*)\]/\1/p'); do
        is_system "$name" || [ -f "$U/lib/$name" ] || die "$(basename "$f") needs $name, which is not bundled"
    done
done
echo "bundled libraries:"; ls "$U/lib" | sed 's/^/  /'
echo "newest glibc symbol: $(objdump -T "$U/bin/"* "$U/lib/"* 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)"

# ---------------------------------------------------------------- 4. license notices
log "license notices"
LIC="$SHARE/Licenses"
cp -R "$DEPS/licenses/." "$LIC/"
mkdir -p "$LIC/Tolga" && cp "$ROOT/LICENSE" "$ROOT/NOTICE" "$LIC/Tolga/"
D="$BUILD/cmake/_deps"
mkdir -p "$LIC/SDL3" "$LIC/imgui" "$LIC/nlohmann_json"
cp "$D/sdl3-src/LICENSE.txt"   "$LIC/SDL3/"          2>/dev/null || die "SDL3 license not found (was SDL fetched?)"
cp "$D/imgui-src/LICENSE.txt"  "$LIC/imgui/"
cp "$D/json-src/LICENSE.MIT"   "$LIC/nlohmann_json/"
if [ -n "$AIRPLAY_DIR" ]; then
    mkdir -p "$LIC/mdns" && cp "$D/mdns-src/LICENSE" "$LIC/mdns/"
elif [ -n "$PREBUILT_LIB" ]; then
    mkdir -p "$LIC/mdns" && cp "$PREBUILT_LIB/../licenses/mdns-LICENSE" "$LIC/mdns/LICENSE"
fi
# other libraries copied from the build machine (e.g. libva, libunibreak): keep their licenses too
for l in "$U/lib/"*; do
    n="$(basename "$l")"
    [ -e "$PREFIX/lib/$n" ] && continue
    [ "$n" = libtolga_airplay.so ] && continue
    pkg="$(dpkg -S "$(find_lib "$n")" 2>/dev/null | head -1 | cut -d: -f1)" || true
    [ -n "$pkg" ] && [ -f "/usr/share/doc/$pkg/copyright" ] && { mkdir -p "$LIC/$pkg"; cp "/usr/share/doc/$pkg/copyright" "$LIC/$pkg/"; }
done
NOTICES="$SHARE/THIRD_PARTY_NOTICES.txt"
AIRPLAY_NOTE=""
[ -n "$HAS_AIRPLAY" ] && AIRPLAY_NOTE="
The AirPlay module of the official app is proprietary software, (c) $(date +%Y) Tolga Ç.. It is not covered by the Apache License; its source code is not published."
{
cat <<EOF
Tolga Player $VERSION

Tolga Player is open source under the Apache License, Version 2.0
(Licenses/Tolga/LICENSE and Licenses/Tolga/NOTICE).
Source code: ${TOLGA_SOURCE_URL:-https://github.com/cctolga/tolga}
${AIRPLAY_NOTE}

Third-party software

Tolga Player uses the open-source libraries listed below. Their license texts are in
usr/share/tolga-player/Licenses/ inside the AppImage (run it with --appimage-extract to see them).

LGPL components (libmpv, FFmpeg, libplacebo, fribidi$( [ -d "$LIC/uchardet" ] && echo ", uchardet")):
  These libraries are licensed under the GNU Lesser General Public License v2.1 or later.
  They are included unmodified, as separate shared libraries in usr/lib (and
  the ffmpeg/ffprobe programs in usr/bin). You may replace them with your own builds of the
  same libraries: extract the AppImage (--appimage-extract), replace the files in
  squashfs-root/usr/lib and start squashfs-root/AppRun.
  The source code of these libraries is available from the addresses below; the exact build
  configuration is in Licenses/build-deps.sh. On request we also provide the source code on a
  data medium, or by download, for at least three years from the date you received Tolga Player.
  FFmpeg is built without --enable-gpl and --enable-nonfree; libmpv is built with -Dgpl=false.

Components and versions:
EOF
sed 's/^/  /' "$DEPS/versions.txt"
cat <<EOF
  SDL3                 zlib License   https://libsdl.org
  Dear ImGui           MIT            https://github.com/ocornut/imgui
  Material Symbols     Apache-2.0     https://github.com/google/material-design-icons (the cast icon)
  PP-OCRv5 models      Apache-2.0     https://github.com/PaddlePaddle/PaddleOCR (downloaded when picture
                                      subtitles are first converted to text)
  nlohmann/json        MIT            https://github.com/nlohmann/json
EOF
if [ -n "$HAS_AIRPLAY" ]; then
cat <<EOF
  mdns                 Public domain  https://github.com/mjansson/mdns
EOF
fi
} > "$NOTICES"

# ---------------------------------------------------------------- 5. smoke test
log "smoke test"
"$U/bin/ffmpeg" -hide_banner -version | head -1
if "$U/bin/ffmpeg" -hide_banner -buildconf | grep -Eq -- '--enable-(gpl|nonfree)'; then
    die "bundled ffmpeg is a GPL build"
fi
# every bundled library loads with the bundled set (catches a missing or mismatched one)
LD_LIBRARY_PATH="$U/lib" ldd "$U/bin/tolga-player" | grep -q 'not found' && { ldd "$U/bin/tolga-player" | grep 'not found'; die "unresolved libraries"; }

# ---------------------------------------------------------------- 6. AppImage
mkdir -p "$TOOLS"
if [ ! -x "$TOOLS/appimagetool" ]; then
    log "download appimagetool"
    curl -fL --retry 3 -o "$TOOLS/appimagetool" \
        https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$TOOLS/appimagetool"
fi
if [ ! -f "$TOOLS/runtime-x86_64" ]; then
    curl -fL --retry 3 -o "$TOOLS/runtime-x86_64" \
        https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
fi
OUTFILE="$DIST/Tolga-Player-Linux.AppImage"   # same name every version: releases/latest/download/... stays valid
mkdir -p "$DIST"
rm -f "$OUTFILE" "$OUTFILE.zsync"
log "AppImage $OUTFILE"
# --appimage-extract-and-run: works without FUSE (containers, CI)
(cd "$DIST" && ARCH=$ARCH "$TOOLS/appimagetool" --appimage-extract-and-run --no-appstream \
    --runtime-file "$TOOLS/runtime-x86_64" -u "$UPDATE_INFO" "$APPDIR" "$OUTFILE")
chmod +x "$OUTFILE"

log "done"
echo "  AppImage: $OUTFILE  ($(du -h "$OUTFILE" | cut -f1))"

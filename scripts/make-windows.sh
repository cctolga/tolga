#!/usr/bin/env bash
# Builds Tolga Player for Windows (x86_64) from Linux, with the MinGW cross compiler and the
# LGPL libraries from scripts/build-deps.sh, and packs it:
#   dist/Tolga-Player-Win.exe   installer (per user, no administrator rights needed)
#   dist/Tolga-Player-Win.zip   the same files to unpack anywhere
#
#   apt install g++-mingw-w64-x86-64-posix mingw-w64-tools nsis meson ninja-build nasm cmake
#   TOLGA_TARGET=windows scripts/build-deps.sh      # once (takes a while)
#   scripts/make-windows.sh
#
# AirPlay comes from the closed-source module: built from ../tolga-airplay (or
# TOLGA_AIRPLAY_DIR) if that folder exists, otherwise the prebuilt library that CMake
# downloads (cmake/AirPlayPrebuilt.cmake); TOLGA_AIRPLAY_PREBUILT_FILE=<package.tar.gz> uses a
# local package instead. Without any of these the app has no AirPlay.
# Release builds are made by .github/workflows/windows.yml.
set -eo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CROSS=x86_64-w64-mingw32
DEPS="${TOLGA_DEPS:-$ROOT/deps/windows-x86_64}"
PREFIX="$DEPS/prefix"
BUILD="$ROOT/build-windows"
DIST="$ROOT/dist"
STAGE="$BUILD/Tolga Player"
VERSION="$(sed -nE 's/^project\(Tolga VERSION ([0-9.]+).*/\1/p' "$ROOT/CMakeLists.txt")"
AIRPLAY_DIR="${TOLGA_AIRPLAY_DIR:-$ROOT/../tolga-airplay}"
if [ -f "$AIRPLAY_DIR/CMakeLists.txt" ]; then
    AIRPLAY_DIR="$(cd "$AIRPLAY_DIR" && pwd)"
else
    AIRPLAY_DIR=""
fi

log() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\033[31merror:\033[0m %s\n' "$*"; exit 1; }

for tool in cmake "$CROSS-objdump" "$CROSS-strip"; do command -v "$tool" >/dev/null || die "missing: $tool"; done
[ -f "$PREFIX/lib/libmpv.dll.a" ] || die "Windows libraries not found in $PREFIX - run TOLGA_TARGET=windows scripts/build-deps.sh first"
[ -f "$PREFIX/bin/ffmpeg.exe" ] || die "$PREFIX/bin/ffmpeg.exe missing - run TOLGA_TARGET=windows scripts/build-deps.sh first"
[ -f "$DEPS/cross.cmake" ] || die "$DEPS/cross.cmake missing - run TOLGA_TARGET=windows scripts/build-deps.sh again"

# ---------------------------------------------------------------- 1. compile
log "build Tolga Player $VERSION (windows-x86_64)"
GEN=(); command -v ninja >/dev/null && GEN=(-G Ninja)
PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH="" \
cmake -S "$ROOT" -B "$BUILD/cmake" ${GEN[@]+"${GEN[@]}"} \
    -DCMAKE_TOOLCHAIN_FILE="$DEPS/cross.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DTOLGA_SYSTEM_SDL=OFF \
    -DOPENSSL_ROOT_DIR="$PREFIX" -DOPENSSL_USE_STATIC_LIBS=TRUE \
    -DCURL_DIR="$PREFIX/lib/cmake/CURL" \
    -DTOLGA_AIRPLAY_DIR="$AIRPLAY_DIR" \
    -DTOLGA_AIRPLAY_PREBUILT_FILE="${TOLGA_AIRPLAY_PREBUILT_FILE:-}" \
    ${TOLGA_BUY_URL:+-DTOLGA_BUY_URL="$TOLGA_BUY_URL"}
PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" cmake --build "$BUILD/cmake" --target tolga -j"$(nproc)"
EXE="$BUILD/cmake/Tolga Player.exe"
[ -f "$EXE" ] || die "$EXE was not built"
IMPORTS="$("$CROSS-objdump" -p "$EXE" | sed -n 's/^\s*DLL Name: //p')"   # (not piped into grep -q: pipefail)
echo "$IMPORTS" | grep -qi '^libcurl' && die "the app links a shared libcurl (expected the static one)"

PREBUILT_DLL=""
if [ -z "$AIRPLAY_DIR" ] && echo "$IMPORTS" | grep -qix 'tolga_airplay.dll'; then
    PREBUILT_DLL="$(sed -n 's/^TOLGA_AIRPLAY_PREBUILT_DIR:INTERNAL=//p' "$BUILD/cmake/CMakeCache.txt")/lib/tolga_airplay.dll"
    [ -f "$PREBUILT_DLL" ] || die "the prebuilt AirPlay library is missing ($PREBUILT_DLL)"
fi
HAS_AIRPLAY=""; [ -n "$AIRPLAY_DIR$PREBUILT_DLL" ] && HAS_AIRPLAY=1
[ -n "$TOLGA_REQUIRE_AIRPLAY" ] && [ -z "$HAS_AIRPLAY" ] &&
    die "no AirPlay module (TOLGA_REQUIRE_AIRPLAY): publish the Windows module first"
if [ -n "$PREBUILT_DLL" ]; then
    echo "AirPlay from the prebuilt module (14-day trial, then a license)"
elif [ -z "$AIRPLAY_DIR" ]; then
    echo "built without AirPlay (no ../tolga-airplay and no prebuilt module)"
elif [ -f "$AIRPLAY_DIR/license-public.key" ]; then
    echo "official build with AirPlay: a license is needed after the 14-day trial"
else
    echo "AirPlay unlocked: no license-public.key in $AIRPLAY_DIR (do not publish this build)"
fi

# ---------------------------------------------------------------- 2. folder
log "assemble the program folder"
rm -rf "$STAGE"
mkdir -p "$STAGE/Licenses"
cp "$EXE" "$STAGE/"
cp "$PREFIX/bin/ffmpeg.exe" "$PREFIX/bin/ffprobe.exe" "$STAGE/"
if [ -n "$PREBUILT_DLL" ]; then cp "$PREBUILT_DLL" "$STAGE/"; fi

# every DLL the programs need that Windows itself doesn't have: from the LGPL prefix, else the
# MinGW runtime (winpthread, libgcc, libstdc++)
SYSROOT="$(dirname "$("$CROSS-gcc-posix" -print-libgcc-file-name 2>/dev/null || "$CROSS-gcc" -print-libgcc-file-name)")"
find_dll() {  # find_dll <name> -> path or nothing
    local n="$1" d
    for d in "$PREFIX/bin" "$PREFIX/lib" "$SYSROOT" "/usr/$CROSS/lib" "/usr/$CROSS/bin" /usr/lib/gcc/$CROSS/*-posix; do
        for f in "$d/$n" "$d/${n,,}"; do [ -f "$f" ] && { echo "$f"; return; }; done
        local hit; hit="$(find "$d" -maxdepth 1 -iname "$n" 2>/dev/null | head -1)"
        [ -n "$hit" ] && { echo "$hit"; return; }
    done
    return 0
}
queue=("$STAGE/"*.exe "$STAGE/"*.dll)
while [ ${#queue[@]} -gt 0 ]; do
    f="${queue[0]}"; queue=("${queue[@]:1}")
    [ -f "$f" ] || continue
    while read -r dll; do
        [ -e "$STAGE/$dll" ] && continue
        src="$(find_dll "$dll")"
        [ -n "$src" ] || continue        # a Windows system DLL
        cp "$src" "$STAGE/$dll"
        queue+=("$STAGE/$dll")
    done < <("$CROSS-objdump" -p "$f" | sed -n 's/^\s*DLL Name: //p')
done
"$CROSS-strip" --strip-unneeded "$STAGE/"*.exe "$STAGE/"*.dll 2>/dev/null || true
echo "program folder:"; ls "$STAGE" | sed 's/^/  /'

# what's still imported must come with Windows
missing=""
for f in "$STAGE/"*.exe "$STAGE/"*.dll; do
    while read -r dll; do
        [ -e "$STAGE/$dll" ] && continue
        case "${dll,,}" in
            kernel32.dll|user32.dll|gdi32.dll|advapi32.dll|shell32.dll|ole32.dll|oleaut32.dll|ws2_32.dll|\
            iphlpapi.dll|crypt32.dll|secur32.dll|bcrypt.dll|ncrypt.dll|winmm.dll|imm32.dll|version.dll|\
            setupapi.dll|cfgmgr32.dll|opengl32.dll|dwmapi.dll|uxtheme.dll|shlwapi.dll|comdlg32.dll|\
            msvcrt.dll|ucrtbase.dll|api-ms-win-*|ntdll.dll|d3d11.dll|d3d9.dll|dxgi.dll|dxva2.dll|\
            mfplat.dll|mf.dll|mfuuid.dll|avrt.dll|ksuser.dll|propsys.dll|dwrite.dll|normaliz.dll|\
            hid.dll|dinput8.dll|xinput1_4.dll|wldap32.dll|psapi.dll|userenv.dll|wtsapi32.dll|\
            powrprof.dll|comctl32.dll|rpcrt4.dll|mmdevapi.dll|evr.dll|d3dcompiler_47.dll|gdiplus.dll|\
            dbghelp.dll|winhttp.dll|wininet.dll|urlmon.dll|dnsapi.dll|sensapi.dll|shcore.dll|\
            mfreadwrite.dll|winspool.drv|mpr.dll|netapi32.dll|wtsapi32.dll|d2d1.dll|windowscodecs.dll) ;;
            *) missing="$missing $dll($(basename "$f"))" ;;
        esac
    done < <("$CROSS-objdump" -p "$f" | sed -n 's/^\s*DLL Name: //p')
done
[ -z "$missing" ] || die "DLLs that are neither bundled nor part of Windows:$missing"

# ---------------------------------------------------------------- 3. license notices
log "license notices"
LIC="$STAGE/Licenses"
cp -R "$DEPS/licenses/." "$LIC/"
mkdir -p "$LIC/Tolga" && cp "$ROOT/LICENSE" "$ROOT/NOTICE" "$LIC/Tolga/"
D="$BUILD/cmake/_deps"
mkdir -p "$LIC/SDL3" "$LIC/imgui" "$LIC/nlohmann_json" "$LIC/mingw-w64"
cp "$D/sdl3-src/LICENSE.txt"   "$LIC/SDL3/"          2>/dev/null || die "SDL3 license not found (was SDL fetched?)"
cp "$D/imgui-src/LICENSE.txt"  "$LIC/imgui/"
cp "$D/json-src/LICENSE.MIT"   "$LIC/nlohmann_json/"
for c in /usr/share/doc/mingw-w64-common/copyright /usr/share/doc/gcc-mingw-w64-base/copyright; do
    [ -f "$c" ] && cp "$c" "$LIC/mingw-w64/$(basename "$(dirname "$c")").txt"
done
if [ -n "$AIRPLAY_DIR" ]; then
    mkdir -p "$LIC/mdns" && cp "$D/mdns-src/LICENSE" "$LIC/mdns/"
elif [ -n "$PREBUILT_DLL" ]; then
    mkdir -p "$LIC/mdns" && cp "$(dirname "$PREBUILT_DLL")/../licenses/mdns-LICENSE" "$LIC/mdns/LICENSE"
fi
AIRPLAY_NOTE=""
[ -n "$HAS_AIRPLAY" ] && AIRPLAY_NOTE="
The AirPlay module of the official app is proprietary software, (c) $(date +%Y) Tolga Ç.. It is not covered by the Apache License; its source code is not published."
{
cat <<EOF
Tolga Player $VERSION

Tolga Player is open source under the Apache License, Version 2.0
(Licenses\\Tolga\\LICENSE and Licenses\\Tolga\\NOTICE).
Source code: ${TOLGA_SOURCE_URL:-https://github.com/cctolga/tolga}
${AIRPLAY_NOTE}

Third-party software

Tolga Player uses the open-source libraries listed below. Their license texts are in the
Licenses folder next to "Tolga Player.exe".

LGPL components (libmpv, FFmpeg, libplacebo, fribidi$( [ -d "$LIC/uchardet" ] && echo ", uchardet")):
  These libraries are licensed under the GNU Lesser General Public License v2.1 or later.
  They are included unmodified, as separate DLLs next to "Tolga Player.exe" (and the
  ffmpeg.exe/ffprobe.exe programs). You may replace them with your own builds of the same
  libraries by replacing those files.
  The source code of these libraries is available from the addresses below; the exact build
  configuration is in Licenses\\build-deps.sh. On request we also provide the source code on a
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
  MinGW-w64 runtime    ZPL / MIT      https://www.mingw-w64.org
EOF
if [ -n "$HAS_AIRPLAY" ]; then
cat <<EOF
  mdns                 Public domain  https://github.com/mjansson/mdns
EOF
fi
} | sed 's/$/\r/' > "$STAGE/THIRD_PARTY_NOTICES.txt"   # Windows line endings for Notepad

# ---------------------------------------------------------------- 4. packages
log "zip and installer"
mkdir -p "$DIST"
rm -f "$DIST/Tolga-Player-Win.zip" "$DIST/Tolga-Player-Win.exe"
(cd "$BUILD" && zip -qr9 "$DIST/Tolga-Player-Win.zip" "Tolga Player")
if command -v makensis >/dev/null; then
    makensis -V2 -DVERSION="$VERSION" -DSTAGE="$STAGE" -DOUTFILE="$DIST/Tolga-Player-Win.exe" \
        -DICON="$ROOT/resources/windows/tolga.ico" "$ROOT/resources/windows/installer.nsi"
    echo "installer: $DIST/Tolga-Player-Win.exe ($(du -h "$DIST/Tolga-Player-Win.exe" | cut -f1))"
else
    echo "makensis not found (apt install nsis): made only the zip"
fi
echo "zip:       $DIST/Tolga-Player-Win.zip ($(du -h "$DIST/Tolga-Player-Win.zip" | cut -f1))"

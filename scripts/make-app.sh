#!/usr/bin/env bash
# Builds Tolga.app and a .dmg for macOS, with the LGPL libraries from scripts/build-deps.sh inside.
#
#   scripts/build-deps.sh                 # once (takes a while)
#   scripts/make-app.sh                   # -> dist/Tolga.app, dist/Tolga-<version>-<arch>.dmg
#
# AirPlay comes from the closed-source module: built from ../tolga-airplay (or
# TOLGA_AIRPLAY_DIR) if that folder exists, otherwise the prebuilt library that CMake
# downloads (cmake/AirPlayPrebuilt.cmake). Without either the app has no AirPlay.
#
# Signing / notarization (optional, needs an Apple Developer account):
#   TOLGA_SIGN_ID="Developer ID Application: Your Name (TEAMID)" \
#   TOLGA_NOTARY_PROFILE=tolga-notary scripts/make-app.sh
# Create the notary profile once with:
#   xcrun notarytool store-credentials tolga-notary --apple-id you@example.com --team-id TEAMID
# Without TOLGA_SIGN_ID the app is signed ad hoc: it runs on this Mac, but other Macs
# will refuse it (Gatekeeper) until it is signed with a Developer ID and notarized.
set -eo pipefail   # (no -u: macOS bash 3.2 treats empty arrays as unset)

[ "$(uname -s)" = Darwin ] || { echo "make-app.sh runs on macOS"; exit 1; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="$(uname -m)"
DEPS="${TOLGA_DEPS:-$ROOT/deps/darwin-$ARCH}"
PREFIX="$DEPS/prefix"
BUILD="$ROOT/build-release"
DIST="$ROOT/dist"
APP="$DIST/Tolga.app"
BUNDLE_ID="${TOLGA_BUNDLE_ID:-app.tolga.player}"
SIGN_ID="${TOLGA_SIGN_ID:-}"
NOTARY="${TOLGA_NOTARY_PROFILE:-}"
MIN_MACOS="${MACOSX_DEPLOYMENT_TARGET:-12.0}"
VERSION="$(sed -nE 's/^project\(Tolga VERSION ([0-9.]+).*/\1/p' "$ROOT/CMakeLists.txt")"
BUILD_NO="${TOLGA_BUILD_NUMBER:-$(date +%Y%m%d%H%M)}"
AIRPLAY_DIR="${TOLGA_AIRPLAY_DIR:-$ROOT/../tolga-airplay}"
if [ -f "$AIRPLAY_DIR/CMakeLists.txt" ]; then
    AIRPLAY_DIR="$(cd "$AIRPLAY_DIR" && pwd)"
else
    AIRPLAY_DIR=""
fi

log() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\033[31merror:\033[0m %s\n' "$*"; exit 1; }

[ -f "$PREFIX/lib/libmpv.dylib" ] || die "LGPL libraries not found in $PREFIX - run scripts/build-deps.sh first"
[ -x "$PREFIX/bin/ffmpeg" ] || die "$PREFIX/bin/ffmpeg missing - run scripts/build-deps.sh first"
[ -f "$PREFIX/lib/libcrypto.a" ] || die "static OpenSSL missing - run scripts/build-deps.sh again"

# ---------------------------------------------------------------- 0. folder moved?
# pkg-config files remember where build-deps.sh installed the libraries; point them here
moved=""
for pc in "$PREFIX"/lib/pkgconfig/*.pc "$PREFIX"/share/pkgconfig/*.pc; do
    [ -f "$pc" ] || continue
    if grep -Eo '/[^ :"=]*/deps/[A-Za-z0-9_-]+/prefix' "$pc" | grep -qvx "$PREFIX"; then
        sed -E -i '' "s#/[^ :\"=]*/deps/[A-Za-z0-9_-]+/prefix#$PREFIX#g" "$pc"
        moved=1
    fi
done
if [ -n "$moved" ]; then
    echo "project folder moved: library paths updated, starting a clean build"
    rm -rf "$BUILD"
fi

# ---------------------------------------------------------------- 1. compile
log "build Tolga $VERSION ($ARCH, macOS $MIN_MACOS+)"
SDK="$(xcrun --show-sdk-path)"
GEN=(); command -v ninja >/dev/null && GEN=(-G Ninja)
PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH="" \
cmake -S "$ROOT" -B "$BUILD" ${GEN[@]+"${GEN[@]}"} \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_MACOS" \
    -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,-headerpad_max_install_names" \
    -DTOLGA_SYSTEM_SDL=OFF \
    -DOPENSSL_ROOT_DIR="$PREFIX" -DOPENSSL_USE_STATIC_LIBS=TRUE \
    -DTOLGA_BUY_URL="${TOLGA_BUY_URL:-}" \
    -DTOLGA_AIRPLAY_DIR="$AIRPLAY_DIR" \
    -DCURL_NO_CURL_CMAKE=ON -DCURL_INCLUDE_DIR="$SDK/usr/include" -DCURL_LIBRARY="$SDK/usr/lib/libcurl.tbd"
cmake --build "$BUILD" --target tolga -j"$(sysctl -n hw.ncpu)"
PREBUILT_LIB=""
if [ -z "$AIRPLAY_DIR" ] && otool -L "$BUILD/tolga" | grep -q 'libtolga_airplay\.dylib'; then
    PREBUILT_LIB="$(sed -n 's/^TOLGA_AIRPLAY_PREBUILT_DIR:INTERNAL=//p' "$BUILD/CMakeCache.txt")/lib"
    [ -f "$PREBUILT_LIB/libtolga_airplay.dylib" ] || die "the prebuilt AirPlay library is missing ($PREBUILT_LIB)"
fi
HAS_AIRPLAY=""; [ -n "$AIRPLAY_DIR$PREBUILT_LIB" ] && HAS_AIRPLAY=1
if [ -n "$PREBUILT_LIB" ]; then
    echo "AirPlay from the prebuilt module (14-day trial, then a license)"
elif [ -z "$AIRPLAY_DIR" ]; then
    echo "built without AirPlay (no ../tolga-airplay and no prebuilt module)"
elif [ -f "$AIRPLAY_DIR/license-public.key" ]; then
    echo "official build with AirPlay: a license is needed after the 14-day trial"
else
    echo "AirPlay unlocked: no license-public.key in $AIRPLAY_DIR (do not publish this build)"
fi

# ---------------------------------------------------------------- 2. bundle layout
log "assemble $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources/Licenses"
cp "$BUILD/tolga" "$APP/Contents/MacOS/Tolga"
cp "$PREFIX/bin/ffmpeg" "$PREFIX/bin/ffprobe" "$APP/Contents/MacOS/"   # used for AirPlay HLS + seek previews
cp "$ROOT/resources/macos/Tolga.icns" "$APP/Contents/Resources/"
sed -e "s/@BUNDLE_ID@/$BUNDLE_ID/" -e "s/@VERSION@/$VERSION/" -e "s/@BUILD@/$BUILD_NO/" \
    -e "s/@MIN_MACOS@/$MIN_MACOS/" -e "s/@YEAR@/$(date +%Y)/" \
    "$ROOT/resources/macos/Info.plist.in" > "$APP/Contents/Info.plist"
plutil -lint "$APP/Contents/Info.plist" >/dev/null
printf 'APPL????' > "$APP/Contents/PkgInfo"

# ---------------------------------------------------------------- 3. copy dylibs, rewrite load paths
# Every non-system library a binary loads is copied into Contents/Frameworks and referenced
# as @rpath/<name>; executables look in ../Frameworks, libraries next to themselves.
FW="$APP/Contents/Frameworks"
is_system() { case "$1" in /usr/lib/*|/System/*) return 0 ;; *) return 1 ;; esac; }

resolve() {  # resolve <reference> <file that has it> -> path on this machine
    local ref="$1" from="$2"
    case "$ref" in
        @rpath/*|@loader_path/*|@executable_path/*)
            local name="${ref##*/}"
            for d in "$PREFIX/lib" "$(dirname "$from")" ${PREBUILT_LIB:+"$PREBUILT_LIB"}; do
                [ -f "$d/$name" ] && { echo "$d/$name"; return; }
            done ;;
        *)  [ -f "$ref" ] && { echo "$ref"; return; }
            # the project folder was moved after build-deps.sh: same library, new place
            [ -f "$PREFIX/lib/${ref##*/}" ] && { echo "$PREFIX/lib/${ref##*/}"; return; } ;;
    esac
    return 1
}

clean_rpaths() {  # drop rpaths that point into the build machine
    local f="$1"
    otool -l "$f" | awk '/cmd LC_RPATH/{getline; getline; print $2}' | while read -r rp; do
        case "$rp" in @*) ;; *) install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null || true ;; esac
    done
}

queue=()
fix_file() {
    local f="$1" own refs
    own="$(otool -D "$f" | sed -n 2p)"
    refs="$(otool -L "$f" | tail -n +2 | awk '{print $1}')"
    for ref in $refs; do
        [ "$ref" = "$own" ] && continue
        is_system "$ref" && continue
        local name="${ref##*/}" src
        src="$(resolve "$ref" "$f")" || die "$(basename "$f") needs $ref, which was not found"
        case "$src" in /opt/homebrew/*|/usr/local/*) die "$(basename "$f") links $src from Homebrew (would not be on users' Macs; may be GPL)";; esac
        if [ ! -f "$FW/$name" ]; then
            cp -L "$src" "$FW/$name"
            chmod u+w "$FW/$name"
            install_name_tool -id "@rpath/$name" "$FW/$name"
            queue+=("$FW/$name")
        fi
        [ "$ref" = "@rpath/$name" ] || install_name_tool -change "$ref" "@rpath/$name" "$f"
    done
}

for exe in "$APP/Contents/MacOS/"*; do
    chmod u+w "$exe"
    fix_file "$exe"
    clean_rpaths "$exe"
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$exe"
done
while [ ${#queue[@]} -gt 0 ]; do
    lib="${queue[0]}"; queue=("${queue[@]:1}")
    fix_file "$lib"
    clean_rpaths "$lib"
    install_name_tool -add_rpath "@loader_path" "$lib" 2>/dev/null || true
done

# verify: only system libraries and our own bundled ones may be referenced
for f in "$APP/Contents/MacOS/"* "$FW/"*.dylib; do
    for ref in $(otool -L "$f" | tail -n +2 | awk '{print $1}'); do
        is_system "$ref" && continue
        case "$ref" in
            @rpath/*) [ -f "$FW/${ref#@rpath/}" ] || die "$(basename "$f"): $ref is not in Frameworks" ;;
            *) die "$(basename "$f") still references $ref" ;;
        esac
    done
done
echo "bundled libraries:"; ls "$FW" | sed 's/^/  /'

# ---------------------------------------------------------------- 4. license notices
log "license notices"
LIC="$APP/Contents/Resources/Licenses"
cp -R "$DEPS/licenses/." "$LIC/"
mkdir -p "$LIC/Tolga" && cp "$ROOT/LICENSE" "$ROOT/NOTICE" "$LIC/Tolga/"
D="$BUILD/_deps"
mkdir -p "$LIC/SDL3" "$LIC/imgui" "$LIC/nlohmann_json"
cp "$D/sdl3-src/LICENSE.txt"   "$LIC/SDL3/"          2>/dev/null || die "SDL3 license not found (was SDL fetched?)"
cp "$D/imgui-src/LICENSE.txt"  "$LIC/imgui/"
cp "$D/json-src/LICENSE.MIT"   "$LIC/nlohmann_json/"
if [ -n "$AIRPLAY_DIR" ]; then
    mkdir -p "$LIC/mdns" && cp "$D/mdns-src/LICENSE" "$LIC/mdns/"
elif [ -n "$PREBUILT_LIB" ]; then
    mkdir -p "$LIC/mdns" "$LIC/openssl"
    cp "$PREBUILT_LIB/../licenses/mdns-LICENSE" "$LIC/mdns/LICENSE"
    cp "$PREBUILT_LIB/../licenses/OpenSSL-LICENSE.txt" "$LIC/openssl/LICENSE.txt"
fi
NOTICES="$APP/Contents/Resources/THIRD_PARTY_NOTICES.txt"
AIRPLAY_NOTE=""
[ -n "$HAS_AIRPLAY" ] && AIRPLAY_NOTE="
The AirPlay module of the official app is proprietary software, (c) $(date +%Y) Tolga Ciftcilik.
It is not covered by the Apache License; its source code is not published."
{
cat <<EOF
Tolga $VERSION

The Tolga player is open source under the Apache License, Version 2.0
(Licenses/Tolga/LICENSE and Licenses/Tolga/NOTICE).
Source code: ${TOLGA_SOURCE_URL:-https://github.com/cctolga/tolga}
${AIRPLAY_NOTE}

Third-party software

Tolga uses the open-source libraries listed below. Their license texts are in
Tolga.app/Contents/Resources/Licenses/.

LGPL components (libmpv, FFmpeg, libplacebo, fribidi$( [ -d "$LIC/uchardet" ] && echo ", uchardet")):
  These libraries are licensed under the GNU Lesser General Public License v2.1 or later.
  They are included unmodified, as separate dynamic libraries in Tolga.app/Contents/Frameworks
  (and the ffmpeg/ffprobe programs in Tolga.app/Contents/MacOS). You may replace them with
  your own builds of the same libraries; afterwards re-sign the app with
      codesign --force --deep -s - Tolga.app
  The source code of these libraries is available from the addresses below; the exact build
  configuration is in Licenses/build-deps.sh. On request we also provide the source code on a
  data medium, or by download, for at least three years from the date you received Tolga.
  FFmpeg is built without --enable-gpl and --enable-nonfree; libmpv is built with -Dgpl=false.

Components and versions:
EOF
sed 's/^/  /' "$DEPS/versions.txt"
cat <<EOF
  SDL3                 zlib License   https://libsdl.org
  Dear ImGui           MIT            https://github.com/ocornut/imgui
  nlohmann/json        MIT            https://github.com/nlohmann/json
EOF
if [ -n "$HAS_AIRPLAY" ]; then
cat <<EOF
  mdns                 Public domain  https://github.com/mjansson/mdns
  OpenSSL (libcrypto)  Apache-2.0     https://openssl.org
EOF
fi
} > "$NOTICES"

# ---------------------------------------------------------------- 5. sign
if [ -n "$SIGN_ID" ]; then
    log "sign with: $SIGN_ID"
    SIGN=(codesign --force --timestamp --options runtime -s "$SIGN_ID")
else
    log "sign ad hoc (runs on this Mac only; set TOLGA_SIGN_ID to distribute)"
    SIGN=(codesign --force -s -)
fi
for f in "$FW/"*.dylib "$APP/Contents/MacOS/ffmpeg" "$APP/Contents/MacOS/ffprobe"; do "${SIGN[@]}" "$f"; done
"${SIGN[@]}" "$APP"
codesign --verify --deep --strict "$APP"

# ---------------------------------------------------------------- 6. smoke test: the bundle starts its own libs
"$APP/Contents/MacOS/ffmpeg" -hide_banner -version | head -1
if "$APP/Contents/MacOS/ffmpeg" -hide_banner -buildconf | grep -Eq -- '--enable-(gpl|nonfree)'; then
    die "bundled ffmpeg is a GPL build"
fi

# ---------------------------------------------------------------- 7. dmg
DMG="$DIST/Tolga-$VERSION-$ARCH.dmg"
log "disk image $DMG"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
cp "$NOTICES" "$STAGE/Third-Party Notices.txt"
rm -f "$DMG"
hdiutil create -quiet -volname "Tolga $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO "$DMG"
[ -n "$SIGN_ID" ] && codesign --force --timestamp -s "$SIGN_ID" "$DMG"

if [ -n "$NOTARY" ]; then
    [ -n "$SIGN_ID" ] || die "notarization needs TOLGA_SIGN_ID"
    log "notarize (a few minutes)"
    xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY" --wait
    xcrun stapler staple "$DMG"
    spctl -a -t open --context context:primary-signature -v "$DMG"
fi

log "done"
echo "  app: $APP"
echo "  dmg: $DMG  ($(du -h "$DMG" | cut -f1))"

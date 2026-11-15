#!/usr/bin/env bash
# Builds the playback libraries Tolga ships with, from source, in LGPL mode:
#   libmpv (-Dgpl=false), FFmpeg (no --enable-gpl / --enable-nonfree) and their dependencies.
# Homebrew's mpv/ffmpeg are GPL builds and must not be shipped in a closed-source app;
# these builds can be (dynamic linking + license notices, see THIRD_PARTY_NOTICES).
#
# Usage:   scripts/build-deps.sh            (macOS: brew install meson ninja nasm pkg-config cmake)
# Output:  deps/<os>-<arch>/prefix/{lib,include,bin}, deps/<os>-<arch>/licenses/, versions.txt
#
# Every library is built as a shared library, so each LGPL part can be replaced by the user.
set -eo pipefail   # (no -u: macOS bash 3.2 treats empty arrays as unset)

# ---------------------------------------------------------------- versions
FFMPEG_VER=9.0.2
MPV_VER=0.42.0
PLACEBO_VER=7.387.0
LIBASS_VER=0.17.5
HARFBUZZ_VER=15.1.0
FRIBIDI_VER=1.0.18
FREETYPE_TAG=VER-2-15-0
DAV1D_VER=1.5.4
UCHARDET_VER=0.0.8
OPENSSL_VER=3.5.9

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"
OUT="${TOLGA_DEPS:-$ROOT/deps/$OS-$ARCH}"
PREFIX="$OUT/prefix"
SRC="$ROOT/deps/src"
LIC="$OUT/licenses"
JOBS="$( (sysctl -n hw.ncpu 2>/dev/null || nproc) )"
mkdir -p "$PREFIX" "$SRC" "$LIC"

for tool in meson ninja nasm pkg-config cmake curl git; do
    command -v "$tool" >/dev/null || { echo "missing: $tool  (macOS: brew install meson ninja nasm pkg-config cmake)"; exit 1; }
done

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
if [ "$OS" = darwin ]; then
    # only our own prefix, so nothing from Homebrew (GPL or not) gets linked in by accident
    export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
    export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-12.0}"
    export CFLAGS="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET ${CFLAGS:-}"
    export LDFLAGS="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -Wl,-headerpad_max_install_names ${LDFLAGS:-}"
fi
export CPPFLAGS="-I$PREFIX/include ${CPPFLAGS:-}"
export LDFLAGS="-L$PREFIX/lib ${LDFLAGS:-}"

MESON_COMMON=(--prefix="$PREFIX" --libdir=lib --buildtype=release -Ddefault_library=shared --wrap-mode=nodownload)

log() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
done_mark() { [ -f "$OUT/.done-$1" ]; }
mark() { touch "$OUT/.done-$1"; }

# fetch_git <name> <repo> <tag> [--recursive] -> shallow clone into $SRC/<name>
fetch_git() {
    if [ ! -d "$SRC/$1" ]; then
        log "download $1 $3"
        local sub=()
        [ "${4:-}" = --recursive ] && sub=(--recursive --shallow-submodules)
        git -c advice.detachedHead=false clone --depth 1 ${sub[@]+"${sub[@]}"} -b "$3" "$2" "$SRC/$1"
    fi
}

# fetch <name> <url> -> unpacks a tarball into $SRC/<name>
fetch() {
    local name="$1" url="$2" file="$SRC/$1.tar"
    if [ ! -d "$SRC/$name" ]; then
        log "download $name"
        curl -fL --retry 3 -o "$file" "$url"
        mkdir -p "$SRC/$name"
        tar -xf "$file" -C "$SRC/$name" --strip-components=1
        rm -f "$file"
    fi
}

# license <name> <files...>: keep the license texts for the app bundle
license() {
    local name="$1"; shift
    mkdir -p "$LIC/$name"
    for f in "$@"; do cp "$f" "$LIC/$name/"; done
}

meson_build() {  # meson_build <name> <options...>
    local name="$1"; shift
    (cd "$SRC/$name" && rm -rf _build &&
     meson setup _build "${MESON_COMMON[@]}" "$@" &&
     ninja -C _build -j"$JOBS" && ninja -C _build install)
}

: > "$OUT/versions.txt.new"
version() { echo "$1 $2  $3" >> "$OUT/versions.txt.new"; }

# ---------------------------------------------------------------- OpenSSL libcrypto (Apache-2.0), static
# for AirPlay pairing; linked into the app, so nothing from Homebrew is needed at run time
fetch_git openssl https://github.com/openssl/openssl.git "openssl-$OPENSSL_VER"
version openssl "$OPENSSL_VER" "Apache-2.0 (static)  https://openssl.org"
if ! done_mark openssl; then
    log openssl
    case "$OS-$ARCH" in
        darwin-arm64)  OSSL_TARGET=(darwin64-arm64-cc "-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET") ;;
        darwin-x86_64) OSSL_TARGET=(darwin64-x86_64-cc "-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET") ;;
        *)             OSSL_TARGET=() ;;
    esac
    (cd "$SRC/openssl" && { make clean >/dev/null 2>&1 || true; } &&
     ./Configure ${OSSL_TARGET[@]+"${OSSL_TARGET[@]}"} no-shared no-tests no-docs no-module --prefix="$PREFIX" --libdir=lib &&
     make -j"$JOBS" build_libs && make install_dev)
    mark openssl
fi
license openssl "$SRC/openssl/LICENSE.txt"

# ---------------------------------------------------------------- dav1d (AV1 decoder, BSD-2)
fetch_git dav1d https://github.com/videolan/dav1d.git "$DAV1D_VER"
version dav1d "$DAV1D_VER" "BSD-2-Clause  https://code.videolan.org/videolan/dav1d"
done_mark dav1d || { log dav1d; meson_build dav1d -Denable_tools=false -Denable_tests=false && mark dav1d; }
license dav1d "$SRC/dav1d/COPYING"

# ---------------------------------------------------------------- freetype (FTL)
fetch_git freetype https://github.com/freetype/freetype.git "$FREETYPE_TAG"
version freetype "$FREETYPE_TAG" "FreeType License  https://freetype.org"
done_mark freetype || { log freetype; meson_build freetype -Dharfbuzz=disabled -Dpng=disabled \
    -Dbrotli=disabled -Dbzip2=disabled -Dzlib=internal -Dtests=disabled && mark freetype; }
license freetype "$SRC/freetype/LICENSE.TXT" "$SRC/freetype/docs/FTL.TXT"

# ---------------------------------------------------------------- fribidi (LGPL-2.1)
fetch_git fribidi https://github.com/fribidi/fribidi.git "v$FRIBIDI_VER"
version fribidi "$FRIBIDI_VER" "LGPL-2.1+  https://github.com/fribidi/fribidi"
done_mark fribidi || { log fribidi; meson_build fribidi -Ddocs=false -Dtests=false -Dbin=false && mark fribidi; }
license fribidi "$SRC/fribidi/COPYING"

# ---------------------------------------------------------------- harfbuzz (MIT)
fetch_git harfbuzz https://github.com/harfbuzz/harfbuzz.git "$HARFBUZZ_VER"
version harfbuzz "$HARFBUZZ_VER" "MIT  https://github.com/harfbuzz/harfbuzz"
done_mark harfbuzz || { log harfbuzz; meson_build harfbuzz -Dglib=disabled -Dgobject=disabled -Dcairo=disabled \
    -Dicu=disabled -Dfreetype=enabled -Dtests=disabled -Ddocs=disabled -Dutilities=disabled \
    -Dintrospection=disabled && mark harfbuzz; }
license harfbuzz "$SRC/harfbuzz/COPYING"

# ---------------------------------------------------------------- libass (ISC)
fetch_git libass https://github.com/libass/libass.git "$LIBASS_VER"
version libass "$LIBASS_VER" "ISC  https://github.com/libass/libass"
if ! done_mark libass; then
    log libass
    if [ "$OS" = darwin ]; then
        meson_build libass -Dfontconfig=disabled -Dcoretext=enabled -Drequire-system-font-provider=false -Dtest=disabled
    else
        meson_build libass -Drequire-system-font-provider=false -Dtest=disabled
    fi
    mark libass
fi
license libass "$SRC/libass/COPYING"

# ---------------------------------------------------------------- uchardet (MPL-1.1 / LGPL-2.1): subtitle charset detection
if [ "${TOLGA_SKIP_UCHARDET:-0}" != 1 ]; then
    fetch uchardet "https://www.freedesktop.org/software/uchardet/releases/uchardet-$UCHARDET_VER.tar.xz"
    version uchardet "$UCHARDET_VER" "MPL-1.1 / LGPL-2.1+  https://www.freedesktop.org/wiki/Software/uchardet/"
    if ! done_mark uchardet; then
        log uchardet
        (cd "$SRC/uchardet" && rm -rf _build &&
         cmake -S . -B _build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
               -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_BINARY=OFF -DBUILD_STATIC=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
               -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-}" \
               -DCMAKE_INSTALL_NAME_DIR="$PREFIX/lib" &&
         cmake --build _build -j"$JOBS" && cmake --install _build)
        mark uchardet
    fi
    license uchardet "$SRC/uchardet/COPYING"
fi

# ---------------------------------------------------------------- FFmpeg (LGPL-2.1 build)
fetch_git ffmpeg https://github.com/FFmpeg/FFmpeg.git "n$FFMPEG_VER"
version ffmpeg "$FFMPEG_VER" "LGPL-2.1+ (built without --enable-gpl/--enable-nonfree)  https://ffmpeg.org"
if ! done_mark ffmpeg; then
    log ffmpeg
    FF_OPTS=(--prefix="$PREFIX" --enable-shared --disable-static --disable-doc --disable-debug
             --disable-ffplay --disable-avdevice --enable-pic
             --disable-autodetect --enable-libdav1d --enable-zlib --enable-iconv --enable-pthreads)
    if [ "$OS" = darwin ]; then
        FF_OPTS+=(--enable-videotoolbox --enable-audiotoolbox --enable-securetransport
                  --install-name-dir="$PREFIX/lib"
                  --extra-cflags="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
                  --extra-ldflags="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -Wl,-headerpad_max_install_names"
                  --extra-libs=-liconv)   # iconv lives in libiconv on macOS, not libSystem
    else
        FF_OPTS+=(--enable-rpath)
    fi
    (cd "$SRC/ffmpeg" && make distclean >/dev/null 2>&1 || true
     cd "$SRC/ffmpeg" && ./configure "${FF_OPTS[@]}" && make -j"$JOBS" && make install)
    mark ffmpeg
fi
# refuse to continue with a GPL FFmpeg
# (DYLD/LD_LIBRARY_PATH: the libraries are found even if the project folder was moved)
ff() { DYLD_LIBRARY_PATH="$PREFIX/lib" LD_LIBRARY_PATH="$PREFIX/lib" "$PREFIX/bin/ffmpeg" "$@"; }
if ff -hide_banner -buildconf 2>/dev/null | grep -Eq -- '--enable-(gpl|nonfree)'; then
    echo "FFmpeg was configured with --enable-gpl/--enable-nonfree: not shippable"; exit 1
fi
ff -hide_banner -L 2>/dev/null | grep -q "Lesser General Public License" ||
    { echo "FFmpeg does not report the LGPL license"; exit 1; }
license ffmpeg "$SRC/ffmpeg/COPYING.LGPLv2.1" "$SRC/ffmpeg/LICENSE.md"

# ---------------------------------------------------------------- libplacebo (LGPL-2.1, mpv's renderer)
fetch_git libplacebo https://github.com/haasn/libplacebo.git "v$PLACEBO_VER" --recursive
version libplacebo "$PLACEBO_VER" "LGPL-2.1+  https://code.videolan.org/videolan/libplacebo"
done_mark libplacebo || { log libplacebo; meson_build libplacebo -Dvulkan=disabled -Dopengl=enabled \
    -Dd3d11=disabled -Dshaderc=disabled -Dglslang=disabled -Dlcms=disabled -Ddovi=disabled \
    -Dlibdovi=disabled -Dxxhash=disabled -Dunwind=disabled -Ddemos=false -Dtests=false && mark libplacebo; }
license libplacebo "$SRC/libplacebo/LICENSE"

# ---------------------------------------------------------------- libmpv (LGPL-2.1, -Dgpl=false)
fetch_git mpv https://github.com/mpv-player/mpv.git "v$MPV_VER"
version mpv "$MPV_VER" "LGPL-2.1+ (built with -Dgpl=false)  https://mpv.io"
# mpv 0.42 bug: view.swift uses MetalLayer, which only exists in Vulkan builds; guard it
if ! grep -q "HAVE_VULKAN" "$SRC/mpv/video/out/mac/view.swift"; then
    python3 - "$SRC/mpv/video/out/mac/view.swift" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
old = """        if let metalLayer = layer as? MetalLayer {
            return !metalLayer.isOpaque
        }
"""
if old in s:
    s = s.replace(old, "#if HAVE_VULKAN\n" + old + "#endif\n")
    open(p, "w").write(s)
PYEOF
fi
if ! done_mark mpv; then
    log libmpv
    MPV_OPTS=(-Dgpl=false -Dlibmpv=true -Dcplayer=false -Dtests=false
              -Dlua=disabled -Djavascript=disabled -Dlibarchive=disabled -Dlibbluray=disabled
              -Ddvdnav=disabled -Dcdda=disabled -Drubberband=disabled -Dzimg=disabled -Djpeg=disabled
              -Dlcms2=disabled -Dvapoursynth=disabled -Dsdl2-audio=disabled -Dsdl2-video=disabled
              -Dsdl2-gamepad=disabled -Dvulkan=disabled -Dshaderc=disabled -Dspirv-cross=disabled
              -Dlibavdevice=disabled -Dmanpage-build=disabled -Dhtml-build=disabled -Dpdf-build=disabled)
    if [ "${TOLGA_SKIP_UCHARDET:-0}" = 1 ]; then MPV_OPTS+=(-Duchardet=disabled); else MPV_OPTS+=(-Duchardet=enabled); fi
    if [ "$OS" = darwin ]; then
        MPV_OPTS+=(-Dswift-build=enabled -Dmacos-cocoa-cb=disabled -Dmacos-media-player=disabled
                   -Dcocoa=enabled -Dcoreaudio=enabled -Daudiounit=disabled -Dgl-cocoa=enabled
                   -Dvideotoolbox-gl=enabled -Dvideotoolbox-pl=disabled)
    fi
    meson_build mpv "${MPV_OPTS[@]}"
    mark mpv
fi
grep -q "^#define HAVE_GPL 0" "$SRC/mpv/_build/config.h" || { echo "libmpv was not built in LGPL mode"; exit 1; }
license mpv "$SRC/mpv/LICENSE.LGPL" "$SRC/mpv/Copyright"

mv "$OUT/versions.txt.new" "$OUT/versions.txt"
cp "$0" "$LIC/build-deps.sh"   # how these libraries were built (part of the LGPL source offer)

log "done: $PREFIX"
cat "$OUT/versions.txt"

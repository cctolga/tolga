#!/usr/bin/env bash
# Builds the playback libraries Tolga ships with, from source, in LGPL mode:
#   libmpv (-Dgpl=false), FFmpeg (no --enable-gpl / --enable-nonfree) and their dependencies.
# Homebrew's mpv/ffmpeg are GPL builds and must not be shipped in a closed-source app;
# these builds can be (dynamic linking + license notices, see THIRD_PARTY_NOTICES).
#
# Usage:   scripts/build-deps.sh            (macOS: brew install meson ninja nasm pkg-config cmake)
#          Intel Mac libraries on an Apple Silicon Mac (for the universal app; needs Rosetta:
#            softwareupdate --install-rosetta):  TOLGA_ARCH=x86_64 scripts/build-deps.sh
#          Linux (Debian/Ubuntu): apt install meson ninja-build nasm pkg-config cmake libva-dev libdrm-dev
#            libasound2-dev libpulse-dev
#            libfontconfig-dev
#          Windows: in an MSYS2 UCRT64 shell (pacman -S mingw-w64-ucrt-x86_64-{toolchain,meson,ninja,nasm,
#            pkgconf,cmake} make git), or cross from Linux with TOLGA_TARGET=windows
#            (apt install g++-mingw-w64-x86-64-posix mingw-w64-tools meson ninja-build nasm cmake)
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
NVCODEC_TAG=n13.0.19.0
CURL_TAG=curl-8_23_0

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"
CROSS=""   # tool prefix when cross-compiling (Windows from Linux)
case "$OS" in mingw*|msys*|ucrt*|cygwin*) OS=windows ;; esac
if [ "${TOLGA_TARGET:-}" = windows ] && [ "$OS" != windows ]; then
    OS=windows ARCH=x86_64 CROSS=x86_64-w64-mingw32
fi
MAC_CROSS=""   # set when building the other Mac architecture (x86_64 on Apple Silicon)
if [ "$OS" = darwin ] && [ -n "${TOLGA_ARCH:-}" ] && [ "$TOLGA_ARCH" != "$ARCH" ]; then
    ARCH="$TOLGA_ARCH" MAC_CROSS=1
fi
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
    if [ -n "$MAC_CROSS" ]; then
        # The test programs some configure scripts run are $ARCH binaries: Rosetta runs them.
        arch -x86_64 /usr/bin/true 2>/dev/null ||
            { echo "building for $ARCH needs Rosetta: softwareupdate --install-rosetta"; exit 1; }
        export CFLAGS="-arch $ARCH $CFLAGS" CXXFLAGS="-arch $ARCH ${CXXFLAGS:-}" OBJCFLAGS="-arch $ARCH ${OBJCFLAGS:-}"
        export LDFLAGS="-arch $ARCH $LDFLAGS"
        # meson takes the host's flags from the cross file, not from the environment
        MARGS="'-arch', '$ARCH', '-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET', '-I$PREFIX/include'"
        MLINK="'-arch', '$ARCH', '-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET', '-Wl,-headerpad_max_install_names', '-L$PREFIX/lib'"
        cat > "$OUT/cross.meson" <<MESONEOF
[binaries]
c = ['clang', '-arch', '$ARCH']
cpp = ['clang++', '-arch', '$ARCH']
objc = ['clang', '-arch', '$ARCH']
objcpp = ['clang++', '-arch', '$ARCH']
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'
[properties]
needs_exe_wrapper = false
pkg_config_libdir = '$PREFIX/lib/pkgconfig'
[built-in options]
c_args = [$MARGS]
cpp_args = [$MARGS]
objc_args = [$MARGS]
objcpp_args = [$MARGS]
c_link_args = [$MLINK]
cpp_link_args = [$MLINK]
objc_link_args = [$MLINK]
objcpp_link_args = [$MLINK]
[host_machine]
system = 'darwin'
cpu_family = '$ARCH'
cpu = '$ARCH'
endian = 'little'
MESONEOF
    fi
fi
if [ "$OS" = windows ]; then
    export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"   # nothing from the build machine
    # the C/C++ runtime goes into each DLL, so the app needs no MinGW DLLs next to it
    export LDFLAGS="-static-libgcc -static-libstdc++ ${LDFLAGS:-}"
    if [ -n "$CROSS" ]; then
        CC="$CROSS-gcc" CXX="$CROSS-g++"
        command -v "$CROSS-gcc-posix" >/dev/null && CC="$CROSS-gcc-posix" CXX="$CROSS-g++-posix"
        export CC CXX AR="$CROSS-ar" RANLIB="$CROSS-ranlib" STRIP="$CROSS-strip" WINDRES="$CROSS-windres" NM="$CROSS-nm"
        cat > "$OUT/cross.meson" <<MESONEOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
windres = '$WINDRES'
dlltool = '$CROSS-dlltool'
pkg-config = 'pkg-config'
[properties]
pkg_config_libdir = '$PREFIX/lib/pkgconfig'
[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
MESONEOF
        cat > "$OUT/cross.cmake" <<CMAKEEOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $CC)
set(CMAKE_CXX_COMPILER $CXX)
set(CMAKE_RC_COMPILER $WINDRES)
set(CMAKE_FIND_ROOT_PATH /usr/$CROSS $PREFIX)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
CMAKEEOF
    fi
fi
CMAKE_CROSS=()
[ -n "$CROSS" ] && CMAKE_CROSS=(-DCMAKE_TOOLCHAIN_FILE="$OUT/cross.cmake")
[ -n "$MAC_CROSS" ] && CMAKE_CROSS=(-DCMAKE_OSX_ARCHITECTURES="$ARCH")
export CPPFLAGS="-I$PREFIX/include ${CPPFLAGS:-}"
export LDFLAGS="-L$PREFIX/lib ${LDFLAGS:-}"

MESON_COMMON=(--prefix="$PREFIX" --libdir=lib --buildtype=release -Ddefault_library=shared --wrap-mode=nodownload)
[ -n "$CROSS$MAC_CROSS" ] && MESON_COMMON+=(--cross-file="$OUT/cross.meson")

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
        windows-*)     OSSL_TARGET=(mingw64) ;;
        *)             OSSL_TARGET=(-fPIC) ;;   # linked into shared libraries (FFmpeg, AirPlay module)
    esac
    (cd "$SRC/openssl" && { make clean >/dev/null 2>&1 || true; } &&
     ./Configure ${OSSL_TARGET[@]+"${OSSL_TARGET[@]}"} no-shared no-tests no-docs no-module --prefix="$PREFIX" --libdir=lib &&
     make -j"$JOBS" build_libs && make install_dev)
    mark openssl
fi
license openssl "$SRC/openssl/LICENSE.txt"

# ---------------------------------------------------------------- curl (curl license), Linux and Windows, static
# HTTPS only, on the static OpenSSL: the system libcurl drags in dozens of libraries that
# differ between distributions (macOS uses the one in the SDK). On Windows TLS comes from the
# system (Schannel), which also knows the system's certificates.
if [ "$OS" = linux ] || [ "$OS" = windows ]; then
    if [ "$OS" = windows ]; then CURL_TLS=(-DCURL_USE_SCHANNEL=ON -DCURL_USE_OPENSSL=OFF -DCURL_ZLIB=OFF)
    else CURL_TLS=(-DCURL_USE_OPENSSL=ON -DOPENSSL_ROOT_DIR="$PREFIX" -DOPENSSL_USE_STATIC_LIBS=ON -DCURL_ZLIB=ON); fi
    fetch_git curl https://github.com/curl/curl.git "$CURL_TAG"
    version curl "$CURL_TAG" "curl License (MIT-style, static)  https://curl.se"
    if ! done_mark curl; then
        log curl
        (cd "$SRC/curl" && rm -rf _build &&
         cmake -S . -B _build -G Ninja ${CMAKE_CROSS[@]+"${CMAKE_CROSS[@]}"} -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
               -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON "${CURL_TLS[@]}" \
               -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
               -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF -DBUILD_EXAMPLES=OFF \
               -DHTTP_ONLY=ON \
               -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF \
               -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DCURL_DISABLE_LDAP=ON \
               -DCURL_USE_GSSAPI=OFF -DENABLE_ARES=OFF -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none &&
         cmake --build _build -j"$JOBS" && cmake --install _build)
        mark curl
    fi
    license curl "$SRC/curl/COPYING"
fi

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
    elif [ "$OS" = windows ]; then
        meson_build libass -Dfontconfig=disabled -Ddirectwrite=enabled -Drequire-system-font-provider=false -Dtest=disabled
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
         cmake -S . -B _build -G Ninja ${CMAKE_CROSS[@]+"${CMAKE_CROSS[@]}"} -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
               -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_BINARY=OFF -DBUILD_STATIC=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
               -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-}" \
               -DCMAKE_INSTALL_NAME_DIR="$PREFIX/lib" &&
         cmake --build _build -j"$JOBS" && cmake --install _build)
        mark uchardet
    fi
    license uchardet "$SRC/uchardet/COPYING"
fi

# ---------------------------------------------------------------- nv-codec-headers (MIT), Linux and Windows
# headers only: FFmpeg loads the NVIDIA driver at run time, nothing is linked
if [ "$OS" = linux ] || [ "$OS" = windows ]; then
    fetch_git nv-codec-headers https://github.com/FFmpeg/nv-codec-headers.git "$NVCODEC_TAG"
    version nv-codec-headers "$NVCODEC_TAG" "MIT (headers only)  https://github.com/FFmpeg/nv-codec-headers"
    done_mark nvcodec || { log nv-codec-headers; make -C "$SRC/nv-codec-headers" PREFIX="$PREFIX" install && mark nvcodec; }
fi

# ---------------------------------------------------------------- FFmpeg (LGPL-2.1 build)
fetch_git ffmpeg https://github.com/FFmpeg/FFmpeg.git "n$FFMPEG_VER"
version ffmpeg "$FFMPEG_VER" "LGPL-2.1+ (built without --enable-gpl/--enable-nonfree)  https://ffmpeg.org"
if ! done_mark ffmpeg; then
    log ffmpeg
    FF_OPTS=(--prefix="$PREFIX" --enable-shared --disable-static --disable-doc --disable-debug
             --disable-ffplay --disable-avdevice --enable-pic
             --disable-autodetect --enable-libdav1d)
    [ "$OS" = windows ] || FF_OPTS+=(--enable-zlib --enable-iconv --enable-pthreads)
    if [ "$OS" = windows ]; then
        # D3D11/DXVA2 (any GPU) and NVDEC copy-back decoding, TLS from Windows (Schannel)
        FF_OPTS+=(--target-os=mingw32 --arch=x86_64 --enable-w32threads --enable-d3d11va --enable-dxva2
                  --enable-ffnvcodec --enable-nvdec --enable-schannel --pkg-config-flags=--static)
        [ -n "$CROSS" ] && FF_OPTS+=(--cross-prefix="$CROSS-" --cc="$CC" --cxx="$CXX" --pkg-config=pkg-config --enable-cross-compile)
    elif [ "$OS" = darwin ]; then
        FF_OPTS+=(--enable-videotoolbox --enable-audiotoolbox --enable-securetransport
                  --install-name-dir="$PREFIX/lib"
                  --extra-cflags="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
                  --extra-ldflags="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -Wl,-headerpad_max_install_names"
                  --extra-libs=-liconv)   # iconv lives in libiconv on macOS, not libSystem
        [ -n "$MAC_CROSS" ] && FF_OPTS+=(--enable-cross-compile --arch="$ARCH" --target-os=darwin
                                         --cc="clang -arch $ARCH" --cxx="clang++ -arch $ARCH"
                                         --objcc="clang -arch $ARCH" --pkg-config=pkg-config)
    else
        # VA-API (Intel/AMD), NVDEC (NVIDIA, loaded at run time), TLS from the static OpenSSL above
        FF_OPTS+=(--enable-rpath --enable-vaapi --enable-libdrm --enable-ffnvcodec --enable-nvdec
                  --enable-openssl --pkg-config-flags=--static)
    fi
    (cd "$SRC/ffmpeg" && make distclean >/dev/null 2>&1 || true
     cd "$SRC/ffmpeg" && ./configure "${FF_OPTS[@]}" && make -j"$JOBS" && make install)
    mark ffmpeg
fi
# refuse to continue with a GPL FFmpeg
# (DYLD/LD_LIBRARY_PATH: the libraries are found even if the project folder was moved)
ff() { DYLD_LIBRARY_PATH="$PREFIX/lib" LD_LIBRARY_PATH="$PREFIX/lib" PATH="$PREFIX/bin:$PATH" "$PREFIX/bin/ffmpeg" "$@"; }
if [ -n "$CROSS" ]; then
    # can't run Windows programs here: check the configuration FFmpeg wrote instead
    grep -q '^#define CONFIG_GPL 0' "$SRC/ffmpeg/config.h" && grep -q '^#define CONFIG_NONFREE 0' "$SRC/ffmpeg/config.h" ||
        { echo "FFmpeg was configured with --enable-gpl/--enable-nonfree: not shippable"; exit 1; }
elif ff -hide_banner -buildconf 2>/dev/null | grep -Eq -- '--enable-(gpl|nonfree)'; then
    echo "FFmpeg was configured with --enable-gpl/--enable-nonfree: not shippable"; exit 1
fi
[ -n "$CROSS" ] || ff -hide_banner -L 2>/dev/null | grep -q "Lesser General Public License" ||
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
    if [ "$OS" = windows ]; then
        # drawn through the render API too; WASAPI sound; decoding copies frames back (d3d11va-copy)
        MPV_OPTS+=(-Dgl=enabled -Dplain-gl=enabled -Dgl-win32=disabled -Dd3d11=disabled -Ddirect3d=disabled
                   -Degl-angle=disabled -Degl-angle-lib=disabled -Degl-angle-win32=disabled -Dd3d-hwaccel=disabled
                   -Dd3d9-hwaccel=disabled -Dgl-dxinterop=disabled -Dgl-dxinterop-d3d9=disabled
                   -Dcuda-hwaccel=disabled -Dcuda-interop=disabled -Dwasapi=enabled -Dopenal=disabled
                   -Dlibcurl=disabled -Dsixel=disabled -Dcaca=disabled)
    elif [ "$OS" = darwin ]; then
        MPV_OPTS+=(-Dswift-build=enabled -Dmacos-cocoa-cb=disabled -Dmacos-media-player=disabled
                   -Dcocoa=enabled -Dcoreaudio=enabled -Daudiounit=disabled -Dgl-cocoa=enabled
                   -Dvideotoolbox-gl=enabled -Dvideotoolbox-pl=disabled)
        [ -n "$MAC_CROSS" ] && MPV_OPTS+=(-Dswift-flags="-target $ARCH-apple-macos$MACOSX_DEPLOYMENT_TARGET")
    else
        # The app draws through the libmpv render API (OpenGL from SDL), so mpv needs no window
        # system of its own; mpv's X11 code is GPL anyway. Hardware decoding: NVDEC hands frames
        # to OpenGL through CUDA, VA-API (Intel/AMD) copies them back (FFmpeg opens the GPU).
        # No PipeWire output: its client library must match the system; PipeWire systems play
        # through their PulseAudio service.
        MPV_OPTS+=(-Dgl=enabled -Dplain-gl=enabled -Dx11=disabled -Dwayland=disabled -Ddrm=disabled
                   -Degl=disabled -Dgbm=disabled -Dvaapi=disabled -Dcuda-hwaccel=enabled -Dcuda-interop=enabled
                   -Dalsa=enabled -Dpulse=enabled -Dpipewire=disabled -Djack=disabled -Dsndio=disabled
                   -Doss-audio=disabled -Dopenal=disabled -Dlibcurl=disabled -Dsixel=disabled -Dcaca=disabled
                   -Dvdpau=disabled -Dxv=disabled -Ddmabuf-wayland=disabled)
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

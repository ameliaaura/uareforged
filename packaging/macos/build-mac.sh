#!/bin/bash
# Build the unsigned macOS app of the UA Reforged engine (Apple Silicon):
#   build/macos/UA Reforged.app   and   build/macos/UA-Reforged-<version>-macOS-arm64.zip
#
#   packaging/macos/build-mac.sh          deps -> engine -> app -> zip (finished steps are skipped)
#   packaging/macos/build-mac.sh deps     only (re)build the pinned third-party libraries
#   packaging/macos/build-mac.sh engine   only (re)build the engine against them
#   packaging/macos/build-mac.sh app      only (re)assemble the .app
#   packaging/macos/build-mac.sh zip      only (re)make the zip from the assembled .app
#   FORCE=1 packaging/macos/build-mac.sh  rebuild everything
#
# Environment (all optional):
#   UA_OUT          output dir                    (default: <repo>/build/macos)
#   UA_DEPS_PREFIX  an already built deps prefix  (default: $UA_OUT/deps/prefix, built here)
#   JOBS            parallel build jobs           (default: all cores)
#   UA_RELEASE_DATE the one timestamp every zip entry carries (default: the release date below)
#
# The app contains NO game data. On first launch it asks the player for their own Urban
# Assault folder (see docs/GAME-DATA.md).
#
# Third-party libraries are built from source with MACOSX_DEPLOYMENT_TARGET=11.0 and shipped
# in Contents/Frameworks, so the app runs on any Apple Silicon Mac from macOS 11 up and never
# depends on Homebrew. FFmpeg is a decode-only LGPL build (no GPL-only or non-free parts).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${UA_OUT:-$ROOT/build/macos}"
PREFIX="${UA_DEPS_PREFIX:-$OUT/deps/prefix}"
SRCDIR="$OUT/deps/src"
EBUILD="$OUT/engine-build"
APPNAME="UA Reforged"
APP="$OUT/$APPNAME.app"
PKGDIR="$ROOT/packaging/macos"
MINOS=11.0
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
export MACOSX_DEPLOYMENT_TARGET=$MINOS
FORCE="${FORCE:-0}"
# the desktop version (version.h also holds the Quest app's, first, under UA_VR_NEWLOOK)
VERSION="$(sed -n 's/.*PortVersion *= *"\([^"]*\)".*/\1/p' "$ROOT/src/version.h" | tail -1)"
ZIP="$OUT/UA-Reforged-$VERSION-macOS-arm64.zip"
SOURCE_URL="https://github.com/ameliaaura/uareforged"   # the public engine repository (GPL source offer)
RELEASE_DATE="${UA_RELEASE_DATE:-2026-11-01}"

log() { printf '\n=== %s\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
[ "$(uname -m)" = arm64 ] || die "run this on an Apple Silicon Mac"
command -v cmake >/dev/null || die "need cmake"
command -v pkg-config >/dev/null || die "need pkg-config"

# ---------------------------------------------------------------- deps
# name|url|sha256 (SKIP = record the hash in licenses/third-party-sources.sha256 only)
DEPS="
SDL2-2.32.10|https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz|SKIP
SDL2_image-2.8.8|https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.8/SDL2_image-2.8.8.tar.gz|SKIP
SDL2_ttf-2.24.0|https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.24.0/SDL2_ttf-2.24.0.tar.gz|SKIP
SDL2_net-2.2.0|https://github.com/libsdl-org/SDL_net/releases/download/release-2.2.0/SDL2_net-2.2.0.tar.gz|SKIP
libogg-1.3.6|https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.gz|SKIP
libvorbis-1.3.7|https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.gz|SKIP
lua-5.5.0|https://www.lua.org/ftp/lua-5.5.0.tar.gz|SKIP
ffmpeg-8.1.2|https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz|SKIP
"

fetch() { # name url sha -> extracted dir $SRCDIR/name
    local name="$1" url="$2" sha="$3" file="$SRCDIR/${url##*/}"
    mkdir -p "$SRCDIR"
    [ -f "$file" ] || curl -fsSL -o "$file" "$url"
    local got; got="$(shasum -a 256 "$file" | awk '{print $1}')"
    if [ "$sha" != SKIP ] && [ "$got" != "$sha" ]; then die "sha256 mismatch for $file: $got"; fi
    echo "$got  ${url##*/}" >> "$SRCDIR/fetched.sha256"
    rm -rf "${SRCDIR:?}/$name"; tar -xf "$file" -C "$SRCDIR"
}

cmake_dep() { # srcdir extra-args...
    local src="$1"; shift
    cmake -S "$src" -B "$src/_build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" \
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$MINOS \
        -DCMAKE_INSTALL_NAME_DIR=@rpath -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=ON "$@" >"$src/_build.log" 2>&1 || { tail -30 "$src/_build.log"; die "configure $src"; }
    cmake --build "$src/_build" -j"$JOBS" >>"$src/_build.log" 2>&1 || { tail -30 "$src/_build.log"; die "build $src"; }
    cmake --install "$src/_build" >>"$src/_build.log" 2>&1 || die "install $src"
}

build_deps() {
    if [ "$FORCE" != 1 ] && [ -f "$PREFIX/.done" ]; then log "deps: already built ($PREFIX)"; return; fi
    log "deps: building into $PREFIX (macOS $MINOS, arm64)"
    rm -rf "$PREFIX" "$SRCDIR/fetched.sha256"; mkdir -p "$PREFIX"
    echo "$DEPS" | while IFS='|' read -r name url sha; do
        if [ -n "$name" ]; then echo "fetch $name"; fetch "$name" "$url" "$sha"; fi
    done
    export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH=""

    echo "build SDL2";       cmake_dep "$SRCDIR/SDL2-2.32.10" -DSDL_STATIC=OFF -DSDL_TEST=OFF
    echo "build SDL2_image"; cmake_dep "$SRCDIR/SDL2_image-2.8.8" -DSDL2IMAGE_SAMPLES=OFF \
        -DSDL2IMAGE_DEPS_SHARED=OFF -DSDL2IMAGE_BACKEND_STB=ON -DSDL2IMAGE_BACKEND_IMAGEIO=OFF \
        -DSDL2IMAGE_AVIF=OFF -DSDL2IMAGE_JXL=OFF -DSDL2IMAGE_TIF=OFF -DSDL2IMAGE_WEBP=OFF
    echo "build SDL2_ttf";   cmake_dep "$SRCDIR/SDL2_ttf-2.24.0" -DSDL2TTF_SAMPLES=OFF \
        -DSDL2TTF_VENDORED=ON -DSDL2TTF_HARFBUZZ=OFF
    echo "build SDL2_net";   cmake_dep "$SRCDIR/SDL2_net-2.2.0" -DSDL2NET_SAMPLES=OFF
    echo "build libogg";     cmake_dep "$SRCDIR/libogg-1.3.6" -DINSTALL_DOCS=OFF -DBUILD_TESTING=OFF
    echo "build libvorbis";  cmake_dep "$SRCDIR/libvorbis-1.3.7"

    echo "build lua (static)"
    ( cd "$SRCDIR/lua-5.5.0" && make -s macosx MYCFLAGS="-arch arm64 -mmacosx-version-min=$MINOS" \
        MYLDFLAGS="-arch arm64 -mmacosx-version-min=$MINOS" >_build.log 2>&1 \
      && make -s install INSTALL_TOP="$PREFIX" >>_build.log 2>&1 ) || die "lua"

    echo "build ffmpeg (decode-only, LGPL, no external codecs)"
    ( cd "$SRCDIR/ffmpeg-8.1.2" && ./configure --prefix="$PREFIX" --cc=clang --arch=arm64 \
        --enable-shared --disable-static --install-name-dir=@rpath \
        --extra-cflags="-mmacosx-version-min=$MINOS" --extra-ldflags="-mmacosx-version-min=$MINOS" \
        --disable-autodetect --enable-zlib --disable-programs --disable-doc --disable-network \
        --disable-avdevice --disable-avfilter --disable-encoders --disable-muxers \
        --disable-devices --disable-filters --disable-hwaccels --disable-bsfs \
        --enable-bsf=hevc_mp4toannexb,h264_mp4toannexb >_build.log 2>&1 \
      && make -j"$JOBS" >>_build.log 2>&1 && make install >>_build.log 2>&1 ) \
      || { tail -30 "$SRCDIR/ffmpeg-8.1.2/_build.log"; die "ffmpeg"; }
    touch "$PREFIX/.done"
}

# ---------------------------------------------------------------- engine
build_engine() {
    if [ "$FORCE" != 1 ] && [ -x "$EBUILD/UA_source" ] && [ "$EBUILD/UA_source" -nt "$PREFIX/.done" ]; then
        log "engine: already built ($EBUILD/UA_source)"; return; fi
    log "engine: building $ROOT/src ($VERSION)"
    rm -rf "$EBUILD"; mkdir -p "$OUT"
    # keep the build machine's paths out of the binary (logical and resolved forms)
    local PMAP="-ffile-prefix-map=$ROOT=. -ffile-prefix-map=$(cd "$ROOT" && pwd -P)=. -ffile-prefix-map=$OUT=build -ffile-prefix-map=$(cd "$OUT" && pwd -P)=build"
    # CMAKE_BUILD_TYPE must be set; the engine CMakeLists then appends its own flags.
    PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH="" \
    cmake -S "$ROOT/src" -B "$EBUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" \
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$MINOS \
        -DLUA_INCLUDE_DIR="$PREFIX/include" -DLUA_LIBRARY="$PREFIX/lib/liblua.a" \
        -DCMAKE_SKIP_RPATH=ON -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath,@executable_path/../Frameworks" \
        -DCMAKE_C_FLAGS="$PMAP" \
        -DCMAKE_CXX_FLAGS="$PMAP" \
        >"$OUT/engine-configure.log" 2>&1 || { tail -30 "$OUT/engine-configure.log"; die "engine configure"; }
    cmake --build "$EBUILD" -j"$JOBS" >"$OUT/engine-build.log" 2>&1 \
        || { grep -m20 -E "error" "$OUT/engine-build.log"; die "engine build"; }
    if otool -L "$EBUILD/UA_source" | grep -E "/opt/homebrew|/usr/local"; then die "engine links Homebrew"; fi
}

# ---------------------------------------------------------------- app
build_app() {
    [ -x "$EBUILD/UA_source" ] || die "no engine; run: $0 engine"
    log "app: assembling $APP"
    rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
    local C="$APP/Contents" R="$APP/Contents/Resources" E="$APP/Contents/Resources/engine"

    # engine + every non-system dylib it needs (recursively), rewired to @rpath
    cp "$EBUILD/UA_source" "$C/MacOS/UA_source"
    strip -S "$C/MacOS/UA_source"   # no debug map: it would carry the build machine's paths
    local queue=("$C/MacOS/UA_source") seen=" " i=0
    while [ $i -lt ${#queue[@]} ]; do
        local f="${queue[$i]}"; i=$((i+1))
        for dep in $(otool -L "$f" | awk 'NR>1{print $1}' | grep '^@rpath/' || true); do
            local leaf="${dep#@rpath/}"
            case "$seen" in *" $leaf "*) continue ;; esac
            seen="$seen$leaf "
            local real; real="$(cd "$PREFIX/lib" && realpath "$leaf")" || die "missing $leaf"
            cp "$real" "$C/Frameworks/$leaf"; chmod u+w "$C/Frameworks/$leaf"
            install_name_tool -id "@rpath/$leaf" "$C/Frameworks/$leaf" 2>/dev/null
            install_name_tool -add_rpath @loader_path "$C/Frameworks/$leaf" 2>/dev/null || true
            queue+=("$C/Frameworks/$leaf")
        done
    done
    for f in "$C/MacOS/UA_source" "$C/Frameworks/"*.dylib; do
        if otool -L "$f" | awk 'NR>1{print $1}' | grep -vE '^(@rpath/|/usr/lib/|/System/Library/)'; then
            die "non-system, non-bundled dependency in $f"; fi
    done

    # the engine's own read-only files (shaders, free fonts, locale tables); no game data
    mkdir -p "$E"
    cp -R "$ROOT/res" "$ROOT/fonts" "$E/"
    cp "$ROOT/docs/ai-tuning.example.txt" "$R/ai-tuning.example.txt"

    # launcher, plist, icon, licences, docs
    cp "$PKGDIR/launcher.sh" "$C/MacOS/$APPNAME"; chmod 755 "$C/MacOS/$APPNAME"
    # "Get the game" + one-click Reforge pack download: the code, its progress window, the links.json check and
    # the addresses + pack checksum list
    cp "$PKGDIR/downloads.sh" "$PKGDIR/downloads.txt" "$PKGDIR/progress.js" "$PKGDIR/dialog.js" "$PKGDIR/links.js" "$PKGDIR/quest.sh" "$R/"
    sed -e "s/@VERSION@/$VERSION/" -e "s/@BUILD@/$VERSION/" -e "s/@MINOS@/$MINOS/" "$PKGDIR/Info.plist.in" > "$C/Info.plist"
    cp "$PKGDIR/AppIcon.icns" "$R/AppIcon.icns"
    mkdir -p "$R/licenses"
    cp "$ROOT/LICENSE" "$R/licenses/UA-Reforged-engine-GPL-2.0.txt"
    cp "$ROOT/CREDITS.md" "$R/licenses/CREDITS.md"
    if [ -f "$SRCDIR/fetched.sha256" ]; then cp "$SRCDIR/fetched.sha256" "$R/licenses/third-party-sources.sha256"; fi
    for d in SDL2-2.32.10 SDL2_image-2.8.8 SDL2_ttf-2.24.0 SDL2_net-2.2.0 libogg-1.3.6 libvorbis-1.3.7; do
        cp "$SRCDIR/$d/"LICENSE.txt "$R/licenses/$d.txt" 2>/dev/null || cp "$SRCDIR/$d/COPYING" "$R/licenses/$d.txt" 2>/dev/null \
            || echo "$d: zlib/BSD licence, see the project's source release" > "$R/licenses/$d.txt"
    done
    cp "$SRCDIR/ffmpeg-8.1.2/COPYING.LGPLv2.1" "$R/licenses/ffmpeg-8.1.2-LGPL-2.1.txt" 2>/dev/null \
        || echo "FFmpeg 8.1.2, LGPL 2.1 or later: https://ffmpeg.org/legal.html" > "$R/licenses/ffmpeg-8.1.2-LGPL-2.1.txt"
    printf 'Lua 5.5.0 - MIT licence, see https://www.lua.org/license.html\n' > "$R/licenses/lua-5.5.0.txt"
    cp "$PKGDIR/README-mac.txt" "$R/README-mac.txt"

    sign_app "$APP"
    du -sh "$APP"
}

# ad-hoc signature (required for arm64 code to run at all); NOT Developer ID. Extended attributes go first.
sign_app() {
    local app="$1" c="$1/Contents"
    xattr -cr "$app"
    for f in "$c/Frameworks/"*.dylib; do codesign --force -s - "$f"; done
    codesign --force -s - "$c/MacOS/UA_source"
    codesign --force -s - "$app"
    codesign --verify --strict "$app" && echo "codesign: ad-hoc signature verifies"
}

# ---------------------------------------------------------------- zip
build_zip() {
    [ -d "$APP" ] || die "no app; run: $0 app"
    log "zip: $ZIP"
    local st="$OUT/zipstage"; rm -rf "$st" "$ZIP"; mkdir -p "$st/UA Reforged"
    # a clean copy: no extended attributes, resource forks or ACLs; signed again after stripping
    ditto --norsrc --noextattr --noacl "$APP" "$st/UA Reforged/$APPNAME.app"
    cp "$PKGDIR/README-mac.txt" "$st/UA Reforged/README.txt"
    cp "$ROOT/LICENSE" "$st/UA Reforged/LICENSE-engine-GPL-2.0.txt"
    printf 'Source code (GNU GPL version 2) of this build of the UA Reforged engine, version %s:\n    %s\n' \
        "$VERSION" "$SOURCE_URL" > "$st/UA Reforged/SOURCE.txt"
    sign_app "$st/UA Reforged/$APPNAME.app"
    xattr -cr "$st"
    # one fixed timestamp, normalised modes, no owner ids / extra fields / AppleDouble entries (mkzip.py)
    python3 "$PKGDIR/mkzip.py" --date "$RELEASE_DATE" "$ZIP" "$st/UA Reforged"
    rm -rf "$st"
    ls -l "$ZIP"; ( cd "$OUT" && shasum -a 256 "$(basename "$ZIP")" | tee "$(basename "$ZIP").sha256" )
}

step="${1:-all}"
case "$step" in
    deps) build_deps ;;
    engine) build_deps; build_engine ;;
    app) build_app ;;
    zip) build_zip ;;
    all) build_deps; build_engine; build_app; build_zip ;;
    *) die "unknown step $step" ;;
esac

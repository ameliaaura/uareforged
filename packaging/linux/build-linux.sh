#!/bin/bash
# Build the Linux edition of the UA Reforged engine (x86_64):
#   $UA_OUT/UA-Reforged-<version>-Linux-x86_64.AppImage   one file: make it executable and run it
#   $UA_OUT/UA-Reforged-<version>-Linux-x86_64.tar.gz     the same files as a folder (no FUSE needed)
#
# Runs inside packaging/linux/Dockerfile (Ubuntu 22.04, glibc 2.35); packaging/linux/docker-build.sh starts it.
#   build-linux.sh [all|deps|engine|appdir|package]
#
# Environment: UA_OUT (output dir, default /out), UA_WORK (build tree on a Linux file system: deps, engine build,
# AppDir; default /work, a container volume), JOBS, UA_BUILD_ID (crash reports), UA_RELEASE_DATE (the one
# timestamp every packaged file carries, YYYY-MM-DD).
#
# The package contains NO game data (docs/GAME-DATA.md). Third-party libraries are built from source and shipped
# next to the engine (found through its RPATH, never through LD_LIBRARY_PATH, so programs the launcher starts, such
# as the web browser, are not affected). The system provides glibc, libstdc++, the OpenGL driver and the
# X11 / Wayland / audio / udev libraries that SDL and OpenAL Soft load at run time.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${UA_OUT:-/out}"
WORK="${UA_WORK:-/work}"
PREFIX="$WORK/deps/prefix"
SRCDIR="$WORK/deps/src"
EBUILD="$WORK/engine-build"
APPDIR="$WORK/AppDir"
PKGDIR="$ROOT/packaging/linux"
JOBS="${JOBS:-$(nproc)}"
VERSION="$(sed -n 's/.*PortVersion *= *"\([^"]*\)".*/\1/p' "$ROOT/src/version.h" | tail -1)"
NAME="UA-Reforged-$VERSION-Linux-x86_64"
SOURCE_URL="https://github.com/ameliaaura/uareforged"   # the public engine repository (GPL source offer)
RELEASE_DATE="${UA_RELEASE_DATE:-2026-11-01}"
[ -n "$RELEASE_DATE" ] || RELEASE_DATE=2026-11-01
EPOCH="$(date -u -d "$RELEASE_DATE 00:00:00" +%s)"

log() { printf '\n=== %s\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
[ "$(uname -m)" = x86_64 ] || die "run this in an x86_64 (amd64) Linux environment"

# ---------------------------------------------------------------- deps
# name|url|sha256 (SKIP = record the hash in licenses/third-party-sources.sha256 only)
DEPS="
SDL2-2.32.10|https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz|SKIP
SDL2_image-2.8.8|https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.8/SDL2_image-2.8.8.tar.gz|SKIP
SDL2_ttf-2.24.0|https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.24.0/SDL2_ttf-2.24.0.tar.gz|SKIP
SDL2_net-2.2.0|https://github.com/libsdl-org/SDL_net/releases/download/release-2.2.0/SDL2_net-2.2.0.tar.gz|SKIP
libogg-1.3.6|https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.gz|SKIP
libvorbis-1.3.7|https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.gz|SKIP
openal-soft-1.24.3|https://github.com/kcat/openal-soft/archive/refs/tags/1.24.3.tar.gz|SKIP
lua-5.5.0|https://www.lua.org/ftp/lua-5.5.0.tar.gz|SKIP
ffmpeg-8.1.2|https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz|SKIP
"
# shipped as they are (pinned by SHA-256): the AppImage start-up code and a self-contained curl for the
# downloads on systems without one (Ubuntu's desktop install has no curl)
RUNTIME_URL="https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64"
CURL_URL="https://github.com/moparisthebest/static-curl/releases/download/v8.21.0/curl-amd64"

fetch() { # name url sha -> extracted dir $SRCDIR/name
    local name="$1" url="$2" sha="$3" file="$SRCDIR/${url##*/}"
    case "$name" in openal-soft-*) file="$SRCDIR/$name.tar.gz" ;; esac
    mkdir -p "$SRCDIR"
    [ -f "$file" ] || curl -fsSL --retry 6 --retry-delay 10 --retry-all-errors --connect-timeout 20 -o "$file" "$url"
    local got; got="$(sha256sum "$file" | awk '{print $1}')"
    if [ "$sha" != SKIP ] && [ "$got" != "$sha" ]; then die "sha256 mismatch for $file: $got"; fi
    echo "$got  ${file##*/}" >> "$SRCDIR/fetched.sha256"
    rm -rf "${SRCDIR:?}/$name"; tar -xf "$file" -C "$SRCDIR"
}

# again <cmd...>: run a build step up to 3 times (builds resume where they stopped). Only for flaky build hosts;
# it cannot repair a configure check that crashed, so release builds run on real x86_64 (docs/BUILD-Linux.md).
again() { "$@" || { echo "  (retrying: $*)"; "$@"; } || { echo "  (retrying again: $*)"; "$@"; }; }

cmake_dep() { # srcdir extra-args...
    local src="$1"; shift
    [ -f "$PREFIX/.done-${src##*/}" ] && { echo "  (already built)"; return 0; }
    cmake -S "$src" -B "$src/_build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=ON "$@" >"$src/_build.log" 2>&1 || { tail -30 "$src/_build.log"; die "configure $src"; }
    again cmake --build "$src/_build" -j"$JOBS" >>"$src/_build.log" 2>&1 || { tail -30 "$src/_build.log"; die "build $src"; }
    cmake --install "$src/_build" >>"$src/_build.log" 2>&1 || die "install $src"
    touch "$PREFIX/.done-${src##*/}"
}

build_deps() {
    if [ -f "$PREFIX/.done" ]; then log "deps: already built ($PREFIX)"; return; fi
    log "deps: building into $PREFIX (x86_64, glibc $(ldd --version | head -1 | awk '{print $NF}'))"
    # a step that finished is not repeated (stamp files in the prefix); remove the build volume for a clean build
    mkdir -p "$PREFIX"
    if [ ! -f "$PREFIX/.fetched" ]; then
        rm -f "$SRCDIR/fetched.sha256"
        echo "$DEPS" | while IFS='|' read -r name url sha; do
            if [ -n "$name" ]; then echo "fetch $name"; fetch "$name" "$url" "$sha"; fi
        done
        touch "$PREFIX/.fetched"
    fi
    export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig" PKG_CONFIG_PATH=""

    # X11, Wayland (with libdecor), KMS/DRM, ALSA, PulseAudio, PipeWire and udev are loaded at run time when present
    echo "build SDL2";       cmake_dep "$SRCDIR/SDL2-2.32.10" -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_RPATH=OFF \
        -DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_WAYLAND_LIBDECOR=ON -DSDL_KMSDRM=ON -DSDL_OPENGL=ON -DSDL_OPENGLES=ON \
        -DSDL_VULKAN=OFF -DSDL_IBUS=OFF -DSDL_ALSA=ON -DSDL_PULSEAUDIO=ON -DSDL_PIPEWIRE=ON -DSDL_JACK=OFF \
        -DSDL_SNDIO=OFF -DSDL_ESD=OFF -DSDL_ARTS=OFF -DSDL_NAS=OFF -DSDL_FUSIONSOUND=OFF -DSDL_LIBSAMPLERATE=OFF
    echo "build SDL2_image"; cmake_dep "$SRCDIR/SDL2_image-2.8.8" -DSDL2IMAGE_SAMPLES=OFF \
        -DSDL2IMAGE_DEPS_SHARED=OFF -DSDL2IMAGE_BACKEND_STB=ON \
        -DSDL2IMAGE_AVIF=OFF -DSDL2IMAGE_JXL=OFF -DSDL2IMAGE_TIF=OFF -DSDL2IMAGE_WEBP=OFF
    echo "build SDL2_ttf";   cmake_dep "$SRCDIR/SDL2_ttf-2.24.0" -DSDL2TTF_SAMPLES=OFF \
        -DSDL2TTF_VENDORED=ON -DSDL2TTF_HARFBUZZ=OFF
    echo "build SDL2_net";   cmake_dep "$SRCDIR/SDL2_net-2.2.0" -DSDL2NET_SAMPLES=OFF
    # SDL2_net 2.2.0 writes "Libs:" twice into its .pc file; Ubuntu's pkg-config (unlike pkgconf) refuses that
    awk '/^Libs:/ { if (seen++) next } { print }' "$PREFIX/lib/pkgconfig/SDL2_net.pc" > "$PREFIX/lib/pkgconfig/SDL2_net.pc.tmp" \
        && mv "$PREFIX/lib/pkgconfig/SDL2_net.pc.tmp" "$PREFIX/lib/pkgconfig/SDL2_net.pc"
    echo "build libogg";     cmake_dep "$SRCDIR/libogg-1.3.6" -DINSTALL_DOCS=OFF -DBUILD_TESTING=OFF
    echo "build libvorbis";  cmake_dep "$SRCDIR/libvorbis-1.3.7"
    echo "build OpenAL Soft"; cmake_dep "$SRCDIR/openal-soft-1.24.3" -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF \
        -DALSOFT_TESTS=OFF -DALSOFT_INSTALL_CONFIG=OFF -DALSOFT_INSTALL_HRTF_DATA=OFF -DALSOFT_INSTALL_AMBDEC_PRESETS=OFF \
        -DALSOFT_BACKEND_PIPEWIRE=ON -DALSOFT_BACKEND_PULSEAUDIO=ON -DALSOFT_BACKEND_ALSA=ON -DALSOFT_BACKEND_OSS=ON \
        -DALSOFT_BACKEND_JACK=OFF -DALSOFT_BACKEND_SNDIO=OFF -DALSOFT_BACKEND_PORTAUDIO=OFF -DALSOFT_BACKEND_SDL2=OFF \
        -DALSOFT_BACKEND_WAVE=ON -DALSOFT_DLOPEN=ON

    echo "build lua (static)"
    [ -f "$PREFIX/.done-lua" ] || ( cd "$SRCDIR/lua-5.5.0" && again make -s linux MYCFLAGS="-fPIC" >_build.log 2>&1 \
      && make -s install INSTALL_TOP="$PREFIX" >>_build.log 2>&1 && touch "$PREFIX/.done-lua" ) || { tail -30 "$SRCDIR/lua-5.5.0/_build.log"; die "lua"; }

    echo "build ffmpeg (decode-only, LGPL, no external codecs)"
    [ -f "$PREFIX/.done-ffmpeg" ] || ( cd "$SRCDIR/ffmpeg-8.1.2" && ./configure --prefix="$PREFIX" --arch=x86_64 \
        --enable-shared --disable-static --enable-pic \
        --disable-autodetect --enable-zlib --disable-programs --disable-doc --disable-network \
        --disable-avdevice --disable-avfilter --disable-encoders --disable-muxers \
        --disable-devices --disable-filters --disable-hwaccels --disable-bsfs \
        --enable-bsf=hevc_mp4toannexb,h264_mp4toannexb >_build.log 2>&1 \
      && again make -j"$JOBS" >>_build.log 2>&1 && make install >>_build.log 2>&1 && touch "$PREFIX/.done-ffmpeg" ) \
      || { tail -30 "$SRCDIR/ffmpeg-8.1.2/_build.log"; die "ffmpeg"; }

    echo "fetch the AppImage runtime + curl"
    curl -fsSL --retry 6 --retry-delay 10 --retry-all-errors -o "$SRCDIR/runtime-x86_64" "$RUNTIME_URL"
    curl -fsSL --retry 6 --retry-delay 10 --retry-all-errors -o "$SRCDIR/curl-amd64" "$CURL_URL"
    ( cd "$SRCDIR" && sha256sum runtime-x86_64 curl-amd64 >> fetched.sha256 )
    touch "$PREFIX/.done"
}

# ---------------------------------------------------------------- engine
build_engine() {
    log "engine: building $ROOT/src ($VERSION)"
    rm -rf "$EBUILD"; mkdir -p "$EBUILD"
    # keep the build machine's paths out of the binary
    local PMAP="-ffile-prefix-map=$ROOT=. -ffile-prefix-map=$WORK=build"
    PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH="" \
    cmake -S "$ROOT/src" -B "$EBUILD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DLUA_INCLUDE_DIR="$PREFIX/include" -DLUA_LIBRARY="$PREFIX/lib/liblua.a" \
        -DOPENAL_INCLUDE_DIR="$PREFIX/include" -DOPENAL_LIBRARY="$PREFIX/lib/libopenal.so" \
        -DOpenGL_GL_PREFERENCE=LEGACY -DCMAKE_SKIP_RPATH=ON \
        -DCMAKE_C_FLAGS="$PMAP" -DCMAKE_CXX_FLAGS="$PMAP" \
        -DUA_BUILD_ID="${UA_BUILD_ID:-dev}" -DUA_TEST_HOOKS="${UA_TEST_HOOKS:-OFF}" \
        >"$OUT/engine-configure.log" 2>&1 || { tail -30 "$OUT/engine-configure.log"; die "engine configure"; }
    again cmake --build "$EBUILD" -j"$JOBS" >"$OUT/engine-build.log" 2>&1 \
        || { grep -m20 -E "error" "$OUT/engine-build.log"; die "engine build"; }
    # the small dialog helper of the launcher (SDL message boxes + a progress window), against the same SDL
    mkdir -p "$OUT/symbols"; cp "$EBUILD/UA_source" "$OUT/symbols/UA_source"   # unstripped, never shipped
    gcc -O2 -Wall $PMAP -o "$EBUILD/ua-dialog" "$PKGDIR/ua-dialog.c" \
        $(PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" pkg-config --cflags --libs sdl2 SDL2_ttf) \
        || die "ua-dialog"
}

# ---------------------------------------------------------------- AppDir (also the tar.gz contents)
build_appdir() {
    [ -x "$EBUILD/UA_source" ] || die "no engine; run: $0 engine"
    log "appdir: assembling $APPDIR"
    rm -rf "$APPDIR"; mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/ua-reforged"
    local B="$APPDIR/usr/bin" LIB="$APPDIR/usr/lib" S="$APPDIR/usr/share/ua-reforged"
    cp "$EBUILD/UA_source" "$EBUILD/ua-dialog" "$B/"
    strip --strip-debug "$B/UA_source"; strip "$B/ua-dialog"   # no debug info: it would carry build paths
    install -m 755 "$SRCDIR/curl-amd64" "$B/curl"
    # every library from the deps prefix the engine and helper need (recursively), found through $ORIGIN
    local queue=("$B/UA_source" "$B/ua-dialog") seen=" " i=0
    while [ $i -lt ${#queue[@]} ]; do
        local f="${queue[$i]}"; i=$((i+1))
        for dep in $(LD_LIBRARY_PATH="$PREFIX/lib" ldd "$f" | awk '$3 ~ /^\// {print $1"|"$3}'); do
            local leaf="${dep%%|*}" real="${dep#*|}"
            case "$real" in "$PREFIX"/*) ;; *) continue ;; esac
            case "$seen" in *" $leaf "*) continue ;; esac
            seen="$seen$leaf "
            cp -L "$real" "$LIB/$leaf"; chmod 644 "$LIB/$leaf"; strip --strip-unneeded "$LIB/$leaf"
            patchelf --force-rpath --set-rpath '$ORIGIN' "$LIB/$leaf"
            queue+=("$LIB/$leaf")
        done
    done
    patchelf --force-rpath --set-rpath '$ORIGIN/../lib' "$B/UA_source"
    patchelf --force-rpath --set-rpath '$ORIGIN/../lib' "$B/ua-dialog"
    # nothing may come from the build machine except the base system every desktop has
    local f bad=0
    for f in "$B/UA_source" "$B/ua-dialog" "$LIB"/*; do
        while read -r so; do
            case "$so" in
                linux-vdso.so.1|ld-linux-x86-64.so.2|libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|librt.so.1|\
                libstdc++.so.6|libgcc_s.so.1|libGL.so.1|libz.so.1) ;;
                *) [ -e "$LIB/$so" ] || { echo "unexpected system library: $so (in $f)"; bad=1; } ;;
            esac
        done < <(patchelf --print-needed "$f")
    done
    [ $bad = 0 ] || die "non-bundled dependency"
    # the newest glibc / libstdc++ symbol versions used (the minimum the player's system needs)
    objdump -T "$B/UA_source" "$LIB"/* 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 > "$OUT/min-glibc.txt"
    objdump -T "$B/UA_source" "$LIB"/* 2>/dev/null | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -1 >> "$OUT/min-glibc.txt"

    # the engine's own read-only files (shaders, free fonts); no game data
    mkdir -p "$S/engine"; cp -R "$ROOT/res" "$ROOT/fonts" "$S/engine/"
    cp "$ROOT/docs/ai-tuning.example.txt" "$S/"
    # launcher: "Get the game", the Reforge pack download, the usage stats question (packaging/linux/README-linux.txt)
    install -m 755 "$PKGDIR/launcher.sh" "$APPDIR/AppRun"
    cp "$PKGDIR/downloads.sh" "$PKGDIR/links.py" "$S/"
    cp "$ROOT/packaging/macos/downloads.txt" "$S/downloads.txt"   # the same addresses and pack checksums as the Mac app
    cp "$PKGDIR/ua-reforged.desktop" "$APPDIR/ua-reforged.desktop"
    cp "$PKGDIR/ua-reforged.png" "$APPDIR/ua-reforged.png"; cp "$PKGDIR/ua-reforged.png" "$APPDIR/.DirIcon"
    mkdir -p "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"
    cp "$PKGDIR/ua-reforged.desktop" "$APPDIR/usr/share/applications/"
    cp "$PKGDIR/ua-reforged.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/"
    # licences, docs, source offer
    mkdir -p "$S/licenses"
    cp "$ROOT/LICENSE" "$S/licenses/UA-Reforged-engine-GPL-2.0.txt"
    cp "$ROOT/CREDITS.md" "$S/licenses/CREDITS.md"
    cp "$SRCDIR/fetched.sha256" "$S/licenses/third-party-sources.sha256"
    for d in SDL2-2.32.10 SDL2_image-2.8.8 SDL2_ttf-2.24.0 SDL2_net-2.2.0 libogg-1.3.6 libvorbis-1.3.7; do
        cp "$SRCDIR/$d/"LICENSE.txt "$S/licenses/$d.txt" 2>/dev/null || cp "$SRCDIR/$d/COPYING" "$S/licenses/$d.txt" 2>/dev/null \
            || echo "$d: zlib/BSD licence, see the project's source release" > "$S/licenses/$d.txt"
    done
    cp "$SRCDIR/openal-soft-1.24.3/COPYING" "$S/licenses/openal-soft-1.24.3-LGPL-2.0.txt"
    cp "$SRCDIR/ffmpeg-8.1.2/COPYING.LGPLv2.1" "$S/licenses/ffmpeg-8.1.2-LGPL-2.1.txt"
    printf 'Lua 5.5.0 - MIT licence, see https://www.lua.org/license.html\n' > "$S/licenses/lua-5.5.0.txt"
    printf 'curl 8.21.0 (static build by moparisthebest/static-curl) - curl licence (MIT style), see https://curl.se/docs/copyright.html\n' > "$S/licenses/curl-8.21.0.txt"
    printf 'AppImage type2 runtime (20251108) - MIT licence, see https://github.com/AppImage/type2-runtime\n' > "$S/licenses/appimage-runtime.txt"
    cp "$PKGDIR/README-linux.txt" "$S/README-linux.txt"
    printf 'Source code (GNU GPL version 2) of this build of the UA Reforged engine, version %s:\n    %s\n' \
        "$VERSION" "$SOURCE_URL" > "$S/SOURCE.txt"
    find "$APPDIR" -exec touch -h -d "@$EPOCH" {} +
    du -sh "$APPDIR"
}

# ---------------------------------------------------------------- AppImage + tar.gz
build_package() {
    [ -x "$APPDIR/AppRun" ] || die "no AppDir; run: $0 appdir"
    log "package: $NAME"
    local img="$OUT/$NAME.AppImage" tgz="$OUT/$NAME.tar.gz" sq="$WORK/appdir.squashfs"
    rm -f "$img" "$tgz" "$sq"
    mksquashfs "$APPDIR" "$sq" -root-owned -noappend -no-xattrs -comp zstd -Xcompression-level 19 \
        -mkfs-time "$EPOCH" -all-time "$EPOCH" -quiet >/dev/null
    cat "$SRCDIR/runtime-x86_64" "$sq" > "$img"; chmod 755 "$img"; rm -f "$sq"
    # tar.gz: the same files in a folder; ./ua-reforged starts it (the AppImage's AppRun)
    local st="$WORK/tarstage"; rm -rf "$st"; mkdir -p "$st"
    cp -a "$APPDIR" "$st/$NAME"
    mv "$st/$NAME/AppRun" "$st/$NAME/ua-reforged"
    cp "$PKGDIR/README-linux.txt" "$st/$NAME/README.txt"
    cp "$ROOT/LICENSE" "$st/$NAME/LICENSE-engine-GPL-2.0.txt"
    cp "$APPDIR/usr/share/ua-reforged/SOURCE.txt" "$st/$NAME/SOURCE.txt"
    find "$st" -exec touch -h -d "@$EPOCH" {} +
    ( cd "$st" && tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$EPOCH" -cf - "$NAME" | gzip -9n > "$tgz" )
    rm -rf "$st"
    ( cd "$OUT" && sha256sum "$NAME.AppImage" "$NAME.tar.gz" | tee "$NAME.sha256" )
    ls -l "$img" "$tgz"
}

mkdir -p "$OUT" "$WORK"
step="${1:-all}"
case "$step" in
    deps) build_deps ;;
    engine) build_deps; build_engine ;;
    appdir) build_appdir ;;
    package) build_package ;;
    all) build_deps; build_engine; build_appdir; build_package ;;
    *) die "unknown step $step" ;;
esac

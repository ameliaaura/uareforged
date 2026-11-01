#!/bin/bash
# Cross-build the engine's third-party libraries for Quest (Android arm64-v8a, API 32) as static
# libraries into one prefix:  <tp>/prefix-android-arm64/{include,lib}
#   SDL2 2.32.10 (patched: no-JNI file opens), SDL2_image 2.8.12 (stb PNG/JPG), SDL2_ttf 2.24.0
#   (vendored FreeType, no HarfBuzz), SDL2_net 2.4.0, OpenAL Soft 1.25.2 (AAudio/OpenSL),
#   libogg 1.3.6, libvorbis 1.3.7, Lua 5.5.1.
# FFmpeg is NOT built: video playback is stubbed on Android (src/system/movie_stub.cpp).
# Toolchain: Android NDK 27.2 (27.2.12479018) + CMake 3.22.1/ninja from the Android SDK (see ../build-quest.sh for the
# environment variables); OpenAL Soft needs a host CMake >= 3.26 (HOST_CMAKE, default: cmake on PATH).
# Usage: build_deps.sh [--clean]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
TP="${UAVR_THIRD_PARTY:-$REPO/build-quest/third_party}"
NDK="${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to an Android NDK 27.2 (27.2.12479018)}"
CMAKE_DIR="${ANDROID_SDK_ROOT:?set ANDROID_SDK_ROOT to the Android SDK}/cmake/3.22.1/bin"
PREFIX="$TP/prefix-android-arm64"
BUILD="$TP/build-android-arm64"
JOBS="${JOBS:-6}"

[[ "${1:-}" == "--clean" ]] && rm -rf "$PREFIX" "$BUILD"
"$HERE/fetch_deps.sh"
mkdir -p "$PREFIX" "$BUILD"

COMMON=(-G Ninja
    -DCMAKE_MAKE_PROGRAM="$CMAKE_DIR/ninja"
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-32 -DANDROID_STL=c++_static
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
    -DCMAKE_PREFIX_PATH="$PREFIX"
    -DCMAKE_FIND_ROOT_PATH="$PREFIX"
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_C_FLAGS="-ffile-prefix-map=$TP=deps -ffile-prefix-map=$NDK=ndk"
    -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$TP=deps -ffile-prefix-map=$NDK=ndk"
    -DBUILD_SHARED_LIBS=OFF)

cm() { # name srcdir [cmake args...]
    local name=$1 src=$2; shift 2
    if [[ -f "$BUILD/$name/.installed" ]]; then echo "== $name: built"; return; fi
    echo "== $name"
    local CM="${CM:-$CMAKE_DIR/cmake}"
    "$CM" -S "$src" -B "$BUILD/$name" "${COMMON[@]}" "$@" > "$BUILD/$name.configure.log" 2>&1 \
        || { tail -30 "$BUILD/$name.configure.log"; exit 1; }
    "$CM" --build "$BUILD/$name" -j "$JOBS" > "$BUILD/$name.build.log" 2>&1 \
        || { grep -E "error|Error" "$BUILD/$name.build.log" | head -30; exit 1; }
    "$CM" --install "$BUILD/$name" > "$BUILD/$name.install.log" 2>&1
    touch "$BUILD/$name/.installed"
}

# SDL2 (+ the no-JNI file-open patch; applied once, marker inside the patched file)
SDL_SRC="$TP/SDL2-2.32.10/src/SDL2-2.32.10"
if ! grep -q "UAVR patch" "$SDL_SRC/src/file/SDL_rwops.c"; then
    patch -d "$SDL_SRC" -p1 < "$HERE/patches/sdl2-rwops-no-jni.patch"
fi
cm SDL2 "$SDL_SRC" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DSDL_TEST=OFF \
    -DSDL_HIDAPI=OFF -DSDL2_DISABLE_SDL2MAIN=ON

SDL_ARGS=(-DSDL2_DIR="$PREFIX/lib/cmake/SDL2")
cm SDL2_image "$TP/SDL2_image-2.8.12/src/SDL2_image-2.8.12" "${SDL_ARGS[@]}" \
    -DSDL2IMAGE_VENDORED=OFF -DSDL2IMAGE_BACKEND_STB=ON -DSDL2IMAGE_SAMPLES=OFF -DSDL2IMAGE_TESTS=OFF \
    -DSDL2IMAGE_AVIF=OFF -DSDL2IMAGE_JXL=OFF -DSDL2IMAGE_TIF=OFF -DSDL2IMAGE_WEBP=OFF \
    -DSDL2IMAGE_DEPS_SHARED=OFF
cm SDL2_ttf "$TP/SDL2_ttf-2.24.0/src/SDL2_ttf-2.24.0" "${SDL_ARGS[@]}" \
    -DSDL2TTF_VENDORED=ON -DSDL2TTF_HARFBUZZ=OFF -DSDL2TTF_SAMPLES=OFF
cm SDL2_net "$TP/SDL2_net-2.4.0/src/SDL2_net-2.4.0" "${SDL_ARGS[@]}" -DSDL2NET_SAMPLES=OFF

cm ogg "$TP/libogg-1.3.6/src/libogg-1.3.6" -DINSTALL_DOCS=OFF -DBUILD_TESTING=OFF
cm vorbis "$TP/libvorbis-1.3.7/src/libvorbis-1.3.7" -DOgg_ROOT="$PREFIX"

# OpenAL Soft 1.25 needs CMake >= 3.26 ($<BUILD_LOCAL_INTERFACE>); the SDK's 3.22 cannot
# configure it, so it uses the host CMake with the same NDK toolchain file.
CM="${HOST_CMAKE:-cmake}" cm openal "$TP/openal-soft-1.25.2/src/openal-soft-1.25.2" -DLIBTYPE=STATIC \
    -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_TESTS=OFF -DALSOFT_INSTALL_EXAMPLES=OFF \
    -DALSOFT_INSTALL_UTILS=OFF -DALSOFT_BACKEND_OBOE=OFF -DALSOFT_BACKEND_SDL2=OFF \
    -DALSOFT_BACKEND_WAVE=ON -DALSOFT_REQUIRE_OPENSL=ON -DALSOFT_ENABLE_MODULES=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF

cm lua "$HERE/lua" -DLUA_SRC="$TP/lua-5.5.1/src/lua-5.5.1/src"

echo "deps installed in $PREFIX"
ls "$PREFIX/lib"

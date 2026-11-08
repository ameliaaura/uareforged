#!/bin/bash
# Build the UA Reforged VR app for Meta Quest (arm64-v8a, NativeActivity + OpenXR, no Java, no Gradle):
#   packaging/quest/build-quest.sh [--verify]
#     -> build-quest/UA-Reforged-VR-<version>-Quest3.apk
#
# Needs (environment):
#   ANDROID_SDK_ROOT   Android SDK with build-tools 36.0.0, platforms/android-34, cmake/3.22.1 (+ ninja)
#   ANDROID_NDK_HOME   Android NDK 27.2 (27.2.12479018)
#   JAVA_HOME          a JDK 17 (apksigner, keytool)
#   HOST_CMAKE         CMake >= 3.26 for OpenAL Soft (default: cmake on PATH)
# Optional:
#   UAVR_KEYSTORE, UAVR_KEY_ALIAS, UAVR_KS_PASS_FILE   release signing (the password is read from the file);
#                      without them a throwaway debug key is made in build-quest/ (fine for your own headset)
#   UAVR_THIRD_PARTY   where the pinned dependencies are fetched and built (default build-quest/third_party)
#   UAVR_OUT (default build-quest), JOBS, BUILD_TYPE (default RelWithDebInfo; symbols stay in the unstripped
#   build-quest/cmake/libuavr.so), RELEASE_DATE (zip entry date, default 2026-11-01)
#
# The app carries no game data. On first launch it offers to download the community OpenUA Standalone Package
# onto the headset and the Reforge pack (src/setup in platform/quest), or the player copies their own files.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
VERSION="0.9.0"
SDK="${ANDROID_SDK_ROOT:?set ANDROID_SDK_ROOT}"
NDK="${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME}"
: "${JAVA_HOME:?set JAVA_HOME to a JDK 17}"
export ANDROID_SDK_ROOT ANDROID_NDK_HOME JAVA_HOME
export PATH="$JAVA_HOME/bin:$PATH"
BT="$SDK/build-tools/36.0.0"
CMAKE_DIR="$SDK/cmake/3.22.1/bin"
ANDROID_JAR="$SDK/platforms/android-34/android.jar"
OUT="${UAVR_OUT:-$REPO/build-quest}"
export UAVR_THIRD_PARTY="${UAVR_THIRD_PARTY:-$OUT/third_party}"
TP="$UAVR_THIRD_PARTY"
OPENXR_AAR_DIR="$TP/openxr-loader-android-1.1.63/extracted"
PREFIX="$TP/prefix-android-arm64"
ABI=arm64-v8a
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
RELEASE_DATE="${RELEASE_DATE:-2026-11-01}"
APK="$OUT/UA-Reforged-VR-$VERSION-Quest3.apk"
JOBS="${JOBS:-6}"
VERIFY=0
for a in "$@"; do
    case "$a" in
        --verify) VERIFY=1 ;;
        *) echo "unknown argument: $a" >&2; exit 2 ;;
    esac
done
HOSTBIN="$NDK/toolchains/llvm/prebuilt/$(ls "$NDK/toolchains/llvm/prebuilt" | head -1)/bin"

mkdir -p "$OUT"
"$HERE/deps/fetch_openxr.sh" >/dev/null
[[ -f "$PREFIX/lib/libSDL2.a" ]] || "$HERE/deps/build_deps.sh"

# 1. libuavr.so (engine + platform layer + start screens). Paths in the binary are relative to the repo.
PMAP="-ffile-prefix-map=$REPO=. -ffile-prefix-map=$(cd "$REPO" && pwd -P)=. -ffile-prefix-map=$TP=deps -ffile-prefix-map=$NDK=ndk -ffile-prefix-map=$OUT=build"
"$CMAKE_DIR/cmake" -S "$REPO/src" -B "$OUT/cmake" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$CMAKE_DIR/ninja" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DUA_VR=ON -DUA_VR_PREVIEW=ON -DUA_VR_NEWLOOK=ON -DUAVR_APP_VERSION="$VERSION" \
    -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-32 -DANDROID_STL=c++_static \
    -DCMAKE_C_FLAGS="$PMAP" -DCMAKE_CXX_FLAGS="$PMAP" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DUAVR_PREFIX="$PREFIX" -DOPENXR_AAR_DIR="$OPENXR_AAR_DIR" \
    -DUA_BUILD_ID="${UA_BUILD_ID:-dev}" -DUA_TEST_HOOKS="${UA_TEST_HOOKS:-OFF}" \
    > "$OUT/configure.log" 2>&1 || { tail -40 "$OUT/configure.log"; exit 1; }
"$CMAKE_DIR/cmake" --build "$OUT/cmake" -j "$JOBS"

# 2. stage: stripped libraries, assets (the engine's own files, downloads list, licences)
STAGE="$OUT/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/lib/$ABI" "$STAGE/assets/ua" "$STAGE/assets/licenses"
"$HOSTBIN/llvm-strip" --strip-unneeded -o "$STAGE/lib/$ABI/libuavr.so" "$OUT/cmake/libuavr.so"
cp "$OPENXR_AAR_DIR/prefab/modules/openxr_loader/libs/android.$ABI/libopenxr_loader.so" "$STAGE/lib/$ABI/"
"$HOSTBIN/llvm-strip" --strip-unneeded "$STAGE/lib/$ABI/libopenxr_loader.so"
cp -R "$REPO/res" "$STAGE/assets/ua/res"
mkdir -p "$STAGE/assets/ua/fonts"
cp "$REPO/fonts/"* "$STAGE/assets/ua/fonts/"
(cd "$STAGE/assets/ua" && find res fonts -type f | LC_ALL=C sort) > "$STAGE/assets/ua-assets.txt"
cp "$HERE/downloads.txt" "$STAGE/assets/downloads.txt"
cp "$REPO/LICENSE" "$STAGE/assets/licenses/LICENSE-GPL-2.0.txt"
cp "$REPO/CREDITS.md" "$STAGE/assets/licenses/CREDITS.md"
cp "$HERE/licenses/"* "$STAGE/assets/licenses/"
printf 'Source code (GNU GPL version 2) of this build of UA Reforged VR, version %s:\n    %s\n(the Quest app is built from packaging/quest/build-quest.sh in that repository)\n' \
    "$VERSION" "https://github.com/ameliaaura/uareforged" > "$STAGE/assets/licenses/SOURCE.txt"
find "$STAGE" -name '.DS_Store' -delete

# 3. manifest + resources -> APK (fixed entry dates, stable order), libs stored + 16 KB aligned, signed
rm -f "$OUT/res-compiled.zip"
"$BT/aapt2" compile --dir "$HERE/res" -o "$OUT/res-compiled.zip"
"$BT/aapt2" link -o "$OUT/base.apk" --manifest "$HERE/AndroidManifest.xml" -I "$ANDROID_JAR" \
    --min-sdk-version 32 --target-sdk-version 34 "$OUT/res-compiled.zip"
python3 "$HERE/apkzip.py" "$OUT/base.apk" "$OUT/unaligned.apk" --assets "$STAGE/assets" --lib "$STAGE/lib" --date "$RELEASE_DATE"
"$BT/zipalign" -f -P 16 4 "$OUT/unaligned.apk" "$OUT/aligned.apk"
if [[ -n "${UAVR_KEYSTORE:-}" ]]; then
    "$BT/apksigner" sign --ks "$UAVR_KEYSTORE" --ks-key-alias "${UAVR_KEY_ALIAS:?set UAVR_KEY_ALIAS}" \
        --ks-pass "file:${UAVR_KS_PASS_FILE:?set UAVR_KS_PASS_FILE}" --out "$APK" "$OUT/aligned.apk"
else
    KS="$OUT/debug.keystore"
    [[ -f "$KS" ]] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias androiddebugkey \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null
    echo "NOTE: signed with a throwaway debug key ($KS)"
    "$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --ks-key-alias androiddebugkey \
        --out "$APK" "$OUT/aligned.apk"
fi
rm -f "$OUT/base.apk" "$OUT/unaligned.apk" "$OUT/aligned.apk" "$APK.idsig"
echo "built $APK ($(wc -c < "$APK" | tr -d ' ') bytes)"
(cd "$OUT" && shasum -a 256 "$(basename "$APK")" | tee "$(basename "$APK").sha256")

if [[ $VERIFY == 1 ]]; then
    echo "== aapt2 dump badging"
    "$BT/aapt2" dump badging "$APK"
    echo "== apksigner verify"
    "$BT/apksigner" verify --verbose --print-certs "$APK" | grep -vE "^WARNING: META-INF" | head -14
    echo "== zipalign -c -P 16"
    "$BT/zipalign" -c -P 16 4 "$APK" && echo "alignment OK"
    echo "== readelf libuavr.so"
    "$HOSTBIN/llvm-readelf" -d "$STAGE/lib/$ABI/libuavr.so" | grep -E "NEEDED|SONAME"
    "$HOSTBIN/llvm-readelf" --dyn-syms "$STAGE/lib/$ABI/libuavr.so" | grep -E " ANativeActivity_onCreate| UA_main" || true
    echo "== undefined symbols not provided by the NEEDED system libs (should be empty)"
    "$HOSTBIN/llvm-readelf" --dyn-syms -W "$STAGE/lib/$ABI/libuavr.so" | awk '$7=="UND"{print $8}' | sed 's/@.*//' | sort -u > "$OUT/undef.txt"
    SYSROOT="$NDK/toolchains/llvm/prebuilt/$(ls "$NDK/toolchains/llvm/prebuilt" | head -1)/sysroot/usr/lib/aarch64-linux-android/32"
    for l in libc libm libdl liblog libandroid libEGL libGLESv3 libGLESv1_CM libOpenSLES libz; do
        "$HOSTBIN/llvm-readelf" --dyn-syms -W "$SYSROOT/$l.so" | awk '$7!="UND"{print $8}' | sed 's/@.*//'
    done | sort -u > "$OUT/provided.txt"
    "$HOSTBIN/llvm-readelf" --dyn-syms -W "$STAGE/lib/$ABI/libopenxr_loader.so" | awk '$7!="UND"{print $8}' | sed 's/@.*//' | sort -u >> "$OUT/provided.txt"
    sort -u -o "$OUT/provided.txt" "$OUT/provided.txt"
    comm -23 "$OUT/undef.txt" "$OUT/provided.txt" | grep -v "^$" || echo "(none)"
fi

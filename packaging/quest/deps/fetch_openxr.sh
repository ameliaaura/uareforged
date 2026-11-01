#!/bin/bash
# Fetch the pinned Khronos OpenXR loader for Android (prebuilt AAR: libopenxr_loader.so for all
# ABIs + headers), verify sha256, extract into its own empty directory. Idempotent.
set -euo pipefail

OPENXR_VER=1.1.63
OPENXR_SHA256=622419d2f6741c3443a3beb4779af0764318edd01830de967f24c741ebcded73
URL="https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-${OPENXR_VER}/openxr_loader_for_android-${OPENXR_VER}.aar"

TP="${UAVR_THIRD_PARTY:-$(cd "$(dirname "$0")/../../.." && pwd)/build-quest/third_party}"
DIR="$TP/openxr-loader-android-${OPENXR_VER}"
AAR="$DIR/download/openxr_loader_for_android-${OPENXR_VER}.aar"

if [[ -f "$DIR/extracted/prefab/modules/headers/include/openxr/openxr.h" ]]; then
    echo "openxr loader ${OPENXR_VER}: present at $DIR/extracted"
    exit 0
fi

mkdir -p "$DIR/download"
if [[ ! -f "$AAR" ]]; then
    curl -sSL --fail -o "$AAR.part" "$URL"
    mv "$AAR.part" "$AAR"
fi
echo "${OPENXR_SHA256}  $AAR" | shasum -a 256 -c -

rm -rf "$DIR/extracted"
mkdir -p "$DIR/extracted"
unzip -q "$AAR" -d "$DIR/extracted"
echo "openxr loader ${OPENXR_VER}: extracted to $DIR/extracted"

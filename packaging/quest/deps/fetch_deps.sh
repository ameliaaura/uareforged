#!/bin/bash
# Fetch the pinned third-party sources for the Quest (Android arm64) engine build.
# Each archive is untrusted data: it is downloaded into <tp>/<name>/download/, its sha256 is
# checked against the pin below, and it is extracted into its own empty <tp>/<name>/src/.
# Nothing is executed from there; build_deps.sh (this directory) drives the builds.
# Idempotent. Default <tp> = <repo>/build-quest/third_party (UAVR_THIRD_PARTY).
set -euo pipefail

TP="${UAVR_THIRD_PARTY:-$(cd "$(dirname "$0")/../../.." && pwd)/build-quest/third_party}"

# name | url | sha256 | cross-check
DEPS=(
"SDL2-2.32.10|https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz|5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165|GitHub asset digest"
"SDL2_image-2.8.12|https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.12/SDL2_image-2.8.12.tar.gz|393f5efb50536ec13ca4f4affb69cc9966d3c3f969e6c5e701faddf9f9785381|GitHub asset digest"
"SDL2_ttf-2.24.0|https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.24.0/SDL2_ttf-2.24.0.tar.gz|0b2bf1e7b6568adbdbc9bb924643f79d9dedafe061fa1ed687d1d9ac4e453bfd|pinned on first download (asset has no digest)"
"SDL2_net-2.4.0|https://github.com/libsdl-org/SDL_net/releases/download/release-2.4.0/SDL2_net-2.4.0.tar.gz|9cbca2527feb3f1a622d48ba65cc7dee9b1e3f2c55ceafb7d7720bb058aafb30|GitHub asset digest"
"openal-soft-1.25.2|https://openal-soft.org/openal-releases/openal-soft-1.25.2.tar.bz2|1dbaac44e7579d5bc8847ca8db4b2e8b9fd3961041f35ee20def4958301e1089|pinned on first download"
"libogg-1.3.6|https://github.com/xiph/ogg/releases/download/v1.3.6/libogg-1.3.6.tar.xz|5c8253428e181840cd20d41f3ca16557a9cc04bad4a3d04cce84808677fa1061|GitHub asset digest"
"libvorbis-1.3.7|https://github.com/xiph/vorbis/releases/download/v1.3.7/libvorbis-1.3.7.tar.xz|b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b|pinned on first download"
"lua-5.5.1|https://www.lua.org/ftp/lua-5.5.1.tar.gz|1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce|lua.org/ftp checksum column"
)

for entry in "${DEPS[@]}"; do
    IFS='|' read -r name url sha note <<<"$entry"
    dir="$TP/$name"
    file="$dir/download/$(basename "$url")"
    if [[ -f "$dir/src/.extracted-$sha" ]]; then
        echo "$name: present"
        continue
    fi
    mkdir -p "$dir/download"
    if [[ ! -f "$file" ]]; then
        curl -sSL --fail -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    echo "$sha  $file" | shasum -a 256 -c - >/dev/null || { echo "$name: SHA256 MISMATCH"; exit 1; }
    rm -rf "$dir/src"
    mkdir -p "$dir/src"
    tar -xf "$file" -C "$dir/src"
    touch "$dir/src/.extracted-$sha"
    echo "$name: verified ($note) and extracted"
done

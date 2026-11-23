#!/bin/bash
# Build the Linux edition (x86_64) in a container on an x86_64 machine with Docker. Runs packaging/linux/build-linux.sh
# inside packaging/linux/Dockerfile. (Under amd64 emulation on Apple Silicon the compiler crashes now and then and
# configure checks can silently come out wrong: not for release builds. See docs/BUILD-Linux.md.)
#
#   packaging/linux/docker-build.sh [step] [out dir]     step: all (default) | deps | engine | appdir | package
#
# Environment: UA_LINUX_IMAGE (default ua-reforged-linux-build:22.04), UA_LINUX_WORK_VOLUME (the build tree, a Docker
# volume: default ua-reforged-linux-work; remove it for a clean build), JOBS, UA_BUILD_ID, UA_RELEASE_DATE.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
STEP="${1:-all}"
OUT="${2:-$ROOT/build/linux}"
IMG="${UA_LINUX_IMAGE:-ua-reforged-linux-build:22.04}"
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
docker build --platform linux/amd64 -t "$IMG" "$ROOT/packaging/linux" >/dev/null
docker run --rm --platform linux/amd64 \
    -v "$ROOT:/src:ro" -v "$OUT:/out" -v "${UA_LINUX_WORK_VOLUME:-ua-reforged-linux-work}:/work" \
    -e UA_OUT=/out -e JOBS="${JOBS:-4}" -e UA_BUILD_ID="${UA_BUILD_ID:-dev}" \
    -e UA_RELEASE_DATE="${UA_RELEASE_DATE:-}" \
    "$IMG" bash /src/packaging/linux/build-linux.sh "$STEP"

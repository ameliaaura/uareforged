#!/bin/bash
# UA Reforged - Windows (x64) build, cross-compiled on a Mac: the NSIS installer and a zip.
#
#   packaging/windows/build-windows.sh [all|deps|engine|stage|installer|zip] [out dir]
#
# Route: Homebrew mingw-w64 (UCRT) against MSYS2 ucrt64 binary packages + BtbN's LGPL FFmpeg 8.1 shared build
# (fetch-deps.py), then NSIS (Homebrew makensis). Needs: brew install mingw-w64 makensis cmake.
# Output (default build/windows): UA-Reforged-<version>-Windows-Setup.exe, UA-Reforged-<version>-Windows-x64.zip
# (+ .sha256). NO game data goes in: the launcher asks for the player's own folder or fetches the community
# package from the Metropolis Dawn community on the player's PC.
# UA_WIN_DEPS=<dir> reuses a dependency folder (sysroot/ + cache/) instead of fetching one.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PKG="$ROOT/packaging/windows"
STEP="${1:-all}"
OUT="${2:-$ROOT/build/windows}"
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
DEPS="${UA_WIN_DEPS:-$OUT/deps}"
SYSROOT="$DEPS/sysroot"
EBUILD="$OUT/engine-build"
STAGE="$OUT/stage/UA Reforged"
GEN="$OUT/stage-gen"
# the desktop version (version.h also holds the Quest app's, first, under UA_VR_NEWLOOK)
VERSION="$(sed -n 's/.*PortVersion *= *"\([^"]*\)".*/\1/p' "$ROOT/src/version.h" | tail -1)"
SOURCE_URL="https://github.com/ameliaaura/uareforged"   # the public engine repository (GPL source offer)
FIXED_TIME="202611010000"   # every staged file and zip entry gets this time (touch -t format, UTC)
SETUP="$OUT/UA-Reforged-$VERSION-Windows-Setup.exe"
ZIP="$OUT/UA-Reforged-$VERSION-Windows-x64.zip"
OBJDUMP=x86_64-w64-mingw32-objdump
# DLLs Windows itself provides. Anything else the engine needs must be bundled.
SYSTEM_DLLS='^(kernel32|user32|gdi32|opengl32|advapi32|shell32|ole32|oleaut32|winmm|imm32|version|setupapi|ws2_32|iphlpapi|dnsapi|bcrypt|bcryptprimitives|crypt32|ncrypt|secur32|shlwapi|dwmapi|cfgmgr32|hid|dinput8|xinput1_4|uxtheme|psapi|avrt|mfplat|mf|mfreadwrite|mfuuid|comdlg32|winhttp|powrprof|ntdll|userenv|dbghelp|msvcrt|ucrtbase|d3d9|d3d11|d3d12|dxgi|dxva2|evr|propsys|wintrust|rpcrt4|combase|d2d1|dwrite|msimg32|usp10|api-ms-win-.*)\.dll$'

log() { printf '\n=== %s\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
for t in x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ x86_64-w64-mingw32-windres x86_64-w64-mingw32-strip makensis cmake; do
    command -v "$t" >/dev/null || die "missing tool: $t (brew install mingw-w64 makensis cmake)"
done
[ -n "$VERSION" ] || die "no PortVersion in src/version.h"

deps() {
    if [ ! -f "$SYSROOT/lib/pkgconfig/libavcodec.pc" ]; then
        log "deps: MSYS2 ucrt64 + FFmpeg 8.1 LGPL -> $DEPS"
        python3 "$PKG/fetch-deps.py" "$DEPS"
    fi
    grep -q '^Version: 62\.' "$SYSROOT/lib/pkgconfig/libavcodec.pc" || die "FFmpeg in the sysroot is not the 8.x API"
}

engine() {
    deps
    log "engine: cross-compiling $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo src)"
    export UA_WIN_SYSROOT="$SYSROOT" PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig" PKG_CONFIG_PATH=""
    # no build-machine paths in the binaries
    local PMAP="-ffile-prefix-map=$ROOT=. -ffile-prefix-map=$(cd "$ROOT" && pwd -P)=. -ffile-prefix-map=$OUT=build -ffile-prefix-map=$(cd "$OUT" && pwd -P)=build -ffile-prefix-map=$SYSROOT=sysroot -ffile-prefix-map=$(cd "$SYSROOT" && pwd -P)=sysroot"
    # CMAKE_BUILD_TYPE as the Mac build (RelWithDebInfo); the debug info is stripped when the files are assembled
    cmake -S "$ROOT/src" -B "$EBUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
          -DCMAKE_TOOLCHAIN_FILE="$PKG/toolchain.cmake" "-DCMAKE_C_FLAGS=$PMAP" "-DCMAKE_CXX_FLAGS=$PMAP" \
          -DUA_BUILD_ID="${UA_BUILD_ID:-dev}" -DUA_TEST_HOOKS="${UA_TEST_HOOKS:-OFF}" >"$OUT/engine-configure.log"
    cmake --build "$EBUILD" -j "${JOBS:-6}" >"$OUT/engine-build.log" || { tail -30 "$OUT/engine-build.log"; die "engine build failed"; }
    [ -f "$EBUILD/UA_source.exe" ] || die "build produced no UA_source.exe"
    log "stub: UA Reforged.exe"
    local sb="$OUT/stub"; rm -rf "$sb"; mkdir -p "$sb"
    cp "$ROOT/svg/icon/icon.ico" "$sb/icon.ico"; cp "$PKG/stub/stub.rc" "$sb/stub.rc"
    sed -i '' "s/0,8,0,0/$(echo "$VERSION" | tr . ,),0/g; s/\"0\.8\.0\"/\"$VERSION\"/g" "$sb/stub.rc"
    ( cd "$sb" && x86_64-w64-mingw32-windres stub.rc -O coff -o stub.res.o )
    x86_64-w64-mingw32-gcc -municode -mwindows -O2 -s -Wall -Wl,--no-insert-timestamp $PMAP "$PKG/stub/launcher-stub.c" "$sb/stub.res.o" -o "$sb/UA Reforged.exe"
}

stage() {
    [ -f "$EBUILD/UA_source.exe" ] && [ -f "$OUT/stub/UA Reforged.exe" ] || die "no engine; run: $0 engine"
    log "stage: $STAGE"
    rm -rf "$OUT/stage" "$GEN"; mkdir -p "$STAGE/engine" "$STAGE/launcher" "$STAGE/licenses" "$GEN"
    local E="$STAGE/engine"
    cp "$OUT/stub/UA Reforged.exe" "$STAGE/UA Reforged.exe"
    x86_64-w64-mingw32-strip -o "$E/UA-Reforged-engine.exe" "$EBUILD/UA_source.exe"
    # strip writes a new COFF time stamp; zero it (reproducible builds)
    python3 -I - "$E/UA-Reforged-engine.exe" <<'PY'
import struct, sys
with open(sys.argv[1], 'r+b') as f:
    f.seek(0x3C); pe = struct.unpack('<I', f.read(4))[0]
    f.seek(pe); assert f.read(4) == b'PE\0\0'
    f.seek(pe + 8); f.write(b'\0\0\0\0')
PY

    # every non-system DLL the engine loads, recursively, from the sysroot
    local queue=("$E/UA-Reforged-engine.exe") seen=" " external=" " i=0 f dll lc src
    while [ $i -lt ${#queue[@]} ]; do
        f="${queue[$i]}"; i=$((i+1))
        for dll in $("$OBJDUMP" -p "$f" | awk '/DLL Name:/{print $3}'); do
            lc="$(echo "$dll" | tr 'A-Z' 'a-z')"
            case "$seen" in *" $lc "*) continue ;; esac
            seen="$seen$lc "
            src="$(find "$SYSROOT/bin" -maxdepth 1 -iname "$dll" | head -1)"
            if [ -n "$src" ]; then
                cp "$src" "$E/"; queue+=("$E/$(basename "$src")")
            else
                echo "$lc" | grep -Eq "$SYSTEM_DLLS" || die "$dll (needed by $(basename "$f")) is neither bundled nor a Windows system DLL"
                external="$external$lc "
            fi
        done
    done
    { echo "# DLL closure of UA-Reforged-engine.exe (generated by build-windows.sh)"
      echo "bundled: $(cd "$E" && ls *.dll | tr '\n' ' ')"
      echo "system:$external"; } > "$GEN/dll-report.txt"
    echo "bundled DLLs: $(cd "$E" && ls *.dll | wc -l | tr -d ' ')"

    # the engine's own read-only files (shaders, free fonts + their licences); no game data
    cp -R "$ROOT/res" "$ROOT/fonts" "$E/"
    find "$STAGE" -name '.DS_Store' -delete
    # launcher: the script, the downloads code and the addresses + pack checksum list (shared with the Mac app)
    cp "$PKG/launcher/UA-Reforged.ps1" "$PKG/launcher/downloads.ps1" "$PKG/launcher/quest.ps1" "$STAGE/launcher/"
    cp "$ROOT/packaging/macos/downloads.txt" "$STAGE/launcher/downloads.txt"
    LC_ALL=C grep -n '[^ -~	]' "$STAGE/launcher/"*.ps1 && die "the launcher scripts must be plain ASCII (Windows PowerShell 5.1)"

    # docs + licences
    sed "s/@VERSION@/$VERSION/g" "$PKG/README-Windows.txt" > "$STAGE/README-Windows.txt"
    cp "$ROOT/LICENSE" "$STAGE/LICENSE-engine-GPL-2.0.txt"
    cp "$ROOT/CREDITS.md" "$STAGE/CREDITS.md"
    cp "$ROOT/docs/ai-tuning.example.txt" "$STAGE/ai-tuning.example.txt"
    printf 'Source code (GNU GPL version 2) of this build of the UA Reforged engine, version %s:\n    %s\n' \
        "$VERSION" "$SOURCE_URL" > "$STAGE/SOURCE.txt"
    cp "$ROOT/LICENSE" "$STAGE/licenses/UA-Reforged-engine-GPL-2.0.txt"
    [ -f "$SYSROOT/LICENSE.txt" ] && cp "$SYSROOT/LICENSE.txt" "$STAGE/licenses/FFmpeg-8.1-LGPL.txt"
    third_party > "$STAGE/licenses/THIRD-PARTY.txt"
    for l in "$SYSROOT/share/licenses/"*; do
        [ -d "$l" ] || continue
        case " $BUNDLED_PKGS " in *" $(basename "$l") "*) cp -R "$l" "$STAGE/licenses/" ;; esac
    done
    # licence texts only (some packages also put source files there)
    find "$STAGE/licenses" -type f \( -name '*.inc' -o -name '*.c' -o -name '*.h' -o -name '*.asm' \) -delete
    find "$STAGE/licenses" -type d -empty -delete
    # CRLF so Notepad on any Windows shows the text files properly
    find "$STAGE" -type f \( -name '*.txt' -o -name '*.md' -o -name '*.ps1' \) ! -path '*/engine/res/*' -print0 \
        | xargs -0 perl -pi -e 's/\r?\n/\r\n/'

    # one fixed time on every staged file (the installer records file times)
    find "$STAGE" -exec env TZ=UTC touch -h -t "$FIXED_TIME" {} +

    # uninstall list = every top-level item the installer lays down
    { echo "; generated by build-windows.sh - do not edit"
      echo "!macro UA_UNINSTALL_LIST"
      ( cd "$STAGE" && for e in *; do
          if [ -d "$e" ]; then echo "  RMDir /r \"\$INSTDIR\\$e\""; else echo "  Delete \"\$INSTDIR\\$e\""; fi
        done )
      echo "!macroend"; } > "$GEN/uninstall-list.nsh"
    du -sh "$STAGE"
}

# THIRD-PARTY.txt: which package every bundled DLL comes from (MSYS2 ucrt64 / BtbN FFmpeg), with versions + sources
third_party() {
    local E="$STAGE/engine" d p name ver
    BUNDLED_PKGS=""
    echo "Third-party libraries bundled with UA Reforged $VERSION for Windows (engine\\*.dll), unmodified."
    echo "Each keeps its own licence; texts are in this folder (one folder per package) where the package ships one."
    echo
    echo "FFmpeg 8.1 (avcodec, avformat, avutil, swresample, swscale): LGPL, a decode-only use; the BtbN"
    echo "  'win64-lgpl-shared-8.1' build. Licence: FFmpeg-8.1-LGPL.txt. Source: https://ffmpeg.org/download.html"
    echo "  (release 8.1) and the build scripts https://github.com/BtbN/FFmpeg-Builds."
    echo
    echo "MSYS2 ucrt64 packages (source packages: https://packages.msys2.org/ and https://github.com/msys2/MINGW-packages):"
    for d in "$E"/*.dll; do
        name="$(basename "$d")"
        p=""
        for pk in "$DEPS/cache/"mingw-w64-ucrt-x86_64-*.pkg.tar.zst; do
            [ -f "$pk" ] || continue
            if tar -tf "$pk" 2>/dev/null | grep -qi "^ucrt64/bin/$name\$"; then p="$(basename "$pk" -any.pkg.tar.zst)"; break; fi
        done
        if [ -n "$p" ]; then
            p="${p#mingw-w64-ucrt-x86_64-}"; ver="${p##*-}"; p="${p%-*}"; ver="${p##*-}-$ver"; p="${p%-*}"
            echo "  $name: $p $ver"
            BUNDLED_PKGS="$BUNDLED_PKGS $p"
        fi
    done
    echo
    echo "Licence texts for packages without a folder here: openal (OpenAL Soft, LGPL 2.0 or later), libvorbis"
    echo "(BSD-3-Clause) and any other: see the package's source at the addresses above."
}

installer() {
    [ -d "$STAGE" ] || die "no stage; run: $0 stage"
    local est_kb; est_kb=$(du -sk "$STAGE" | awk '{print $1}')
    log "installer: $SETUP"
    # makensis needs a UTF-8 locale (with none set it aborts with std::bad_alloc)
    LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 makensis -V2 -INPUTCHARSET UTF8 -DSTAGE="$STAGE" -DGEN="$GEN" -DOUTFILE="$SETUP" -DVERSION="$VERSION" \
        -DICON="$ROOT/svg/icon/icon.ico" -DEST_KB="$est_kb" "$PKG/installer.nsi"
    ( cd "$OUT" && shasum -a 256 "$(basename "$SETUP")" | tee "$(basename "$SETUP").sha256" )
}

zipit() {
    [ -d "$STAGE" ] || die "no stage; run: $0 stage"
    log "zip: $ZIP"
    rm -f "$ZIP"
    # sorted entries, one fixed time, no extra fields, no owner ids, no comments
    python3 -I - "$OUT/stage" "UA Reforged" "$ZIP" "$FIXED_TIME" <<'PY'
import os, sys, zipfile
root, top, out, t = sys.argv[1:5]
when = (int(t[0:4]), int(t[4:6]), int(t[6:8]), int(t[8:10]), int(t[10:12]), 0)
paths = []
for d, dirs, files in os.walk(os.path.join(root, top)):
    dirs.sort()
    rel = os.path.relpath(d, root)
    paths.append(rel + '/')
    paths += [os.path.join(rel, f) for f in sorted(files)]
with zipfile.ZipFile(out, 'w') as z:
    for p in paths:
        zi = zipfile.ZipInfo(p, when)
        zi.create_system = 0          # MS-DOS: no Unix mode/owner bits
        zi.external_attr = 0x10 if p.endswith('/') else 0
        if p.endswith('/'):
            z.writestr(zi, b'')
        else:
            zi.compress_type = zipfile.ZIP_DEFLATED
            with open(os.path.join(root, p), 'rb') as f:
                z.writestr(zi, f.read(), compresslevel=9)
PY
    ( cd "$OUT" && shasum -a 256 "$(basename "$ZIP")" | tee "$(basename "$ZIP").sha256" )
}

case "$STEP" in
    deps) deps ;;
    engine) engine ;;
    stage) stage ;;
    installer) installer ;;
    zip) zipit ;;
    all) engine; stage; installer; zipit ;;
    *) die "unknown step: $STEP" ;;
esac
ls -l "$OUT"/*.exe "$OUT"/*.zip 2>/dev/null || true

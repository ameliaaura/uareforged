#!/bin/bash
# Tests for the launcher downloads carrying on where they stopped (packaging/macos/downloads.sh and
# packaging/linux/downloads.sh: dl_precheck, dl_fetch, dl_again, game_get, pack_get). A local server
# (dl_test_server.py) drops connections half way, ends transfers early, stalls, ignores or refuses Range requests,
# sends a damaged file and serves a new version of a file; downloads are stopped and the launcher "restarted".
# No network beyond 127.0.0.1, no dialog (ask is a stub that logs), no browser.
#   tests/launcher/download_resume_test.sh [mac|linux]     (default: this system's)
# Needs bash, python3, curl (Linux: coreutils; on a Mac the Linux run needs GNU stat + sha256sum as gstat/gsha256sum).
# Exit 0 = all passed.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PLAT="${1:-$(case "$(uname -s)" in Darwin) echo mac ;; *) echo linux ;; esac)}"
T="$(mktemp -d "${TMPDIR:-/tmp}/ua-dl-test.XXXXXX")"; SRV=""; trap 'kill $SRV 2>/dev/null; rm -rf "$T"' EXIT
PASS=0; FAIL=0
ok() { PASS=$((PASS + 1)); echo "ok   $1"; }
bad() { FAIL=$((FAIL + 1)); echo "FAIL $1"; [ -n "${2:-}" ] && [ -f "$2" ] && sed 's/^/     | /' "$2" | tail -40; }
sha() { python3 -I -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"; }
size() { python3 -I -c 'import os,sys; print(os.path.getsize(sys.argv[1]))' "$1"; }

# the launcher files, laid out as the app has them
if [ "$PLAT" = mac ]; then
    APP="$T/app/Contents"; mkdir -p "$APP/Resources"
    cp "$ROOT/packaging/macos/"{downloads.sh,downloads.txt,links.js,progress.js} "$APP/Resources/"
    SRC='CONTENTS="$APP"; . "$CONTENTS/Resources/downloads.sh"'
    TPATH=/usr/bin:/bin:/usr/sbin:/sbin
else
    APP="$T/app"; S="$APP/usr/share/ua-reforged"; mkdir -p "$S"
    cp "$ROOT/packaging/linux/"{downloads.sh,links.py} "$S/"; cp "$ROOT/packaging/macos/downloads.txt" "$S/"
    SRC='HERE="$APP"; SHARE="$HERE/usr/share/ua-reforged"; DIALOG=/bin/false; . "$SHARE/downloads.sh"'
    TPATH=/usr/local/bin:/usr/bin:/bin
    if [ "$(uname -s)" = Darwin ]; then   # GNU tools under their own names (Homebrew coreutils)
        mkdir -p "$T/gnu"; ln -s "$(command -v gstat)" "$T/gnu/stat"; ln -s "$(command -v gsha256sum)" "$T/gnu/sha256sum"
        TPATH="$T/gnu:/usr/bin:/bin:/usr/sbin:/sbin"
    fi
fi

# the files the server offers: a zip-looking file of 3 MB (the precheck wants the zip signature), a second version of
# it (same size, other bytes) and a pack of two parts
FILES="$T/srv"; mkdir -p "$FILES"
mkfile() { python3 -I -c 'import os,sys; open(sys.argv[1],"wb").write(b"PK\x03\x04" + os.urandom(int(sys.argv[2]) - 4))' "$1" "$2"; }
mkfile "$FILES/game.zip" 3000000; mkfile "$FILES/game-v2.zip" 3000000; mkfile "$FILES/part1.zip" 1500000; mkfile "$FILES/part2.zip" 1200000
GSUM="$(sha "$FILES/game.zip")"
PORT=$(python3 -I -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
U="http://127.0.0.1:$PORT"
python3 -I "$ROOT/tests/launcher/dl_test_server.py" "$FILES" "$PORT" "$T/server.log" 2>"$T/server.err" & SRV=$!
for i in $(seq 50); do curl -s -o /dev/null "$U/ok/game.zip" -r 0-1023 && break; sleep 0.1; done
: > "$T/server.log"

# fetch <case> <url> <file name> <bytes> <sha or -> [env ...]: one launcher run of dl_precheck + dl_fetch into a support
# folder of its own (kept between runs of the same case: a "restart"); logs to $T/<case>.log, status lines to .status
fetch() {
    local c=$1 url=$2 name=$3 bytes=$4 sum=$5; shift 5
    mkdir -p "$T/sup-$c"
    env -i PATH="$TPATH" HOME="$T" APP="$APP" UA_PACK_NOUI=1 UA_DL_WAITS="${WAITS:-0 0 0 0}" UA_DL_STALL_SECS=3 "$@" /bin/bash -c "
        SUPPORT=\"\$1\"; PACK=\"\$SUPPORT/reforge-pack\"; GAME=''
        $SRC
        dl_status() { printf '%s\t%s\n' \"\$1\" \"\$2\" >> \"\$SUPPORT/../$c.status\"; }
        DL_DIR=\"\$SUPPORT/dl\"; mkdir -p \"\$DL_DIR\"; UI_DIR=\"\$DL_DIR\"
        DL_WHAT=Downloading; DL_BASE=0
        dl_precheck \"\$2\" 0 0 \"\$4\" || { echo \"RESULT precheck \$PC_WHY net=\$PC_NET\"; exit 3; }
        DL_TOTAL=\"\$4\"
        dl_fetch \"\$2\" \"\$DL_DIR/\$3\" \"\$4\" \"\$5\"; rc=\$?
        echo \"RESULT rc=\$rc net=\$DL_NET badsum=\$DL_BADSUM why=\$DL_WHY\"; exit \$rc
    " _ "$T/sup-$c" "$url" "$name" "$bytes" "$( [ "$sum" = - ] && echo '' || echo "$sum")" >> "$T/$c.log" 2>&1
}
part() { echo "$T/sup-$1/partial-downloads/$2.part"; }
got() { echo "$T/sup-$1/dl/$2"; }
reqs() { grep -c "^$1 $2 " "$T/server.log"; }   # download + check requests seen for mode/name

# --- 1. plain download: file in place, no partial left
fetch ok "$U/ok/game.zip" game.zip 3000000 "$GSUM"
[ "$(sha "$(got ok game.zip)")" = "$GSUM" ] && [ ! -e "$(part ok game.zip)" ] && [ ! -e "$(part ok game.zip).id" ] \
    && ok "plain download: right file, partial and its id removed" || bad "plain download" "$T/ok.log"

# --- 2. connection drops at 30 % of each response: carries on with Range requests until done
fetch drop "$U/drop/game.zip" game.zip 3000000 "$GSUM"
r="$(grep '^drop game.zip range=[0-9]' "$T/server.log" | awk '{print $3}' | tr '\n' ' ')"
if [ "$(sha "$(got drop game.zip)" 2>/dev/null)" = "$GSUM" ] && grep -q 'RESULT rc=0' "$T/drop.log" \
   && [ "$(grep -c '^drop game.zip range=[1-9]' "$T/server.log")" -ge 3 ]; then ok "drops every 30 %: carried on 3 times ($r), file right"
else bad "drops every 30 % ($r)" "$T/drop.log"; fi

# --- 3. transfer ends early without an error (no Content-Length, as seen from the game's host): carries on
fetch close "$U/close/game.zip" game.zip 3000000 ""
if [ "$(sha "$(got close game.zip)" 2>/dev/null)" = "$GSUM" ] && grep -q 'connection dropped' "$T/close.log"; then
    ok "transfer ends early without an error: noticed by the size, carried on, file right (no SHA-256 given)"
else bad "transfer ends early" "$T/close.log"; fi

# --- 4. stall: no data for UA_DL_STALL_SECS (3 s here; 60 s in the app): dropped, carried on
t0=$SECONDS; fetch stall "$U/stall/game.zip" game.zip 3000000 "$GSUM"
if [ "$(sha "$(got stall game.zip)" 2>/dev/null)" = "$GSUM" ] && grep -q 'stalled' "$T/stall.log" && grep -q '^stall game.zip range=900000 ' "$T/server.log"; then
    ok "stall: given up after the stall time ($((SECONDS - t0)) s), carried on from 30 %, file right"
else bad "stall" "$T/stall.log"; fi

# --- 5. server ignores the Range request (200, whole file): starts over, file right
fetch norange "$U/norange/game.zip" game.zip 3000000 "$GSUM"
if [ "$(sha "$(got norange game.zip)" 2>/dev/null)" = "$GSUM" ] && grep -q 'does not carry on downloads' "$T/norange.log"; then
    ok "server ignores Range (200): started over, file right"
else bad "server ignores Range" "$T/norange.log"; fi

# --- 6. server refuses the Range request (416): starts over, file right
fetch r416 "$U/r416/game.zip" game.zip 3000000 "$GSUM"
if [ "$(sha "$(got r416 game.zip)" 2>/dev/null)" = "$GSUM" ] && grep -q 'HTTP 416' "$T/r416.log"; then
    ok "server refuses Range (416): started over, file right"
else bad "server refuses Range (416)" "$T/r416.log"; fi

# --- 7. complete but wrong SHA-256 once: deleted, downloaded once more from the start
fetch badonce "$U/badonce/game.zip" game.zip 3000000 "$GSUM"
if [ "$(sha "$(got badonce game.zip)" 2>/dev/null)" = "$GSUM" ] && grep -q 'once more from the start' "$T/badonce.log" \
   && [ "$(grep -c '^badonce game.zip range=- ' "$T/server.log")" = 2 ]; then ok "wrong SHA-256 once: downloaded once more, file right"
else bad "wrong SHA-256 once" "$T/badonce.log"; fi

# --- 8. always the wrong SHA-256: gives up after the one repeat, nothing kept, not a connection problem
fetch bad "$U/bad/game.zip" game.zip 3000000 "$GSUM"
if grep -q 'RESULT rc=1 net=0 badsum=1' "$T/bad.log" && [ ! -e "$(got bad game.zip)" ] && [ ! -e "$(part bad game.zip)" ] \
   && [ "$(grep -c '^bad game.zip range=- ' "$T/server.log")" = 2 ]; then ok "always the wrong SHA-256: 2 downloads, then refused, nothing kept"
else bad "always the wrong SHA-256" "$T/bad.log"; fi

# --- 9. the connection gives nothing: 5 tries in a row, then a connection failure (Try again), with the waits between
WAITS="1 1 1 1" fetch dead "$U/dead/game.zip" game.zip 3000000 "$GSUM"; t1=$(grep -c '^dead game.zip range=- ' "$T/server.log")
if grep -q 'RESULT rc=1 net=1' "$T/dead.log" && [ "$t1" = 5 ] && grep -q 'it stopped, carrying on in 1 s' "$T/dead.status"; then
    ok "no data at all: 5 tries (waits between them shown), then given up as a connection problem"
else bad "no data at all (tries $t1)" "$T/dead.log"; fi

# --- 10. stopped half way (the app quit / crashed), started again: carries on from the partial download
( fetch restart "$U/slow/game.zip" game.zip 3000000 "$GSUM" ) & FP=$!
for i in $(seq 100); do [ "$(size "$(part restart game.zip)" 2>/dev/null || echo 0)" -gt 600000 ] && break; sleep 0.1; done
pkill -P $FP 2>/dev/null; kill $FP 2>/dev/null; wait $FP 2>/dev/null; sleep 0.5
pkill -f "curl.*$U/slow/game.zip" 2>/dev/null; sleep 0.3
h=$(size "$(part restart game.zip)" 2>/dev/null || echo 0)
fetch restart "$U/ok/game.zip" game.zip 3000000 "$GSUM"
start=$(grep '^ok game.zip range=[1-9]' "$T/server.log" | tail -1 | sed 's/.*range=\([0-9]*\).*/\1/')
if [ "$h" -gt 0 ] && [ "${start:-0}" = "$h" ] && [ "$(sha "$(got restart game.zip)" 2>/dev/null)" = "$GSUM" ] && grep -q 'carrying on from' "$T/restart.log"; then
    ok "restart: carried on from the partial download ($h bytes; Range start $start), file right"
else bad "restart (partial $h, range start ${start:-none})" "$T/restart.log"; fi

# --- 11. Cancel half way: 130, the partial download stays; next run carries on
( fetch cancel "$U/slow/game.zip" game.zip 3000000 "$GSUM" ) & FP=$!
for i in $(seq 100); do [ "$(size "$(part cancel game.zip)" 2>/dev/null || echo 0)" -gt 400000 ] && break; sleep 0.1; done
touch "$T/sup-cancel/dl/cancel"; wait $FP; rm -f "$T/sup-cancel/dl/cancel"
h=$(size "$(part cancel game.zip)" 2>/dev/null || echo 0)
: > "$T/cancel.status"
fetch cancel "$U/ok/game.zip" game.zip 3000000 "$GSUM"
p0=$(awk -F'\t' '/Downloading/ {print $1; exit}' "$T/cancel.status")
if grep -q 'RESULT rc=130' "$T/cancel.log" && [ "$h" -gt 0 ] && grep -q 'carrying on from' "$T/cancel.log" \
   && [ "$(sha "$(got cancel game.zip)" 2>/dev/null)" = "$GSUM" ]; then ok "Cancel: partial download kept ($h bytes), the next run carried on, file right"
else bad "Cancel ($h)" "$T/cancel.log"; fi
if [ "${p0:-0}" -ge $((h * 90 / 3000000)) ] && [ "${p0:-0}" -gt 0 ]; then ok "progress after the restart starts at ${p0} % (not 0 %)"
else bad "progress after the restart starts at ${p0:-?} %" "$T/cancel.status"; fi

# --- 12. a new version of the file on the server (same size, new tag): the old partial is not used
( fetch ver "$U/slow/game.zip" game.zip 3000000 - ) & FP=$!
for i in $(seq 100); do [ "$(size "$(part ver game.zip)" 2>/dev/null || echo 0)" -gt 400000 ] && break; sleep 0.1; done
touch "$T/sup-ver/dl/cancel"; wait $FP; rm -f "$T/sup-ver/dl/cancel"
cp "$FILES/game.zip" "$FILES/game-v1.zip"; sleep 1; cp "$FILES/game-v2.zip" "$FILES/game.zip"   # new bytes, new mtime = new ETag
fetch ver "$U/ok/game.zip" game.zip 3000000 -
if grep -q 'another version' "$T/ver.log" && [ "$(sha "$(got ver game.zip)" 2>/dev/null)" = "$(sha "$FILES/game-v2.zip")" ]; then
    ok "new version on the server (new tag): old partial discarded, new file downloaded whole"
else bad "new version on the server" "$T/ver.log"; fi
cp "$FILES/game-v1.zip" "$FILES/game.zip"
# a partial that belongs to another SHA-256 (e.g. a pack part of an older release) is not used either
( fetch ver2 "$U/slow/game.zip" game.zip 3000000 "$GSUM" ) & FP=$!
for i in $(seq 100); do [ "$(size "$(part ver2 game.zip)" 2>/dev/null || echo 0)" -gt 400000 ] && break; sleep 0.1; done
touch "$T/sup-ver2/dl/cancel"; wait $FP; rm -f "$T/sup-ver2/dl/cancel"
sed -i.bak 's/sha256=[0-9a-f]*/sha256=0000000000000000000000000000000000000000000000000000000000000000/' "$(part ver2 game.zip).id"
fetch ver2 "$U/ok/game.zip" game.zip 3000000 "$GSUM"
if grep -q 'another version' "$T/ver2.log" && [ "$(sha "$(got ver2 game.zip)" 2>/dev/null)" = "$GSUM" ]; then
    ok "partial for another SHA-256: discarded, file right"
else bad "partial for another SHA-256" "$T/ver2.log"; fi
# a partial for a file of another size (the precheck sees the new size): discarded
mkdir -p "$T/sup-ver3/partial-downloads"; head -c 500000 "$FILES/part1.zip" > "$(part ver3 part1.zip)"
printf 'bytes=1400000 sha256=%s tag=-\n' "$(sha "$FILES/part1.zip")" > "$(part ver3 part1.zip).id"
fetch ver3 "$U/ok/part1.zip" part1.zip 1500000 "$(sha "$FILES/part1.zip")"
if grep -q 'another version' "$T/ver3.log" && [ "$(sha "$(got ver3 part1.zip)" 2>/dev/null)" = "$(sha "$FILES/part1.zip")" ]; then
    ok "partial for another file size: discarded, file right"
else bad "partial for another file size" "$T/ver3.log"; fi

# --- 13. game_get: the connection keeps failing -> "Try again" (carries on) -> game downloaded
# flaky6: 5 empty tries (first run gives up), Try again, 1 more empty, then drops every 30 % (carries on)
setup() { # setup <case> <kind game|pack> <env...>: game_get / pack_get as the launcher runs them
    local c=$1 kind=$2; shift 2; mkdir -p "$T/sup-$c"
    env -i PATH="$TPATH" HOME="$T" APP="$APP" UA_PACK_NOUI=1 UA_DL_WAITS="0 0 0 0" UA_DL_STALL_SECS=3 UA_LINKS_URL="$U/none/links.json" "$@" /bin/bash -c "
        SUPPORT=\"\$1\"; PACK=\"\$SUPPORT/reforge-pack\"; GAME=''
        $SRC
        ask() { echo \"ASK [\$1] buttons [\${*:3}] default [\$2]\"; printf '%s' \"\${ASK_ANSWER:-}\"; }
        if [ \"\$2\" = game ]; then game_get; echo \"RESULT game_get rc=\$?\"
        else links_load; pack_get; echo \"RESULT pack_get rc=\$?\"; fi
    " _ "$T/sup-$c" "$kind" >> "$T/$c.log" 2>&1
}
# the game zip must unpack: serve a real zip of a fake game folder
python3 -I - "$FILES/realgame.zip" <<'EOF'
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_STORED) as z:
    for n in ['UA/DATA/world.ini', 'UA/DATA/SET1/x', 'UA/DATA/SCRIPTS/x', 'UA/DATA/SOUNDS/x'] + ['UA/LEVELS/SINGLE/L%02d.LDF' % i for i in range(12)]:
        z.writestr(n, os.urandom(120000))
EOF
DLT="$APP/$( [ "$PLAT" = mac ] && echo Resources/downloads.txt || echo usr/share/ua-reforged/downloads.txt)"
cp "$DLT" "$T/dl.orig"; sed 's/^game_min_bytes	.*/game_min_bytes	1000/' "$T/dl.orig" > "$DLT"   # the test game is small
setup try game UA_GAME_URL="$U/flaky6/realgame.zip" UA_GAME_AUTO=get UA_RETRY_AUTO=again UA_NOTICE_AUTO=later
if grep -q 'The download keeps stopping. Check your connection and press Try again; it will carry on where it stopped' "$T/try.log" \
   || grep -q 'The download could not get going. Check your connection and press Try again.' "$T/try.log"; then
    ok "game: connection keeps failing -> the Try again notice"
else bad "game: Try again notice" "$T/try.log"; fi
if grep -q 'again: again' "$T/try.log" && grep -q 'get the game: installed' "$T/try.log" && grep -q 'RESULT game_get rc=0' "$T/try.log"; then
    ok "game: Try again carried on and the game was set up"
else bad "game: Try again" "$T/try.log"; fi
# Not now: back to the choices, the partial download stays, the next start carries on
setup later game UA_GAME_URL="$U/flaky99/realgame.zip" UA_GAME_AUTO=get UA_NOTICE_AUTO=later
if grep -q 'again: later' "$T/later.log" && grep -q 'RESULT game_get rc=1' "$T/later.log" && grep -q 'not now (the partial download stays' "$T/later.log"; then
    ok "game: Not now -> back to the choices (rc 1), nothing broken"
else bad "game: Not now" "$T/later.log"; fi
# --- 14. pack_get: Try again carries on, parts checked, Not now keeps the parts for the launcher's Resume
P1=$(sha "$FILES/part1.zip"); P2=$(sha "$FILES/part2.zip")
{ grep -v '^part	\|^url	' "$APP/$( [ "$PLAT" = mac ] && echo Resources/downloads.txt || echo usr/share/ua-reforged/downloads.txt)"; } > "$T/dl.txt"
printf 'url\t%s/flaky6/\npart\tpart1.zip\t1500000\t%s\npart\tpart2.zip\t1200000\t%s\n' "$U" "$P1" "$P2" >> "$T/dl.txt"
cp "$T/dl.txt" "$DLT"
setup pack pack UA_PACK_AUTO=yes UA_RETRY_AUTO=2 UA_NOTICE_AUTO=later
# (the parts are not a real pack: it stops when it unpacks them, after both parts were downloaded and checked)
if [ "$(grep -c 'again: again' "$T/pack.log")" = 2 ] && grep -q 'does not unpack (part1.zip)\|not a Reforge pack' "$T/pack.log" \
   && ! grep -q 'damaged or not the expected' "$T/pack.log"; then ok "pack: Try again (twice) carried on, both parts downloaded and SHA-256 checked"
else bad "pack: Try again" "$T/pack.log"; fi
printf 'url\t%s/flaky99/\npart\tpart1.zip\t1500000\t%s\npart\tpart2.zip\t1200000\t%s\n' "$U" "$P1" "$P2" > "$T/parts"
{ grep -v '^part	\|^url	' "$T/dl.orig"; cat "$T/parts"; } > "$DLT"
# a half-downloaded part 1 from an earlier try is here; the connection now gives nothing
mkdir -p "$T/sup-packl/partial-downloads"; head -c 700000 "$FILES/part1.zip" > "$(part packl part1.zip)"
printf 'bytes=1500000 sha256=%s tag=%s\n' "$P1" "$(python3 -I -c 'import os,sys; s=os.stat(sys.argv[1]); print("\"%x-%x\"" % (s.st_size, s.st_mtime_ns))' "$FILES/part1.zip")" > "$(part packl part1.zip).id"
setup packl pack UA_PACK_AUTO=yes UA_NOTICE_AUTO=later
if grep -q 'again: later' "$T/packl.log" && grep -q 'partial download stays' "$T/packl.log" && [ "$(size "$(part packl part1.zip)")" = 700000 ] \
   && grep -q '700 KB of 2 MB are already downloaded' "$T/packl.log"; then ok "pack: Not now keeps the partial part; the notice says how much is here (700 KB of 2 MB)"
else bad "pack: Not now" "$T/packl.log"; fi
# a stopped part kept by an older app version in the pack folder carries on
mkdir -p "$T/sup-packo/reforge-pack-download"; head -c 600000 "$FILES/part1.zip" > "$T/sup-packo/reforge-pack-download/part1.zip"
printf 'url\t%s/ok/\npart\tpart1.zip\t1500000\t%s\npart\tpart2.zip\t1200000\t%s\n' "$U" "$P1" "$P2" > "$T/parts"
{ grep -v '^part	\|^url	' "$T/dl.orig"; cat "$T/parts"; } > "$DLT"
setup packo pack UA_PACK_AUTO=yes UA_NOTICE_AUTO=later
if grep -q 'carrying on from 600 KB' "$T/packo.log" && grep -q '^ok part1.zip range=600000 ' "$T/server.log"; then
    ok "pack: a part an older version stopped carries on (Range from 600000)"
else bad "pack: older version's part" "$T/packo.log"; fi
cp "$T/dl.orig" "$DLT"

echo "$PASS passed, $FAIL failed ($PLAT)"
[ $FAIL -eq 0 ]

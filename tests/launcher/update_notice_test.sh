#!/bin/bash
# Tests for the Mac app's new-version notice (packaging/macos: links.js "latest", downloads.sh update_check, ver_cmp).
# No network: links.json comes from a file (UA_LINKS_FILE) or a local server on 127.0.0.1; no dialog is ever shown
# (ask is replaced by a stub that logs "DIALOG"), no browser is opened.
#   tests/launcher/update_notice_test.sh [--old <links.js of an earlier release> ...]
# --old: also runs that earlier validator on the new links.json (it must accept the file and give the same lines as
# for the file without "latest"). Exit 0 = all passed.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OLD=(); while [ $# -gt 0 ]; do case "$1" in --old) OLD+=("$2"); shift 2 ;; *) echo "unknown argument $1"; exit 2 ;; esac; done
T="$(mktemp -d "${TMPDIR:-/tmp}/ua-update-test.XXXXXX")"; trap 'kill $SRV $SLOW 2>/dev/null; rm -rf "$T"' EXIT
SRV=""; SLOW=""
PASS=0; FAIL=0
ok() { PASS=$((PASS + 1)); echo "ok   $1"; }
bad() { FAIL=$((FAIL + 1)); echo "FAIL $1"; [ -n "${2:-}" ] && sed 's/^/     | /' "$2"; }

mkdir -p "$T/app/Contents/Resources" "$T/fx"
cp "$ROOT/packaging/macos/"{downloads.sh,links.js} "$T/app/Contents/Resources/"
BASE="$(cat "$ROOT/packaging/macos/downloads.txt")"
# set_version <v or empty>: this app's version, as build-mac.sh writes it
set_version() { { printf '%s\n' "$BASE"; [ -n "$1" ] && printf 'app_version\t%s\n' "$1"; } > "$T/app/Contents/Resources/downloads.txt"; }

# links.json as the site serves it, plus "latest"
LINKS_BASE='"game_url": "https://metropolisdawn.de/filebase/download/5/", "game_min_bytes": 100000000,
  "help_url": "https://uareforged.pages.dev/downloads/#nogame-h", "pack_url": "https://pub-9574664f67b04217aa2ca93f509638ad.r2.dev/pack/",
  "pack_parts": [{"name": "UA-Reforged-pack-0.8.0-1of2.zip", "bytes": 699598021, "sha256": "afb815b7de26a078097d9ef0fd2190f2434655232f4e903d3e822019b3eca98f"}],
  "quest_parts": [{"name": "UA-Reforged-Quest-pack-0.9.0.zip", "bytes": 712134306, "sha256": "e2b5447e831dfd75afa0ba1c0b193546e4fa9bce330860fb130faaa52f1dfb78"}],
  "message": "Test notice"'
REL=https://github.com/ameliaaura/uareforged/releases/tag
fx() { printf '{ %s%s }\n' "$LINKS_BASE" "${2:+, $2}" > "$T/fx/$1.json"; }
lat() { printf '"latest": {"mac": {"version": "%s", "url": "%s"}, "windows": {"version": "%s", "url": "%s/v%s"}}' "$1" "${2:-$REL/v$1}" "$1" "$REL" "$1"; }
fx none ""
fx v086 "$(lat 0.8.6)"; fx v085 "$(lat 0.8.5)"; fx v084 "$(lat 0.8.4)"; fx v0841 "$(lat 0.8.4.1)"; fx v0850 "$(lat 0.8.5.0)"
fx v087 "$(lat 0.8.7)"; fx v0810 "$(lat 0.8.10)"; fx site "$(lat 0.8.6 https://uareforged.pages.dev/downloads/#mac-h)"
fx winonly '"latest": {"windows": {"version": "0.9.0", "url": "https://uareforged.pages.dev/downloads/"}}'
fx future '"latest": {"mac": {"version": "0.8.6", "url": "https://uareforged.pages.dev/downloads/", "notes": "x"}, "linux": {"version": "1.0", "url": "https://uareforged.pages.dev/"}, "ios": 5}'
# malformed "latest" (each must be ignored on its own, the rest of the file still used)
fx m_vprefix "$(lat v0.8.6 $REL/v0.8.6)"; fx m_vbeta "$(lat 0.8.6-beta $REL/v0.8.6)"; fx m_vone "$(lat 1 $REL/v1)"
fx m_vfive "$(lat 0.8.6.1.2 $REL/v0)"; fx m_vlong "$(lat 0.8.1234567 $REL/v0)"
fx m_vnum '"latest": {"mac": {"version": 0.86, "url": "https://uareforged.pages.dev/"}}'
fx m_vnl '"latest": {"mac": {"version": "0.8.6\n", "url": "https://uareforged.pages.dev/"}}'
fx m_http "$(lat 0.8.6 http://uareforged.pages.dev/downloads/)"; fx m_host "$(lat 0.8.6 https://evil.example/uareforged)"
fx m_repo "$(lat 0.8.6 https://github.com/ameliaaura/uareforged-evil/releases)"; fx m_owner "$(lat 0.8.6 https://github.com/someone/uareforged)"
fx m_dots "$(lat 0.8.6 https://github.com/ameliaaura/uareforged/../../someone/x)"; fx m_query "$(lat 0.8.6 'https://uareforged.pages.dev/?x=1')"
fx m_port "$(lat 0.8.6 https://uareforged.pages.dev:8443/)"; fx m_sub "$(lat 0.8.6 https://x.uareforged.pages.dev/)"
fx m_userinfo "$(lat 0.8.6 https://uareforged.pages.dev@evil.example/)"; fx m_pct "$(lat 0.8.6 https://github.com/ameliaaura/uareforged/%2e%2e/x)"
fx m_quote '"latest": {"mac": {"version": "0.8.6", "url": "https://uareforged.pages.dev/\"x"}}'
fx m_nourl '"latest": {"mac": {"version": "0.8.6"}}'
fx m_array '"latest": [{"mac": "0.8.6"}]'; fx m_str '"latest": "0.8.6"'; fx m_null '"latest": null'
fx m_macstr '"latest": {"mac": "0.8.6"}'; fx m_macarr '"latest": {"mac": ["0.8.6"]}'
printf '{ "latest": {"mac": {"version": "0.8.6", "url": "%s/v0.8.6"} }\n' "$REL" > "$T/fx/badjson.json"

# run <name> <expect> <version> <env...>: one launcher start up to the point where the game would start
run() {
    local name=$1; shift; local ver=$1; shift
    set_version "$ver"
    mkdir -p "$T/sup-$name"
    env -i PATH=/usr/bin:/bin HOME="$T" "$@" /bin/bash -c '
        CONTENTS="$1"; SUPPORT="$2"; PACK="$SUPPORT/reforge-pack"; GAME="$3"
        is_game_dir() { [ -d "$1/DATA" ] && [ -d "$1/LEVELS" ]; }
        . "$CONTENTS/Resources/downloads.sh"
        ask() { echo "DIALOG [$1] buttons [${*:3}] default [$2]" >&2; printf "%s" "${ASK_ANSWER:-Later}"; }
        links_prefetch
        t0=$(perl -MTime::HiRes=time -e "printf q(%.3f), time")
        update_check
        t1=$(perl -MTime::HiRes=time -e "printf q(%.3f), time")
        echo "CHECK SECONDS $(echo "$t1 - $t0" | bc)"
        echo "GAME STARTS"
    ' _ "$T/app/Contents" "$T/sup-$name" "${GAMEDIR:-$T/game}" > "$T/log-$name.txt" 2>&1
}
SUP() { echo "$T/sup-$1"; }
mkdir -p "$T/game/DATA" "$T/game/LEVELS"
expect() { # expect <name> <description> <grep -E pattern that must match> [pattern that must NOT match]
    local f="$T/log-$1.txt"
    if grep -Eq "$3" "$f" && grep -q 'GAME STARTS' "$f" && { [ -z "${4:-}" ] || ! grep -Eq "$4" "$f"; }; then ok "$1: $2"; else bad "$1: $2" "$f"; fi
}
F() { echo "UA_LINKS_FILE=$T/fx/$1.json"; }

# --- versions
run newer 0.8.5 "$(F v086)" ASK_ANSWER=Later
expect newer "0.8.6 > 0.8.5: one dialog, Later" 'DIALOG \[A new version of UA Reforged \(0\.8\.6\) is out\.' 'update page'
grep -q "buttons \[Don't remind me for this version Later Download\] default \[Download\]" "$T/log-newer.txt" && ok "newer: buttons + default" || bad "newer: buttons" "$T/log-newer.txt"
grep -q 'You have version 0.8.5' "$T/log-newer.txt" && ok "newer: names this version" || bad "newer: names this version" "$T/log-newer.txt"
[ ! -f "$(SUP newer)/update-skip.txt" ] && ok "newer: Later stores nothing" || bad "newer: Later stored a skip"
grep -q 'links: .* accepted' "$T/log-newer.txt" && ok "newer: links.json accepted" || bad "newer: links accepted" "$T/log-newer.txt"
run same 0.8.5 "$(F v085)";   expect same "0.8.5 = 0.8.5: no dialog" 'up to date \(latest 0\.8\.5\)' DIALOG
run older 0.8.5 "$(F v084)";  expect older "0.8.4 < 0.8.5: no dialog" 'up to date \(latest 0\.8\.4\)' DIALOG
run p0850 0.8.5 "$(F v0850)"; expect p0850 "0.8.5.0 = 0.8.5: no dialog" 'up to date' DIALOG
run p4a 0.8.4.1 "$(F v085)";  expect p4a "0.8.5 > 0.8.4.1: dialog" 'DIALOG \[A new version of UA Reforged \(0\.8\.5\)'
run p4b 0.8.4.1 "$(F v084)";  expect p4b "0.8.4 < 0.8.4.1: no dialog" 'up to date' DIALOG
run p4c 0.8.4 "$(F v0841)";   expect p4c "0.8.4.1 > 0.8.4: dialog" 'DIALOG \[A new version of UA Reforged \(0\.8\.4\.1\)'
run p4d 0.8.4.1 "$(F v0841)"; expect p4d "0.8.4.1 = 0.8.4.1: no dialog" 'up to date' DIALOG
run p10 0.8.9 "$(F v0810)";   expect p10 "0.8.10 > 0.8.9 (numeric, not text): dialog" 'DIALOG \[A new version of UA Reforged \(0\.8\.10\)'
run site 0.8.5 "$(F site)" UA_UPDATE_AUTO=download; expect site "uareforged.pages.dev URL accepted; Download opens it" 'update page: https://uareforged.pages.dev/downloads/#mac-h \(test run'

run future 0.8.5 "$(F future)"; expect future "unknown platforms and extra keys inside latest: tolerated" 'DIALOG \[A new version of UA Reforged \(0\.8\.6\)'
run winonly 0.8.5 "$(F winonly)"; expect winonly "latest without a mac entry: no dialog" 'no version information' 'DIALOG|ignored'
run none 0.8.5 "$(F none)";   expect none "no latest field (today's site): no dialog" 'no version information' DIALOG
run nover "" "$(F v086)";     expect nover "build without app_version: no dialog" 'this build has no version' DIALOG

# --- the three answers
run dl 0.8.5 "$(F v086)" UA_UPDATE_AUTO=download; expect dl "test answer download: page logged, not opened" "update page: $REL/v0\.8\.6 \(test run: not opened\)" DIALOG
run later 0.8.5 "$(F v086)" UA_UPDATE_AUTO=later; expect later "test answer later: nothing kept" 'test answer later' 'update page|no reminder'
# skip: kept per version; same version stays quiet, a newer one asks again
run skip1 0.8.5 "$(F v086)" UA_UPDATE_AUTO=skip
[ "$(cat "$(SUP skip1)/update-skip.txt" 2>/dev/null)" = 0.8.6 ] && ok "skip: update-skip.txt = 0.8.6" || bad "skip: update-skip.txt" "$T/log-skip1.txt"
mkdir -p "$(SUP skip2)" "$(SUP skip3)" "$(SUP skip4)"; cp "$(SUP skip1)/update-skip.txt" "$(SUP skip2)/"; cp "$(SUP skip1)/update-skip.txt" "$(SUP skip3)/"
run skip2 0.8.5 "$(F v086)"; expect skip2 "skipped 0.8.6, site still 0.8.6: no dialog" 'not reminding \(skipped\)' DIALOG
run skip3 0.8.5 "$(F v087)"; expect skip3 "skipped 0.8.6, site now 0.8.7: asks again" 'DIALOG \[A new version of UA Reforged \(0\.8\.7\)'
printf 'junk; rm -rf /\n' > "$(SUP skip4)/update-skip.txt"
run skip4 0.8.5 "$(F v086)"; expect skip4 "unreadable skip file: ignored, asks" 'DIALOG'
run skip5 0.8.5 "$(F v086)" ASK_ANSWER="Don't remind me for this version"
[ "$(cat "$(SUP skip5)/update-skip.txt" 2>/dev/null)" = 0.8.6 ] && ok "skip via the dialog button" || bad "skip via the dialog button" "$T/log-skip5.txt"

# --- malformed: only "latest" is dropped; the rest of links.json still counts; never a dialog
for m in m_vprefix m_vbeta m_vone m_vfive m_vlong m_vnum m_vnl m_http m_host m_repo m_owner m_dots m_query m_port m_sub m_userinfo m_pct m_quote m_nourl m_array m_str m_null m_macstr m_macarr; do
    run "$m" 0.8.5 "$(F "$m")"
    expect "$m" "malformed latest ignored, links still accepted" 'links: .*\(ignored; no new-version notice\)' 'DIALOG|rejected'
    grep -q 'links: .* accepted' "$T/log-$m.txt" || bad "$m: links.json no longer accepted" "$T/log-$m.txt"
done
run badjson 0.8.5 "$(F badjson)"; expect badjson "bad JSON: rejected as before, no dialog" 'rejected \(not valid JSON\)' DIALOG
run nofile 0.8.5 UA_LINKS_FILE="$T/fx/missing.json"; expect nofile "no file: built-in links, no dialog" 'not reachable' DIALOG

# --- the real background read (links_prefetch), against local servers
cp "$T/fx/v086.json" "$T/fx/links.json"
PORT=$((20000 + RANDOM % 20000)); SPORT=$((PORT + 1))
python3 -I -c 'import http.server, functools, sys
h = functools.partial(http.server.SimpleHTTPRequestHandler, directory=sys.argv[1])
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[2])), h).serve_forever()' "$T/fx" $PORT 2> "$T/server.log" &
SRV=$!
python3 -I -c 'import socket, sys, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(8)
c = []
while True: c.append(s.accept()[0])' $SPORT &
SLOW=$!
sleep 1
L="UA_LINKS_ALLOW_LOCAL=1"
run net 0.8.5 $L UA_LINKS_URL=http://127.0.0.1:$PORT/links.json UA_UPDATE_AUTO=later
expect net "background read from a server: notice" 'is out \(this app 0\.8\.5\); test answer later'
grep -q '"GET /links.json HTTP' "$T/server.log" && ok "net: setup-time read has no ?start (game folder only, no pack)" || bad "net: plain read" "$T/server.log"
mkdir -p "$(SUP net2)/reforge-pack/hd-models"
run net2 0.8.5 $L UA_LINKS_URL=http://127.0.0.1:$PORT/links.json UA_UPDATE_AUTO=later
grep -q '"GET /links.json?start HTTP' "$T/server.log" && ok "net2: game + pack in place: the read says ?start" || bad "net2: ?start" "$T/server.log"
[ "$(grep -c 'GET /links.json' "$T/server.log")" = 2 ] && ok "one request per start (2 starts, 2 requests)" || bad "request count" "$T/server.log"
run offline 0.8.5 $L UA_LINKS_URL=http://127.0.0.1:9/links.json
expect offline "offline (connection refused): no dialog" 'not reachable; built-in links used' DIALOG
s=$(sed -n 's/^CHECK SECONDS //p' "$T/log-offline.txt"); awk -v s="$s" 'BEGIN { exit !(s < 1.0) }' && ok "offline: check took ${s}s (< 1 s)" || bad "offline: check took ${s}s" "$T/log-offline.txt"
run slow 0.8.5 $L UA_LINKS_URL=http://127.0.0.1:$SPORT/links.json
expect slow "server never answers: skipped, no dialog" 'slow to answer; skipped' DIALOG
s=$(sed -n 's/^CHECK SECONDS //p' "$T/log-slow.txt"); awk -v s="$s" 'BEGIN { exit !(s < 2.6) }' && ok "slow: gave up after ${s}s (at most 2 s of waiting)" || bad "slow: took ${s}s" "$T/log-slow.txt"
run dns 0.8.5 UA_LINKS_URL=https://links.invalid/links.json
expect dns "unknown host: no dialog" 'not reachable|slow to answer' DIALOG

# --- ver_cmp
vc() { env -i PATH=/usr/bin:/bin /bin/bash -c 'CONTENTS=/x SUPPORT=/x; . "$1"; ver_cmp "$2" "$3"' _ "$T/app/Contents/Resources/downloads.sh" "$1" "$2"; }
for c in "0.8.5 0.8.4.1 1" "0.8.4.1 0.8.4 1" "0.8.4 0.8.4.1 -1" "0.8.4 0.8.4.0 0" "0.8.10 0.8.9 1" "1.0 0.9.9.9 1" "0.8.05 0.8.5 0" "0.9 0.8.99 1" "0.8.4.2 0.8.4.10 -1"; do
    set -- $c; r="$(vc "$1" "$2")"; [ "$r" = "$3" ] && ok "ver_cmp $1 $2 = $3" || bad "ver_cmp $1 $2 = $r (want $3)"
done

# --- released validators (0.8.3, 0.8.4, ...) on the new links.json
LJ() { /usr/bin/osascript -l JavaScript "$1" "$2" "$(grep '^allow_host' "$ROOT/packaging/macos/downloads.txt" | cut -f2 | tr '\n' ' ')" 0; }
for o in ${OLD[@]+"${OLD[@]}"}; do
    a="$(LJ "$o" "$T/fx/none.json" 2>&1)"; ra=$?
    for f in v086 future m_array m_vnum; do
        b="$(LJ "$o" "$T/fx/$f.json" 2>&1)"; rb=$?
        if [ $ra = 0 ] && [ $rb = 0 ] && [ "$a" = "$b" ]; then ok "old validator $(basename "$(dirname "$o")")/$(basename "$o") accepts $f.json, same lines as without latest"
        else bad "old validator $o on $f.json: exit $rb"; echo "$b" | head -3; fi
    done
done

echo "update notice tests: $PASS passed, $FAIL failed"
[ $FAIL = 0 ]

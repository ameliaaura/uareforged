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
        ask() { echo "NOTICE [$1] buttons [${*:3}] default [$2]" >&2; printf "%s" "OK"; }
        dlg() { echo "DIALOG [$1] [$2] buttons [${*:8}] default [$8]" >&2; printf "%s\t0\t0" "${ASK_ANSWER:-Later}"; }
        # the new app "starts": it runs its own update_finish (RELAUNCH=none: it never starts)
        update_relaunch() { echo "RELAUNCH $1"; [ "${RELAUNCH:-finish}" = finish ] && ( sleep 0.3; CONTENTS="$1/Contents"; . "$1/Contents/Resources/downloads.sh"; update_finish ) & }
        links_prefetch
        t0=$(perl -MTime::HiRes=time -e "printf q(%.3f), time")
        update_check
        t1=$(perl -MTime::HiRes=time -e "printf q(%.3f), time")
        echo "CHECK SECONDS $(echo "$t1 - $t0" | bc)"
        echo "GAME STARTS"
    ' _ "${APPC:-$T/app/Contents}" "$T/sup-$name" "${GAMEDIR:-$T/game}" > "$T/log-$name.txt" 2>&1
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
grep -q "buttons \[Download Later Don't remind me for this version What's new\] default \[Download\]" "$T/log-newer.txt" && ok "newer (no one-click update in links.json): Download / Later / Don't remind / What's new" || bad "newer: buttons" "$T/log-newer.txt"
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

# --- one-click update (signed manifest + file), against a local server; keys made here (never the release key)
U="$T/upd"; mkdir -p "$U/keys" "$U/srv"
for k in rel test other; do /usr/bin/openssl ecparam -name prime256v1 -genkey -noout -out "$U/keys/$k.pem" 2>/dev/null
    /usr/bin/openssl ec -in "$U/keys/$k.pem" -pubout 2>/dev/null | grep -v -- ----- | tr -d '\n' > "$U/keys/$k.pub"; done
BID=io.github.ameliaaura.uareforged
# mkapp <folder> <version> [bundle id] [extra downloads.txt lines]: a small signed app like the real one (same launcher code)
mkapp() {
    local a="$1/UA Reforged.app/Contents"; mkdir -p "$a/MacOS" "$a/Resources"
    printf '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0"><dict><key>CFBundleIdentifier</key><string>%s</string><key>CFBundleExecutable</key><string>UA Reforged</string><key>CFBundlePackageType</key><string>APPL</string><key>CFBundleShortVersionString</key><string>%s</string></dict></plist>\n' "${3:-$BID}" "$2" > "$a/Info.plist"
    printf '#!/bin/bash\necho app %s\n' "$2" > "$a/MacOS/UA Reforged"; chmod 755 "$a/MacOS/UA Reforged"
    cp "$ROOT/packaging/macos/"{downloads.sh,links.js} "$a/Resources/"
    { grep -v '^update_key' "$ROOT/packaging/macos/downloads.txt"; printf 'app_version\t%s\nupdate_key\t%s\n' "$2" "$(cat "$U/keys/rel.pub")"; [ -n "${4:-}" ] && printf '%s\n' "$4"; } > "$a/Resources/downloads.txt"
    /usr/bin/codesign --force -s - "$1/UA Reforged.app" 2>/dev/null
}
# mkzip <name> <version> [bundle id]: the release zip of a new version (UA Reforged/UA Reforged.app inside)
mkzip() { rm -rf "$U/z"; mkapp "$U/z/UA Reforged" "$2" "${3:-}"; ( cd "$U/z" && /usr/bin/ditto -c -k --keepParent "UA Reforged" "$U/srv/$1" ); }
# sign <key> <manifest name> <version> <platform> <file for bytes + sha> [url name]: the links.json "update" object
sign() {
    local f="$U/srv/$5" m="$U/man.$$"
    printf 'UA Reforged update 1\nname\t%s\nversion\t%s\nplatform\t%s\nbytes\t%s\nsha256\t%s\n' "$2" "$3" "$4" "$(wc -c < "$f" | tr -d ' ')" "$(shasum -a 256 "$f" | cut -d' ' -f1)" > "$m"
    /usr/bin/openssl dgst -sha256 -sign "$U/keys/$1.pem" -out "$m.sig" "$m"
    printf '{"url": "http://127.0.0.1:%s/%s", "manifest": "%s", "sig": "%s"}' "$UPORT" "${6:-$2}" "$(/usr/bin/openssl base64 -A -in "$m")" "$(/usr/bin/openssl base64 -A -in "$m.sig")"
    rm -f "$m" "$m.sig"
}
ufx() { printf '{ %s, "latest": {"mac": {"version": "%s", "url": "https://uareforged.pages.dev/downloads/", "update": %s}} }\n' "$LINKS_BASE" "$2" "$3" > "$T/fx/$1.json"; }
UPORT=$((20000 + RANDOM % 20000)); [ "$UPORT" = "$PORT" ] && UPORT=$((UPORT + 7))
python3 -I -c 'import http.server, functools, sys, os
class H(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/trunc/"):   # the full length announced, half sent, connection dropped
            p = os.path.join(sys.argv[1], self.path[7:]); d = open(p, "rb").read()
            if self.headers.get("Range"):   # the link check gets its first KB as usual
                self.send_response(206); self.send_header("Content-Range", "bytes 0-1023/%d" % len(d)); self.send_header("Content-Length", "1024")
                self.send_header("Content-Type", "application/zip"); self.end_headers(); self.wfile.write(d[:1024]); return
            self.send_response(200); self.send_header("Content-Length", str(len(d))); self.send_header("Content-Type", "application/zip"); self.end_headers()
            self.wfile.write(d[:len(d) // 2]); self.wfile.flush(); self.connection.shutdown(2); return
        return super().do_GET()
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[2])), functools.partial(H, directory=sys.argv[1])).serve_forever()' "$U/srv" $UPORT 2> "$U/server.log" &
USRV=$!
trap 'kill $SRV $SLOW $USRV 2>/dev/null; chmod -R u+w "$T" 2>/dev/null; rm -rf "$T"' EXIT
Z6=UA-Reforged-0.8.6-macOS-arm64.zip
mkzip $Z6 0.8.6
cp "$U/srv/$Z6" "$U/srv/good-copy.zip"
mkzip UA-Reforged-0.8.6-other.zip 0.8.6 io.example.other
mkzip UA-Reforged-0.8.6-wrongver.zip 0.8.7
mkzip UA-Reforged-0.8.6-tampered.zip 0.8.6; printf 'x' >> "$U/srv/UA-Reforged-0.8.6-tampered.zip"
# a zip whose app's code no longer matches its signature
rm -rf "$U/z"; mkapp "$U/z/UA Reforged" 0.8.6; echo '# changed after signing' >> "$U/z/UA Reforged/UA Reforged.app/Contents/Resources/downloads.sh"
( cd "$U/z" && /usr/bin/ditto -c -k --keepParent "UA Reforged" "$U/srv/UA-Reforged-0.8.6-badsig.zip" )
sleep 1
L="UA_LINKS_ALLOW_LOCAL=1"
ufx u_good 0.8.6 "$(sign rel $Z6 0.8.6 mac $Z6)"
ufx u_badkey 0.8.6 "$(sign other $Z6 0.8.6 mac $Z6)"
ufx u_testkey 0.8.6 "$(sign test $Z6 0.8.6 mac $Z6)"
ufx u_win 0.8.6 "$(sign rel $Z6 0.8.6 windows $Z6)"
ufx u_replay 0.8.6 "$(sign rel $Z6 0.8.5 mac $Z6)"
ufx u_mismatch 0.8.7 "$(sign rel $Z6 0.8.6 mac $Z6)"
ufx u_trunc 0.8.6 "$(sign rel $Z6 0.8.6 mac $Z6 trunc/$Z6)"
# tampered: the file differs, the signed manifest is the original one (its sha256 no longer matches the file) ...
ufx u_tamper 0.8.6 "$(sign rel UA-Reforged-0.8.6-tampered.zip 0.8.6 mac $Z6)"
# ... and the other way: a manifest that matches the tampered file, signed with a key the app does not trust
ufx u_tamper2 0.8.6 "$(sign other UA-Reforged-0.8.6-tampered.zip 0.8.6 mac UA-Reforged-0.8.6-tampered.zip)"
ufx u_other 0.8.6 "$(sign rel UA-Reforged-0.8.6-other.zip 0.8.6 mac UA-Reforged-0.8.6-other.zip)"
ufx u_wrongver 0.8.6 "$(sign rel UA-Reforged-0.8.6-wrongver.zip 0.8.6 mac UA-Reforged-0.8.6-wrongver.zip)"
ufx u_badsig 0.8.6 "$(sign rel UA-Reforged-0.8.6-badsig.zip 0.8.6 mac UA-Reforged-0.8.6-badsig.zip)"
ufx u_urlname 0.8.6 "$(sign rel $Z6 0.8.6 mac $Z6 good-copy.zip)"
GOODSIG="$(sign rel $Z6 0.8.6 mac $Z6)"
ufx u_nosig 0.8.6 "$(printf '%s' "$GOODSIG" | sed 's/, "sig": "[^"]*"//')"
ufx u_junksig 0.8.6 "$(printf '%s' "$GOODSIG" | sed 's/"sig": "[^"]*"/"sig": "MEUCIQDjunkjunkjunkjunkjunkjunkjunkjunkjunkjunkjunkAAAA"/')"
ufx u_hostbad 0.8.6 "$(printf '%s' "$GOODSIG" | sed 's#http://127.0.0.1:[0-9]*/#https://evil.example/#')"
ufx u_notzip 0.8.6 "$(printf '%s' "$GOODSIG" | sed 's#\.zip"#.dmg"#')"
ufx u_manjunk 0.8.6 "$(printf '%s' "$GOODSIG" | sed 's/"manifest": "[^"]*"/"manifest": "not base64!"/')"
# urun <name> <links fixture> [env...]: Update now on a fresh copy of the 0.8.5 app in its own folder
urun() {
    local n=$1 fx=$2; shift 2
    rm -rf "$U/a-$n"; mkapp "$U/a-$n" 0.8.5 "" "${UEXTRA:-}"
    APPC="$U/a-$n/UA Reforged.app/Contents" run "$n" 0.8.5-unused "$L" "UA_LINKS_FILE=$T/fx/$fx.json" UA_UPDATE_AUTO=update "$@"
}
# (run's set_version writes the shared test app; the update runs use their own app, so restore it after)
appver() { awk -F'\t' '$1 == "app_version" { print $2 }' "$U/a-$1/UA Reforged.app/Contents/Resources/downloads.txt" 2>/dev/null; }
intact() { [ "$(appver "$1")" = 0.8.5 ] && [ -z "$(find "$U/a-$1" -maxdepth 1 -name '.*' -print -quit)" ] && [ ! -f "$(SUP "$1")/update-pending.txt" ] && [ ! -d "$(SUP "$1")/update-download" ]; }
# refused <name> <fixture> <reason pattern> <description>: no update, a notice, the download page, the game starts, app untouched
refused() {
    urun "$1" "$2"
    if grep -Eq "update: not installed - .*($3)" "$T/log-$1.txt" && grep -q 'notice: UA Reforged could not update itself' "$T/log-$1.txt" \
       && grep -q 'update page: https://uareforged.pages.dev/downloads/ (test run' "$T/log-$1.txt" && grep -q 'GAME STARTS' "$T/log-$1.txt" && intact "$1"; then
        ok "$1: $4"; else bad "$1: $4" "$T/log-$1.txt"; fi
}
# the dialog with a one-click update
rm -rf "$U/a-dlg"; mkapp "$U/a-dlg" 0.8.5
APPC="$U/a-dlg/UA Reforged.app/Contents" run dlg x "$L" "UA_LINKS_FILE=$T/fx/u_good.json"
grep -q "buttons \[Update now Later Don't remind me for this version What's new\] default \[Update now\]" "$T/log-dlg.txt" && grep -q 'Update now downloads it, checks it and starts the new version' "$T/log-dlg.txt" \
    && grep -q 'GAME STARTS' "$T/log-dlg.txt" && ok "dialog: Update now / Later / Don't remind / What's new (Later: the game starts)" || bad "dialog with update" "$T/log-dlg.txt"
APPC="$U/a-dlg/UA Reforged.app/Contents" run news x "$L" "UA_LINKS_FILE=$T/fx/u_good.json" UA_UPDATE_AUTO=whatsnew
grep -q 'update page: https://uareforged.pages.dev/downloads/ (test run' "$T/log-news.txt" && grep -q 'GAME STARTS' "$T/log-news.txt" && intact dlg && ok "What's new: the page (not opened in tests), nothing installed" || bad "whatsnew" "$T/log-news.txt"
# the good update
urun good u_good
lg="$T/log-good.txt"
if grep -q 'update: signature and manifest ok' "$lg" && grep -q 'update: installed 0.8.6' "$lg" && grep -q 'RELAUNCH ' "$lg" && grep -q 'update: now running 0.8.6; the old version was removed' "$lg" \
   && grep -q 'the new version has started; this launcher stops' "$lg" && ! grep -q 'GAME STARTS' "$lg"; then ok "good: downloaded, checked, swapped, new app started, this launcher stopped"; else bad "good: update flow" "$lg"; fi
[ "$(appver good)" = 0.8.6 ] && ok "good: the app in place is 0.8.6" || bad "good: app version $(appver good)"
[ -z "$(find "$U/a-good" -maxdepth 1 -name '.*' -print -quit)" ] && ok "good: backup and work folder gone" || { bad "good: leftovers"; ls -la "$U/a-good"; }
[ ! -f "$(SUP good)/update-pending.txt" ] && [ ! -d "$(SUP good)/update-download" ] && ok "good: pending note and download removed" || bad "good: leftovers in the support folder"
/usr/bin/codesign --verify --deep --strict "$U/a-good/UA Reforged.app" 2>/dev/null && ok "good: new app's signature verifies in place" || bad "good: codesign"
grep -q "GET /$Z6" "$U/server.log" && ok "good: fetched from the server" || bad "good: no request" "$U/server.log"
# the new app never starts: the old one comes back
UPDATE_START_WAIT=4 urun nostart u_good RELAUNCH=none
grep -q 'the new version did not start; old version restored' "$T/log-nostart.txt" && grep -q 'GAME STARTS' "$T/log-nostart.txt" && intact nostart \
    && ok "nostart: the new app did not start within the wait: old app restored, the game starts" || bad "nostart" "$T/log-nostart.txt"
# refused
refused badkey u_badkey "signature is not valid" "signed with an unknown key: refused"
refused testkey u_testkey "signature is not valid" "signed with the TEST key, release build (no update_key_test): refused"
UEXTRA="update_key_test	$(cat "$U/keys/test.pub")" urun testok u_testkey
[ "$(appver testok)" = 0.8.6 ] && ok "testok: a test-hook build (update_key_test) takes the test-signed update" || bad "testok" "$T/log-testok.txt"
refused junksig u_junksig "signature is not valid" "garbage signature: refused"
refused win u_win "for another system \(windows\)" "manifest for Windows: refused"
refused replay u_replay "not newer than this version" "replay of a 0.8.5 manifest (downgrade): refused"
refused mismatch u_mismatch "not the version the site names" "manifest 0.8.6, site says 0.8.7: refused"
refused tamper u_tamper "does not match its signature|not the expected" "tampered file, original signed manifest: refused"
refused tamper2 u_tamper2 "signature is not valid" "tampered file with a manifest that matches it, unknown key: refused"
refused urlname u_urlname "address does not match" "url names another file than the manifest: refused"
refused other u_other "is not UA Reforged 0.8.6" "a different app (bundle id): refused"
refused wrongver u_wrongver "is not UA Reforged 0.8.6" "the app inside is another version: refused"
refused badsig u_badsig "code signature does not check out" "app changed after code signing: refused"
refused trunc u_trunc "connection dropped|download failed|not the expected" "interrupted download (half the file, connection dropped): refused"
# no or malformed update in links.json: only the update is dropped; the notice offers Download
for m in u_nosig u_hostbad u_notzip u_manjunk; do
    rm -rf "$U/a-$m"; mkapp "$U/a-$m" 0.8.5
    APPC="$U/a-$m/UA Reforged.app/Contents" run "$m" x "$L" "UA_LINKS_FILE=$T/fx/$m.json"
    if grep -q '(ignored; no one-click update)' "$T/log-$m.txt" && grep -q 'buttons \[Download ' "$T/log-$m.txt" && grep -q 'GAME STARTS' "$T/log-$m.txt"; then ok "$m: update dropped, notice offers Download"; else bad "$m" "$T/log-$m.txt"; fi
done
# where the app may not update itself: translocated, read-only folder, an app copy owned by someone else
rm -rf "$U/a-transl"; mkdir -p "$U/a-transl/AppTranslocation/X1"; mkapp "$U/a-transl/AppTranslocation/X1" 0.8.5
APPC="$U/a-transl/AppTranslocation/X1/UA Reforged.app/Contents" run transl x "$L" "UA_LINKS_FILE=$T/fx/u_good.json" UA_UPDATE_AUTO=update
grep -q 'Move UA Reforged to your Applications folder to enable one-click updates' "$T/log-transl.txt" && grep -q 'update page: https://uareforged.pages.dev/downloads/ (test run' "$T/log-transl.txt" \
    && grep -q 'GAME STARTS' "$T/log-transl.txt" && ok "translocated: explained, download page, nothing downloaded" || bad "translocated" "$T/log-transl.txt"
rm -rf "$U/a-ro"; mkapp "$U/a-ro" 0.8.5; chmod 555 "$U/a-ro"
APPC="$U/a-ro/UA Reforged.app/Contents" run ro x "$L" "UA_LINKS_FILE=$T/fx/u_good.json" UA_UPDATE_AUTO=update
chmod 755 "$U/a-ro"
grep -q 'may not change the folder it is in' "$T/log-ro.txt" && grep -q 'GAME STARTS' "$T/log-ro.txt" && [ "$(awk -F'\t' '$1=="app_version"{print $2}' "$U/a-ro/UA Reforged.app/Contents/Resources/downloads.txt")" = 0.8.5 ] \
    && ok "read-only folder: explained, download page, app untouched" || bad "read-only" "$T/log-ro.txt"
# Update now without an update block (Download dialog answered with update): the page
APPC="$U/a-dlg/UA Reforged.app/Contents" run noupd x "$L" "UA_LINKS_FILE=$T/fx/v086.json" UA_UPDATE_AUTO=update
grep -q 'update page: .*(test run' "$T/log-noupd.txt" && grep -q 'GAME STARTS' "$T/log-noupd.txt" && ok "no update block + update answer: download page" || bad "noupd" "$T/log-noupd.txt"
# old validators must still accept a links.json with "update" inside latest
cp "$T/fx/u_good.json" "$T/fx/v086u.json"

# --- ver_cmp
vc() { env -i PATH=/usr/bin:/bin /bin/bash -c 'CONTENTS=/x SUPPORT=/x; . "$1"; ver_cmp "$2" "$3"' _ "$T/app/Contents/Resources/downloads.sh" "$1" "$2"; }
for c in "0.8.5 0.8.4.1 1" "0.8.4.1 0.8.4 1" "0.8.4 0.8.4.1 -1" "0.8.4 0.8.4.0 0" "0.8.10 0.8.9 1" "1.0 0.9.9.9 1" "0.8.05 0.8.5 0" "0.9 0.8.99 1" "0.8.4.2 0.8.4.10 -1"; do
    set -- $c; r="$(vc "$1" "$2")"; [ "$r" = "$3" ] && ok "ver_cmp $1 $2 = $3" || bad "ver_cmp $1 $2 = $r (want $3)"
done

# --- released validators (0.8.3, 0.8.4, ...) on the new links.json
LJ() { /usr/bin/osascript -l JavaScript "$1" "$2" "$(grep '^allow_host' "$ROOT/packaging/macos/downloads.txt" | cut -f2 | tr '\n' ' ')" 0; }
for o in ${OLD[@]+"${OLD[@]}"}; do
    a="$(LJ "$o" "$T/fx/none.json" 2>&1)"; ra=$?
    for f in v086 future m_array m_vnum v086u; do
        b="$(LJ "$o" "$T/fx/$f.json" 2>&1)"; rb=$?
        if [ $ra = 0 ] && [ $rb = 0 ] && [ "$a" = "$b" ]; then ok "old validator $(basename "$(dirname "$o")")/$(basename "$o") accepts $f.json, same lines as without latest"
        else bad "old validator $o on $f.json: exit $rb"; echo "$b" | head -3; fi
    done
done

echo "update notice tests: $PASS passed, $FAIL failed"
[ $FAIL = 0 ]

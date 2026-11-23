#!/bin/bash
# Tests for the Linux edition's new-version notice and one-click update (packaging/linux: links.py "latest",
# downloads.sh update_check / update_apply, ver_cmp). The Linux twin of update_notice_test.sh (macOS).
# No network: links.json comes from a file (UA_LINKS_FILE) or a local server on 127.0.0.1; no dialog is ever shown
# (ask and dlg are replaced by stubs that log), no browser is opened. Needs bash, python3, curl, openssl, coreutils.
#   tests/launcher/update_notice_test_linux.sh [--old <links.py of an earlier build> ...]
# --old: also runs that earlier validator on the new links.json (it must accept the file and give the same lines as
# for the file without "latest"). Exit 0 = all passed.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OLD=(); while [ $# -gt 0 ]; do case "$1" in --old) OLD+=("$2"); shift 2 ;; *) echo "unknown argument $1"; exit 2 ;; esac; done
TPATH="${UA_TEST_PATH:-/usr/local/bin:/usr/bin:/bin}"
T="$(mktemp -d "${TMPDIR:-/tmp}/ua-update-test.XXXXXX")"; trap 'kill $SRV $SLOW 2>/dev/null; rm -rf "$T"' EXIT
SRV=""; SLOW=""
PASS=0; FAIL=0
ok() { PASS=$((PASS + 1)); echo "ok   $1"; }
bad() { FAIL=$((FAIL + 1)); echo "FAIL $1"; [ -n "${2:-}" ] && sed 's/^/     | /' "$2"; }

S="$T/app/usr/share/ua-reforged"
mkdir -p "$S" "$T/fx"
cp "$ROOT/packaging/linux/"{downloads.sh,links.py} "$S/"
BASE="$(cat "$ROOT/packaging/macos/downloads.txt")"   # build-linux.sh ships the Mac app's downloads.txt
# set_version <v or empty>: this app's version, as build-linux.sh writes it
set_version() { { printf '%s\n' "$BASE"; [ -n "$1" ] && printf 'app_version\t%s\n' "$1"; } > "$S/downloads.txt"; }

# links.json as the site serves it, plus "latest"
LINKS_BASE='"game_url": "https://metropolisdawn.de/filebase/download/5/", "game_min_bytes": 100000000,
  "help_url": "https://uareforged.pages.dev/downloads/#nogame-h", "pack_url": "https://pub-9574664f67b04217aa2ca93f509638ad.r2.dev/pack/",
  "pack_parts": [{"name": "UA-Reforged-pack-0.8.0-1of2.zip", "bytes": 699598021, "sha256": "afb815b7de26a078097d9ef0fd2190f2434655232f4e903d3e822019b3eca98f"}],
  "quest_parts": [{"name": "UA-Reforged-Quest-pack-0.9.0.zip", "bytes": 712134306, "sha256": "e2b5447e831dfd75afa0ba1c0b193546e4fa9bce330860fb130faaa52f1dfb78"}],
  "message": "Test notice"'
REL=https://github.com/ameliaaura/uareforged/releases/tag
fx() { printf '{ %s%s }\n' "$LINKS_BASE" "${2:+, $2}" > "$T/fx/$1.json"; }
lat() { printf '"latest": {"linux": {"version": "%s", "url": "%s"}, "mac": {"version": "%s", "url": "%s/v%s"}}' "$1" "${2:-$REL/v$1}" "$1" "$REL" "$1"; }
fx none ""
fx v086 "$(lat 0.8.6)"; fx v085 "$(lat 0.8.5)"; fx v084 "$(lat 0.8.4)"; fx v0841 "$(lat 0.8.4.1)"; fx v0850 "$(lat 0.8.5.0)"
fx v087 "$(lat 0.8.7)"; fx v0810 "$(lat 0.8.10)"; fx site "$(lat 0.8.6 https://uareforged.pages.dev/downloads/#linux-h)"
fx maconly '"latest": {"mac": {"version": "0.9.0", "url": "https://uareforged.pages.dev/downloads/"}, "windows": {"version": "0.9.0", "url": "https://uareforged.pages.dev/downloads/"}}'
fx future '"latest": {"linux": {"version": "0.8.6", "url": "https://uareforged.pages.dev/downloads/", "notes": "x"}, "freebsd": {"version": "1.0", "url": "https://uareforged.pages.dev/"}, "ios": 5}'
# malformed "latest" (each must be ignored on its own, the rest of the file still used)
fx m_vprefix "$(lat v0.8.6 $REL/v0.8.6)"; fx m_vbeta "$(lat 0.8.6-beta $REL/v0.8.6)"; fx m_vone "$(lat 1 $REL/v1)"
fx m_vfive "$(lat 0.8.6.1.2 $REL/v0)"; fx m_vlong "$(lat 0.8.1234567 $REL/v0)"
fx m_vnum '"latest": {"linux": {"version": 0.86, "url": "https://uareforged.pages.dev/"}}'
fx m_vnl '"latest": {"linux": {"version": "0.8.6\n", "url": "https://uareforged.pages.dev/"}}'
fx m_vbool '"latest": {"linux": {"version": true, "url": "https://uareforged.pages.dev/"}}'
fx m_http "$(lat 0.8.6 http://uareforged.pages.dev/downloads/)"; fx m_host "$(lat 0.8.6 https://evil.example/uareforged)"
fx m_repo "$(lat 0.8.6 https://github.com/ameliaaura/uareforged-evil/releases)"; fx m_owner "$(lat 0.8.6 https://github.com/someone/uareforged)"
fx m_dots "$(lat 0.8.6 https://github.com/ameliaaura/uareforged/../../someone/x)"; fx m_query "$(lat 0.8.6 'https://uareforged.pages.dev/?x=1')"
fx m_port "$(lat 0.8.6 https://uareforged.pages.dev:8443/)"; fx m_sub "$(lat 0.8.6 https://x.uareforged.pages.dev/)"
fx m_userinfo "$(lat 0.8.6 https://uareforged.pages.dev@evil.example/)"; fx m_pct "$(lat 0.8.6 https://github.com/ameliaaura/uareforged/%2e%2e/x)"
fx m_quote '"latest": {"linux": {"version": "0.8.6", "url": "https://uareforged.pages.dev/\"x"}}'
fx m_urlnl '"latest": {"linux": {"version": "0.8.6", "url": "https://uareforged.pages.dev/\n"}}'
fx m_nourl '"latest": {"linux": {"version": "0.8.6"}}'
fx m_array '"latest": [{"linux": "0.8.6"}]'; fx m_str '"latest": "0.8.6"'; fx m_null '"latest": null'
fx m_linstr '"latest": {"linux": "0.8.6"}'; fx m_linarr '"latest": {"linux": ["0.8.6"]}'
printf '{ "latest": {"linux": {"version": "0.8.6", "url": "%s/v0.8.6"} }\n' "$REL" > "$T/fx/badjson.json"

now() { python3 -I -c 'import time; print("%.3f" % time.time())'; }
# run <name> <version> <env...>: one launcher start up to the point where the game would start
run() {
    local name=$1; shift; local ver=$1; shift
    set_version "$ver"
    mkdir -p "$T/sup-$name"
    env -i PATH="$TPATH" HOME="$T" "$@" /bin/bash -c '
        HERE="$1"; SHARE="$HERE/usr/share/ua-reforged"; DIALOG=/bin/false; SUPPORT="$2"; PACK="$SUPPORT/reforge-pack"; GAME="$3"
        is_game_dir() { [ -d "$1/DATA" ] && [ -d "$1/LEVELS" ]; }
        . "$SHARE/downloads.sh"
        ask() { echo "NOTICE [$1] buttons [${*:3}] default [$2]" >&2; printf "%s" "OK"; }
        dlg() { echo "DIALOG [$1] [$2] buttons [${*:6}] default [$5]" >&2; printf "%s\t0" "${ASK_ANSWER:-Later}"; }
        open_with_desktop() { echo "OPENED $1"; }
        # the new AppImage "starts": its launcher runs update_finish (RELAUNCH=none: it never starts). Its version is
        # the app_version line the test wrote into the fake AppImage.
        update_relaunch() {
            echo "RELAUNCH $1"
            [ "${RELAUNCH:-finish}" = finish ] || return 0
            ( sleep 0.3; d="$(mktemp -d)"; { grep -v "^app_version" "$DL_CONF"; grep -a "^app_version" "$1"; } > "$d/downloads.txt"
              DL_CONF="$d/downloads.txt" APPIMAGE="$1"; update_finish; rm -rf "$d" ) &
        }
        links_prefetch
        t0=$(python3 -I -c "import time; print(\"%.3f\" % time.time())")
        update_check
        t1=$(python3 -I -c "import time; print(\"%.3f\" % time.time())")
        echo "CHECK SECONDS $(awk -v a="$t0" -v b="$t1" "BEGIN { printf \"%.3f\", b - a }")"
        echo "GAME STARTS"
    ' _ "${APPH:-$T/app}" "$T/sup-$name" "${GAMEDIR:-$T/game}" > "$T/log-$name.txt" 2>&1
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
grep -q "buttons \[Download Later Don't remind me for this version What's new\] default \[0\]" "$T/log-newer.txt" && ok "newer (no one-click update in links.json): Download / Later / Don't remind / What's new" || bad "newer: buttons" "$T/log-newer.txt"
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
run site 0.8.5 "$(F site)" UA_UPDATE_AUTO=download; expect site "uareforged.pages.dev URL accepted; Download opens it" 'update page: https://uareforged.pages.dev/downloads/#linux-h \(test run'

run future 0.8.5 "$(F future)"; expect future "unknown platforms and extra keys inside latest: tolerated" 'DIALOG \[A new version of UA Reforged \(0\.8\.6\)'
run maconly 0.8.5 "$(F maconly)"; expect maconly "latest without a linux entry: no dialog" 'no version information' 'DIALOG|ignored'
run none 0.8.5 "$(F none)";   expect none "no latest field (today's site): no dialog" 'no version information' DIALOG
run nover "" "$(F v086)";     expect nover "build without app_version: no dialog" 'this build has no version' DIALOG

# --- the answers
run dl 0.8.5 "$(F v086)" UA_UPDATE_AUTO=download; expect dl "test answer download: page logged, not opened" "update page: $REL/v0\.8\.6 \(test run: not opened\)" 'DIALOG|OPENED'
run later 0.8.5 "$(F v086)" UA_UPDATE_AUTO=later; expect later "test answer later: nothing kept" 'test answer later' 'update page|no reminder'
run dlbtn 0.8.5 "$(F v086)" ASK_ANSWER=Download; expect dlbtn "Download button: the desktop opens the page" "OPENED $REL/v0\.8\.6"
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
for m in m_vprefix m_vbeta m_vone m_vfive m_vlong m_vnum m_vnl m_vbool m_http m_host m_repo m_owner m_dots m_query m_port m_sub m_userinfo m_pct m_quote m_urlnl m_nourl m_array m_str m_null m_linstr m_linarr; do
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

# --- one-click update (signed manifest + AppImage), against a local server; keys made here (never the release key)
U="$T/upd"; mkdir -p "$U/keys" "$U/srv"
for k in rel test other; do openssl ecparam -name prime256v1 -genkey -noout -out "$U/keys/$k.pem" 2>/dev/null
    openssl ec -in "$U/keys/$k.pem" -pubout 2>/dev/null | grep -v -- ----- | tr -d '\n' > "$U/keys/$k.pub"; done
IMG=UA-Reforged-0.8.5-Linux-x86_64.AppImage
# mkimg <file> <version> [kind]: a small stand-in AppImage (ELF magic + "AI" 2 at byte 8, as a type 2 AppImage) that
# carries its version as an app_version line; kind elf = a plain ELF program (no AppImage magic)
mkimg() {
    if [ "${3:-}" = elf ]; then printf '\177ELF\002\001\001\000\000\000\000' > "$1"; else printf '\177ELF\002\001\001\000AI\002' > "$1"; fi
    head -c 3000 /dev/zero >> "$1"; printf '\napp_version\t%s\n' "$2" >> "$1"; head -c 2000 /dev/zero >> "$1"; chmod 755 "$1"
}
# mkapp <folder> <version> [extra downloads.txt lines]: the launcher's files (as in usr/share/ua-reforged) + this AppImage
mkapp() {
    local s="$1/app/usr/share/ua-reforged"; mkdir -p "$s"
    cp "$ROOT/packaging/linux/"{downloads.sh,links.py} "$s/"
    { grep -v '^update_key' "$ROOT/packaging/macos/downloads.txt"; printf 'app_version\t%s\nupdate_key\t%s\n' "$2" "$(cat "$U/keys/rel.pub")"; [ -n "${3:-}" ] && printf '%s\n' "$3"; } > "$s/downloads.txt"
    mkdir -p "$1/apps"; mkimg "$1/apps/$IMG" "$2"
}
sha256f() { sha256sum "$1" | cut -d' ' -f1; }
# sign <key> <manifest name> <version> <platform> <file for bytes + sha> [url name]: the links.json "update" object
sign() {
    local f="$U/srv/$5" m="$U/man.$$"
    printf 'UA Reforged update 1\nname\t%s\nversion\t%s\nplatform\t%s\nbytes\t%s\nsha256\t%s\n' "$2" "$3" "$4" "$(wc -c < "$f" | tr -d ' ')" "$(sha256f "$f")" > "$m"
    openssl dgst -sha256 -sign "$U/keys/$1.pem" -out "$m.sig" "$m"
    printf '{"url": "http://127.0.0.1:%s/%s", "manifest": "%s", "sig": "%s"}' "$UPORT" "${6:-$2}" "$(openssl base64 -A -in "$m")" "$(openssl base64 -A -in "$m.sig")"
    rm -f "$m" "$m.sig"
}
ufx() { printf '{ %s, "latest": {"linux": {"version": "%s", "url": "https://uareforged.pages.dev/downloads/", "update": %s}} }\n' "$LINKS_BASE" "$2" "$3" > "$T/fx/$1.json"; }
UPORT=$((20000 + RANDOM % 20000)); [ "$UPORT" = "$PORT" ] && UPORT=$((UPORT + 7)); [ "$UPORT" = "$SPORT" ] && UPORT=$((UPORT + 7))
python3 -I -c 'import http.server, functools, sys, os
class H(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/trunc/"):   # the full length announced, half sent, connection dropped
            p = os.path.join(sys.argv[1], self.path[7:]); d = open(p, "rb").read()
            if self.headers.get("Range"):   # the link check gets its first KB as usual
                self.send_response(206); self.send_header("Content-Range", "bytes 0-1023/%d" % len(d)); self.send_header("Content-Length", "1024")
                self.send_header("Content-Type", "application/octet-stream"); self.end_headers(); self.wfile.write(d[:1024]); return
            self.send_response(200); self.send_header("Content-Length", str(len(d))); self.send_header("Content-Type", "application/octet-stream"); self.end_headers()
            self.wfile.write(d[:len(d) // 2]); self.wfile.flush(); self.connection.shutdown(2); return
        return super().do_GET()
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[2])), functools.partial(H, directory=sys.argv[1])).serve_forever()' "$U/srv" $UPORT 2> "$U/server.log" &
USRV=$!
trap 'kill $SRV $SLOW $USRV 2>/dev/null; chmod -R u+w "$T" 2>/dev/null; rm -rf "$T"' EXIT
I6=UA-Reforged-0.8.6-Linux-x86_64.AppImage
mkimg "$U/srv/$I6" 0.8.6
cp "$U/srv/$I6" "$U/srv/good-copy.AppImage"
mkimg "$U/srv/UA-Reforged-0.8.6-tampered.AppImage" 0.8.6; printf 'x' >> "$U/srv/UA-Reforged-0.8.6-tampered.AppImage"
mkimg "$U/srv/UA-Reforged-0.8.6-elf.AppImage" 0.8.6 elf
printf 'PK\003\004 not an AppImage\n' > "$U/srv/UA-Reforged-0.8.6-zip.AppImage"; head -c 4000 /dev/zero >> "$U/srv/UA-Reforged-0.8.6-zip.AppImage"
mkimg "$U/srv/UA-Reforged-0.8.6-Linux.zip" 0.8.6
mkimg "$U/srv/UA-Reforged-0.8.6-wrongver.AppImage" 0.8.7
sleep 1
ufx u_good 0.8.6 "$(sign rel $I6 0.8.6 linux $I6)"
ufx u_badkey 0.8.6 "$(sign other $I6 0.8.6 linux $I6)"
ufx u_testkey 0.8.6 "$(sign test $I6 0.8.6 linux $I6)"
ufx u_win 0.8.6 "$(sign rel $I6 0.8.6 windows $I6)"
ufx u_mac 0.8.6 "$(sign rel $I6 0.8.6 mac $I6)"
ufx u_replay 0.8.6 "$(sign rel $I6 0.8.5 linux $I6)"
ufx u_mismatch 0.8.7 "$(sign rel $I6 0.8.6 linux $I6)"
ufx u_trunc 0.8.6 "$(sign rel $I6 0.8.6 linux $I6 trunc/$I6)"
# tampered: the file differs, the signed manifest is the original one (its sha256 no longer matches the file) ...
ufx u_tamper 0.8.6 "$(sign rel UA-Reforged-0.8.6-tampered.AppImage 0.8.6 linux $I6)"
# ... and the other way: a manifest that matches the tampered file, signed with a key the app does not trust
ufx u_tamper2 0.8.6 "$(sign other UA-Reforged-0.8.6-tampered.AppImage 0.8.6 linux UA-Reforged-0.8.6-tampered.AppImage)"
ufx u_elf 0.8.6 "$(sign rel UA-Reforged-0.8.6-elf.AppImage 0.8.6 linux UA-Reforged-0.8.6-elf.AppImage)"
ufx u_zipfile 0.8.6 "$(sign rel UA-Reforged-0.8.6-zip.AppImage 0.8.6 linux UA-Reforged-0.8.6-zip.AppImage)"
ufx u_zipname 0.8.6 "$(sign rel UA-Reforged-0.8.6-Linux.zip 0.8.6 linux UA-Reforged-0.8.6-Linux.zip)"
ufx u_wrongver 0.8.6 "$(sign rel UA-Reforged-0.8.6-wrongver.AppImage 0.8.6 linux UA-Reforged-0.8.6-wrongver.AppImage)"
ufx u_urlname 0.8.6 "$(sign rel $I6 0.8.6 linux $I6 good-copy.AppImage)"
GOODSIG="$(sign rel $I6 0.8.6 linux $I6)"
ufx u_nosig 0.8.6 "$(printf '%s' "$GOODSIG" | sed 's/, "sig": "[^"]*"//')"
ufx u_junksig 0.8.6 "$(printf '%s' "$GOODSIG" | sed 's/"sig": "[^"]*"/"sig": "MEUCIQDjunkjunkjunkjunkjunkjunkjunkjunkjunkjunkjunkAAAA"/')"
ufx u_hostbad 0.8.6 "$(printf '%s' "$GOODSIG" | sed 's#http://127.0.0.1:[0-9]*/#https://evil.example/#')"
ufx u_notimg 0.8.6 "$(printf '%s' "$GOODSIG" | sed 's#\.AppImage"#.deb"#')"
ufx u_manjunk 0.8.6 "$(printf '%s' "$GOODSIG" | sed 's/"manifest": "[^"]*"/"manifest": "not base64!"/')"
# urun <name> <links fixture> [env...]: Update now on a fresh copy of the 0.8.5 AppImage in its own folder
urun() {
    local n=$1 fx=$2; shift 2
    rm -rf "$U/a-$n"; mkapp "$U/a-$n" 0.8.5 "${UEXTRA:-}"
    APPH="$U/a-$n/app" run "$n" 0.8.5-unused "$L" "UA_LINKS_FILE=$T/fx/$fx.json" UA_UPDATE_AUTO=update APPIMAGE="$U/a-$n/apps/$IMG" "$@"
}
imgver() { grep -a '^app_version' "$U/a-$1/apps/$IMG" 2>/dev/null | cut -f2; }
intact() { [ "$(imgver "$1")" = 0.8.5 ] && [ "$(ls -A "$U/a-$1/apps")" = "$IMG" ] && [ ! -f "$(SUP "$1")/update-pending.txt" ] && [ ! -d "$(SUP "$1")/update-download" ]; }
# refused <name> <fixture> <reason pattern> <description>: no update, a notice, the download page, the game starts, AppImage untouched
refused() {
    urun "$1" "$2"
    if grep -Eq "update: not installed - .*($3)" "$T/log-$1.txt" && grep -q 'notice: UA Reforged could not update itself' "$T/log-$1.txt" \
       && grep -q 'update page: https://uareforged.pages.dev/downloads/ (test run' "$T/log-$1.txt" && grep -q 'GAME STARTS' "$T/log-$1.txt" && intact "$1"; then
        ok "$1: $4"; else bad "$1: $4" "$T/log-$1.txt"; ls -la "$U/a-$1/apps" 2>/dev/null | sed 's/^/     | /'; fi
}
# the dialog with a one-click update
rm -rf "$U/a-dlg"; mkapp "$U/a-dlg" 0.8.5
APPH="$U/a-dlg/app" run dlg x "$L" "UA_LINKS_FILE=$T/fx/u_good.json" APPIMAGE="$U/a-dlg/apps/$IMG"
grep -q "buttons \[Update now Later Don't remind me for this version What's new\] default \[0\]" "$T/log-dlg.txt" && grep -q 'Update now downloads it, checks it and starts the new version' "$T/log-dlg.txt" \
    && grep -q 'GAME STARTS' "$T/log-dlg.txt" && ok "dialog: Update now / Later / Don't remind / What's new (Later: the game starts)" || bad "dialog with update" "$T/log-dlg.txt"
APPH="$U/a-dlg/app" run news x "$L" "UA_LINKS_FILE=$T/fx/u_good.json" UA_UPDATE_AUTO=whatsnew APPIMAGE="$U/a-dlg/apps/$IMG"
grep -q 'update page: https://uareforged.pages.dev/downloads/ (test run' "$T/log-news.txt" && grep -q 'GAME STARTS' "$T/log-news.txt" && intact dlg && ok "What's new: the page (not opened in tests), nothing installed" || bad "whatsnew" "$T/log-news.txt"
# the good update
urun good u_good
lg="$T/log-good.txt"
if grep -q 'update: signature and manifest ok' "$lg" && grep -q 'update: installed 0.8.6' "$lg" && grep -q 'RELAUNCH ' "$lg" && grep -q 'update: now running 0.8.6; the old version was removed' "$lg" \
   && grep -q 'the new version has started; this launcher stops' "$lg" && ! grep -q 'GAME STARTS' "$lg"; then ok "good: downloaded, checked, replaced, new AppImage started, this launcher stopped"; else bad "good: update flow" "$lg"; fi
[ "$(imgver good)" = 0.8.6 ] && ok "good: the AppImage in place is 0.8.6 (same path)" || bad "good: AppImage version $(imgver good)"
[ "$(sha256f "$U/a-good/apps/$IMG")" = "$(sha256f "$U/srv/$I6")" ] && [ -x "$U/a-good/apps/$IMG" ] && ok "good: byte-identical to the signed file and executable" || bad "good: file or mode differs"
[ "$(ls -A "$U/a-good/apps")" = "$IMG" ] && ok "good: backup and temporary file gone" || { bad "good: leftovers"; ls -la "$U/a-good/apps"; }
[ ! -f "$(SUP good)/update-pending.txt" ] && [ ! -d "$(SUP good)/update-download" ] && ok "good: pending note and download removed" || bad "good: leftovers in the support folder"
grep -q "GET /$I6" "$U/server.log" && ok "good: fetched from the server" || bad "good: no request" "$U/server.log"
# a symlink to the AppImage (e.g. ~/Applications/ua -> the file): the real file is replaced, the link stays
rm -rf "$U/a-link"; mkapp "$U/a-link" 0.8.5; ln -s "$U/a-link/apps/$IMG" "$U/a-link/ua-link"
APPH="$U/a-link/app" run link x "$L" "UA_LINKS_FILE=$T/fx/u_good.json" UA_UPDATE_AUTO=update APPIMAGE="$U/a-link/ua-link"
[ "$(imgver link)" = 0.8.6 ] && [ -L "$U/a-link/ua-link" ] && grep -q 'now running 0.8.6' "$T/log-link.txt" && ok "link: the AppImage behind a symlink is replaced, the link stays" || bad "link" "$T/log-link.txt"
# the new AppImage never starts: the old one comes back
UPDATE_START_WAIT=4 urun nostart u_good RELAUNCH=none
grep -q 'the new version did not start; old version restored' "$T/log-nostart.txt" && grep -q 'GAME STARTS' "$T/log-nostart.txt" && intact nostart \
    && ok "nostart: the new AppImage did not start within the wait: old one restored, the game starts" || bad "nostart" "$T/log-nostart.txt"
# the new AppImage starts but is another version than signed: it never confirms, so the old one comes back
UPDATE_START_WAIT=4 urun wrongver u_wrongver
grep -q 'the new version did not start; old version restored' "$T/log-wrongver.txt" && intact wrongver \
    && ok "wrongver: the AppImage inside is another version: never confirmed, old one restored" || bad "wrongver" "$T/log-wrongver.txt"
# refused
refused badkey u_badkey "signature is not valid" "signed with an unknown key: refused"
refused testkey u_testkey "signature is not valid" "signed with the TEST key, release build (no update_key_test): refused"
UEXTRA="update_key_test	$(cat "$U/keys/test.pub")" urun testok u_testkey
[ "$(imgver testok)" = 0.8.6 ] && ok "testok: a test-hook build (update_key_test) takes the test-signed update" || bad "testok" "$T/log-testok.txt"
refused junksig u_junksig "signature is not valid" "garbage signature: refused"
refused win u_win "for another system \(windows\)" "manifest for Windows: refused"
refused mac u_mac "for another system \(mac\)" "manifest for the Mac: refused"
refused replay u_replay "not newer than this version" "replay of a 0.8.5 manifest (downgrade): refused"
refused mismatch u_mismatch "not the version the site names" "manifest 0.8.6, site says 0.8.7: refused"
refused tamper u_tamper "does not match its signature|not the expected" "tampered file, original signed manifest: refused"
refused tamper2 u_tamper2 "signature is not valid" "tampered file with a manifest that matches it, unknown key: refused"
refused urlname u_urlname "address does not match" "url names another file than the manifest: refused"
refused zipname u_zipname "not an AppImage" "manifest names a .zip: refused"
refused zipfile u_zipfile "does not lead to a program" "signed file is not a program: refused at the link check"
refused elf u_elf "not an AppImage" "a plain ELF program, not an AppImage: refused"
refused trunc u_trunc "connection dropped|download failed|not the expected" "interrupted download (half the file, connection dropped): refused"
# no or malformed update in links.json: only the update is dropped; the notice offers Download
for m in u_nosig u_hostbad u_notimg u_manjunk; do
    APPH="$U/a-dlg/app" run "$m" x "$L" "UA_LINKS_FILE=$T/fx/$m.json" APPIMAGE="$U/a-dlg/apps/$IMG"
    if grep -q '(ignored; no one-click update)' "$T/log-$m.txt" && grep -q 'buttons \[Download ' "$T/log-$m.txt" && grep -q 'GAME STARTS' "$T/log-$m.txt"; then ok "$m: update dropped, notice offers Download"; else bad "$m" "$T/log-$m.txt"; fi
done
# where the app may not update itself: the tar.gz folder (no AppImage), a read-only folder, no openssl
rm -rf "$U/a-tgz"; mkapp "$U/a-tgz" 0.8.5
APPH="$U/a-tgz/app" run tgz x "$L" "UA_LINKS_FILE=$T/fx/u_good.json" UA_UPDATE_AUTO=update
grep -q 'unpacked from the tar.gz' "$T/log-tgz.txt" && grep -q 'update page: https://uareforged.pages.dev/downloads/ (test run' "$T/log-tgz.txt" \
    && grep -q 'GAME STARTS' "$T/log-tgz.txt" && ! grep -q 'signature and manifest ok' "$T/log-tgz.txt" && intact tgz && ok "tar.gz: explained, download page, nothing downloaded" || bad "tar.gz" "$T/log-tgz.txt"
rm -rf "$U/a-ro"; mkapp "$U/a-ro" 0.8.5; chmod 555 "$U/a-ro/apps"
APPH="$U/a-ro/app" run ro x "$L" "UA_LINKS_FILE=$T/fx/u_good.json" UA_UPDATE_AUTO=update APPIMAGE="$U/a-ro/apps/$IMG"
chmod 755 "$U/a-ro/apps"
grep -q 'may not change the AppImage file or the folder it is in' "$T/log-ro.txt" && grep -q 'GAME STARTS' "$T/log-ro.txt" && intact ro \
    && ok "read-only folder: explained, download page, AppImage untouched" || bad "read-only" "$T/log-ro.txt"
rm -rf "$U/a-rof"; mkapp "$U/a-rof" 0.8.5; chmod 555 "$U/a-rof/apps/$IMG"
APPH="$U/a-rof/app" run rof x "$L" "UA_LINKS_FILE=$T/fx/u_good.json" UA_UPDATE_AUTO=update APPIMAGE="$U/a-rof/apps/$IMG"
chmod 755 "$U/a-rof/apps/$IMG"
grep -q 'may not change the AppImage file' "$T/log-rof.txt" && grep -q 'GAME STARTS' "$T/log-rof.txt" && intact rof \
    && ok "read-only AppImage file: explained, download page, untouched" || bad "read-only file" "$T/log-rof.txt"
# no openssl: a PATH with every tool but openssl
mkdir -p "$T/nossl"; IFS=: read -ra pd <<< "$TPATH"
for d in "${pd[@]}"; do for f in "$d"/*; do b="${f##*/}"; [ "$b" = openssl ] || [ -e "$T/nossl/$b" ] || ln -s "$f" "$T/nossl/$b" 2>/dev/null; done; done
rm -rf "$U/a-nossl"; mkapp "$U/a-nossl" 0.8.5
TPATH="$T/nossl" APPH="$U/a-nossl/app" run nossl x "$L" "UA_LINKS_FILE=$T/fx/u_good.json" UA_UPDATE_AUTO=update APPIMAGE="$U/a-nossl/apps/$IMG"
grep -q 'openssl is not installed' "$T/log-nossl.txt" && grep -q 'update page: https://uareforged.pages.dev/downloads/ (test run' "$T/log-nossl.txt" && grep -q 'GAME STARTS' "$T/log-nossl.txt" && intact nossl \
    && ok "no openssl: explained, download page, AppImage untouched" || bad "no openssl" "$T/log-nossl.txt"
# Update now without an update block (Download dialog answered with update): the page
APPH="$U/a-dlg/app" run noupd x "$L" "UA_LINKS_FILE=$T/fx/v086.json" UA_UPDATE_AUTO=update APPIMAGE="$U/a-dlg/apps/$IMG"
grep -q 'update page: .*(test run' "$T/log-noupd.txt" && grep -q 'GAME STARTS' "$T/log-noupd.txt" && intact dlg && ok "no update block + update answer: download page" || bad "noupd" "$T/log-noupd.txt"
cp "$T/fx/u_good.json" "$T/fx/v086u.json"

# --- update_finish ignores a pending note that is not for this AppImage / version
rm -rf "$U/a-fin"; mkapp "$U/a-fin" 0.8.6; mkdir -p "$(SUP fin)"; : > "$U/a-fin/apps/.$IMG-0.8.5.update-backup"
printf '0.8.6\t%s\t%s\n' "/elsewhere/$IMG" "$U/a-fin/apps/.$IMG-0.8.5.update-backup" > "$(SUP fin)/update-pending.txt"
env -i PATH="$TPATH" HOME="$T" APPIMAGE="$U/a-fin/apps/$IMG" /bin/bash -c 'HERE="$1"; SHARE="$HERE/usr/share/ua-reforged"; SUPPORT="$2"; . "$SHARE/downloads.sh"; update_finish' _ "$U/a-fin/app" "$(SUP fin)" > "$T/log-fin.txt" 2>&1
[ -f "$(SUP fin)/update-pending.txt" ] && [ -f "$U/a-fin/apps/.$IMG-0.8.5.update-backup" ] && ok "update_finish: a note for another AppImage is left alone" || bad "update_finish: other AppImage" "$T/log-fin.txt"

# --- ver_cmp
vc() { env -i PATH="$TPATH" /bin/bash -c 'HERE=/x SHARE=/x SUPPORT=/x; . "$1"; ver_cmp "$2" "$3"' _ "$S/downloads.sh" "$1" "$2"; }
for c in "0.8.5 0.8.4.1 1" "0.8.4.1 0.8.4 1" "0.8.4 0.8.4.1 -1" "0.8.4 0.8.4.0 0" "0.8.10 0.8.9 1" "1.0 0.9.9.9 1" "0.8.05 0.8.5 0" "0.9 0.8.99 1" "0.8.4.2 0.8.4.10 -1"; do
    set -- $c; r="$(vc "$1" "$2")"; [ "$r" = "$3" ] && ok "ver_cmp $1 $2 = $3" || bad "ver_cmp $1 $2 = $r (want $3)"
done

# --- earlier validators (links.py of an earlier build) on the new links.json
LP() { python3 -I "$1" "$2" "$(grep '^allow_host' "$ROOT/packaging/macos/downloads.txt" | cut -f2 | tr '\n' ' ')" 0; }
for o in ${OLD[@]+"${OLD[@]}"}; do
    a="$(LP "$o" "$T/fx/none.json" 2>&1)"; ra=$?
    for f in v086 future m_array m_vnum v086u; do
        b="$(LP "$o" "$T/fx/$f.json" 2>&1)"; rb=$?
        if [ $ra = 0 ] && [ $rb = 0 ] && [ "$a" = "$b" ]; then ok "earlier validator $o accepts $f.json, same lines as without latest"
        else bad "earlier validator $o on $f.json: exit $rb"; echo "$b" | head -3; fi
    done
done

echo "update notice tests (Linux): $PASS passed, $FAIL failed"
[ $FAIL = 0 ]

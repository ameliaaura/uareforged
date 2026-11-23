# UA Reforged (Linux) - downloads the app can make for the player (sourced by the launcher, bash). A port of
# packaging/macos/downloads.sh; the flow, wording, checks and test switches are the same:
#   * "Get the game": the community OpenUA Standalone Package, fetched straight from the Metropolis Dawn
#     community's own server on the player's computer (UA Reforged never hosts, mirrors or ships those files);
#   * the Reforge pack (the new look), checked against SHA-256 sums shipped in the app.
# Linux differences: the dialogs are ua-dialog (SDL, works with mouse, keyboard and game controller on any desktop);
# the folder picker is zenity or kdialog when the desktop has one; zip files are unpacked with bsdtar, unzip or
# python3 (whichever exists); downloads use the system's curl, else the copy in the package. No "Install on my
# Quest" here (use the Mac or Windows app, or SideQuest).
#
# Where from: usr/share/ua-reforged/downloads.txt (built-in values). At start-up the app reads a small
# links.json from the UA Reforged site (links_url) so a broken link can be fixed without a new app. That file is
# only parsed (links.py: strict types, https only, an allowlist of hosts); it can never run anything. If it is
# missing or invalid the built-in values are used.
#
# Failsafe: every link is checked first (a 1 KB ranged GET: must answer 2xx, must not be a web page, must start
# like a zip, must have a sane size). During a download a stall of 30 s or a dropped connection is retried twice
# (resuming where the server allows it). Any break cleans up the partial files and shows a plain notice with
# "Open help page" (the site's help) or another way on; the app is never left half set up.
#
# First launch is one step ("Set up everything", setup_first): one dialog for the game + the Reforge pack (or "I already
# have the game", a checkbox to skip the pack), then one progress window for both downloads.
# Every later start shows a small launcher (play_menu): a Play button per campaign, and Get the Reforge
# pack while none is installed. Dialogs never show file paths (a player's path holds their account name); the log does.
#
# Testing only: UA_LINKS_URL, UA_GAME_URL, UA_PACK_URL override addresses (UA_LINKS_ALLOW_LOCAL=1 also accepts
# http://127.0.0.1 in links.json); UA_SETUP_AUTO=everything|game-only|folder|folder-only|quit answers the
# first-launch dialog (UA_GAME_AUTO=get = "everything", with the pack unless UA_PACK_AUTO=no; UA_TEST_FOLDER=<dir>
# answers the folder picker); UA_PACK_AUTO=yes|no answers the pack offer; UA_NOTICE_AUTO=help|folder|later answers
# the notices (and opens no browser); UA_PLAY_AUTO=play|pack|quit answers the launcher once (UA_CAMPAIGN skips
# it); UA_PACK_NOUI=1 shows no progress window; UA_STATS_AUTO=yes|no|dismiss answers the usage stats question.
# UA_LINKS_FILE=<file> reads that file instead of links.json (no request); UA_UPDATE_AUTO=update|download|later|skip|
# whatsnew answers the new-version notice. A build with test hooks (UA_TEST_HOOKS=ON) also accepts updates signed with
# the test key (update_key_test in downloads.txt); release builds never carry that line.

DL_CONF="$SHARE/downloads.txt"
PACK_DL="$SUPPORT/reforge-pack-download"
GAME_DL="$SUPPORT/game-download"
GAME_ORIG="$SUPPORT/game-original"
PACK_OFF="$SUPPORT/reforge-pack-offer-off"
DL_UA="UA-Reforged (Linux)"

SETUP_PROGRESS="$SUPPORT/setup-progress"
SETUP_ON=""        # set while "Set up everything" runs: one progress window for every step
PACK_ASKED=""      # the pack was offered (or chosen) this launch: no second offer
FONT="$SHARE/engine/fonts/LiberationSans-Regular.ttf"

# curl: the system's (it knows the system's certificates), else the self-contained copy in the package with the
# distribution's certificate bundle (Debian/Ubuntu/Arch/SteamOS, Fedora, openSUSE paths)
if command -v curl >/dev/null 2>&1; then CURL="$(command -v curl)"
else
    CURL="$HERE/usr/bin/curl"
    for c in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/ca-bundle.pem /etc/ssl/cert.pem; do
        [ -f "$c" ] && { export CURL_CA_BUNDLE="$c"; break; }
    done
fi
export UA_CURL="$CURL"   # the game sends its usage stats (only when the player said yes) with the same curl
fsize() { stat -c %s "$1" 2>/dev/null || echo 0; }
sha256() { sha256sum "$1" | cut -d' ' -f1; }
# unzip_to <zip> <dir>: whichever unpacker the system has
unzip_to() {
    if command -v bsdtar >/dev/null 2>&1; then bsdtar -xf "$1" -C "$2"
    elif command -v unzip >/dev/null 2>&1; then unzip -q -o "$1" -d "$2"
    elif command -v python3 >/dev/null 2>&1; then python3 -I -m zipfile -e "$1" "$2"
    else echo "no unzip tool (bsdtar, unzip or python3)"; return 1; fi
}

conf() { awk -F'\t' -v k="$1" '$1 == k { sub(/^[^\t]*\t/, ""); print }' "$DL_CONF"; }
human() { awk -v b="$1" 'BEGIN { if (b >= 1e9) printf "%.1f GB", b / 1e9; else if (b >= 1e6) printf "%d MB", b / 1e6; else printf "%d KB", (b + 999) / 1000 }'; }

# ask <text> <default button> <button>...: a dialog; prints the button (empty when closed). Text goes in as an
# argument, never into a script, so no text can change what runs.
ask() {
    local t="$1" d="$2" i=0 di=0 b; shift 2
    for b in "$@"; do [ "$b" = "$d" ] && di=$i; i=$((i + 1)); done
    "$DIALOG" ask --font "$FONT" --title "UA Reforged" --text "$t" --default "$di" -- "$@" 2>/dev/null </dev/null | cut -f1
}

# no_paths <text>: what a dialog may show. A player's paths hold their account name (it shows when they stream or share
# a screenshot), so our folders get friendly names and any other path under the home folder is left out.
no_paths() {
    local t="$1" a="UA Reforged's folder" b="the UA Reforged app" c="your home folder"
    t="${t//"$SUPPORT"/$a}"; t="${t//"$HERE"/$b}"; t="${t//"$HOME"/$c}"
    printf '%s' "$t" | sed -E 's#/(home|media|mnt|run|var|tmp)/[^ ,;:)"]*#a file on this computer#g'
}
# dlg <heading> <text> <check label> <check 0|1> <default index> <buttons...>: the larger dialog.
# Prints "<button>\t<check>".
dlg() {
    local h="$1" t="$2" c="$3" cv="$4" d="$5"; shift 5
    "$DIALOG" ask --font "$FONT" --title "UA Reforged" --heading "$h" --text "$t" --check "$c" --checked "$cv" \
        --default "$d" -- "$@" 2>/dev/null </dev/null
}
# open_with_desktop <file, folder or URL>: the desktop's default program (browser, file manager)
open_with_desktop() { if command -v xdg-open >/dev/null 2>&1; then xdg-open "$1" >/dev/null 2>&1 & else echo "no xdg-open: $1 not opened"; fi; }
# show_in_folder <file>: open the folder that holds it (the player sees it; no path in our text)
show_in_folder() { [ -n "${UA_NOTICE_AUTO:-}" ] && { echo "show folder: $1 (test run: not opened)"; return; }; open_with_desktop "$(dirname "$1")"; }

open_help() {
    if [ -n "${UA_NOTICE_AUTO:-}" ]; then echo "help page: $1 (test run: not opened)"; return; fi
    echo "help page: $1"; open_with_desktop "$1"
}

# ---------------------------------------------------------------- links: built-in values, then the site's links.json
# One read of links.json per start at most. The launcher starts it in the background (links_prefetch) as soon as it
# knows the game folder; the setup, the pack and the new-version notice all use that one read. A start with the game
# and the Reforge pack in place says so with "?start" on the address, which the site does not count as a setup
# (docs/usage-stats.md).
links_src() { printf '%s' "${UA_LINKS_URL:-$(conf links_url | head -1)}"; }
links_curl() { # links_curl <url> <file>: the one request (the same limits as always)
    local proto="=https"; [ -n "${UA_LINKS_ALLOW_LOCAL:-}" ] && proto="=https,http"
    ${LINKS_EXEC:+exec} "$CURL" -fsSL --proto "$proto" --proto-redir "$proto" --max-time 8 --connect-timeout 5 \
        --max-filesize 16384 -A "$DL_UA" -o "$2" "$1" </dev/null 2>/dev/null
}
links_prefetch() {
    [ -n "${LINKS_DONE:-}${LINKS_PRE_SRC:-}" ] && return 0
    [ -f "$DL_CONF" ] || return 0
    [ -n "${UA_LINKS_FILE:-}" ] && return 0   # test runs: a local file, read by links_load
    local src; src="$(links_src)"; [ -n "$src" ] || return 0
    if is_game_dir "$GAME" && [ -d "$PACK/hd-models" ]; then
        case "$src" in *\?*) src="$src&start" ;; *) src="$src?start" ;; esac
    fi
    LINKS_PRE_SRC="$src"
    mkdir -p "$SUPPORT"; rm -f "$SUPPORT/links.json.pre"
    ( LINKS_EXEC=1 links_curl "$src" "$SUPPORT/links.json.pre" ) &   # (exec: the job is curl itself)
    LINKS_PRE_PID=$!
}
links_pre_wait() { # the background read is over (or this waits for it): LINKS_PRE_RC = curl's exit code
    [ -n "${LINKS_PRE_PID:-}" ] || return 0
    wait "$LINKS_PRE_PID" 2>/dev/null; LINKS_PRE_RC=$?; LINKS_PRE_PID=""
}
links_load() {
    L_GAME_URL="$(conf game_url | head -1)"; L_GAME_MIN="$(conf game_min_bytes | head -1)"
    L_GAME_MAX="$(conf game_max_bytes | head -1)"
    L_PACK_URL="$(conf url | head -1)"; L_PACK_PARTS="$(conf part)"
    L_HELP="$(conf help_url | head -1)"; L_PACK_HELP="$(conf pack_help_url | head -1)"; L_MESSAGE=""
    L_LATEST=""; L_LATEST_URL=""; L_UPD_URL=""; L_UPD_MAN=""; L_UPD_SIG=""
    local src tmp="$SUPPORT/links.json.tmp" out ok=1
    local allow; allow="$(conf allow_host | tr '\n' ' ')"
    local loc=0; [ -n "${UA_LINKS_ALLOW_LOCAL:-}" ] && loc=1
    rm -f "$tmp"
    if [ -n "${UA_LINKS_FILE:-}" ]; then
        src="$UA_LINKS_FILE (test file)"; cp "$UA_LINKS_FILE" "$tmp" 2>/dev/null && ok=0
    elif [ -n "${LINKS_PRE_SRC:-}" ]; then
        src="$LINKS_PRE_SRC"; links_pre_wait
        [ "${LINKS_PRE_RC:-1}" = 0 ] && [ -f "$SUPPORT/links.json.pre" ] && mv -f "$SUPPORT/links.json.pre" "$tmp" && ok=0
        rm -f "$SUPPORT/links.json.pre"
    else
        src="$(links_src)"; [ -n "$src" ] && links_curl "$src" "$tmp" && ok=0
    fi
    if [ $ok = 0 ]; then
        if ! command -v python3 >/dev/null 2>&1; then
            echo "links: $src not checked (python3 is not installed); built-in links used"
        elif out="$(python3 -I "$SHARE/links.py" "$tmp" "$allow" "$loc" linux 2>&1)"; then
            local k v w parts=""
            while IFS="$(printf '\t')" read -r k v w; do
                [ -n "$w" ] && [ "$k" != latest ] && [ "$k" != latest_update ] && v="$v"$'\t'"$w"
                case "$k" in
                    game_url) L_GAME_URL="$v" ;; game_min_bytes) L_GAME_MIN="$v" ;; pack_url) L_PACK_URL="$v" ;;
                    help_url) L_HELP="$v" ;; pack_help_url) L_PACK_HELP="$v" ;; message) L_MESSAGE="$v" ;;
                    part) parts="$parts$v"$'\n' ;;
                    latest) L_LATEST="$v"; L_LATEST_URL="$w" ;;
                    latest_ignored) echo "links: $v (ignored; no new-version notice)" ;;
                    latest_update) L_UPD_URL="$v"; L_UPD_MAN="${w%%$'\t'*}"; L_UPD_SIG="${w#*$'\t'}" ;;
                    latest_update_ignored) echo "links: $v (ignored; no one-click update)" ;;
                esac
            done <<< "$out"
            [ -n "$parts" ] && L_PACK_PARTS="${parts%$'\n'}"
            echo "links: $src accepted"
        else
            echo "links: $src rejected (${out##*: }); built-in links used"
            stats_error links_json_invalid
        fi
    else
        echo "links: $src not reachable; built-in links used"
    fi
    rm -f "$tmp"
    [ -n "${UA_GAME_URL:-}" ] && L_GAME_URL="$UA_GAME_URL"
    [ -n "${UA_PACK_URL:-}" ] && L_PACK_URL="$UA_PACK_URL"
    case "$L_PACK_URL" in */) ;; *) L_PACK_URL="$L_PACK_URL/" ;; esac
    LINKS_DONE=1
}

# ---------------------------------------------------------------- the new-version notice (every start)
# ver_cmp <a> <b>: prints -1, 0 or 1. Up to four numeric parts, missing parts count as 0 (0.8.4 = 0.8.4.0 < 0.8.4.1 < 0.8.5).
ver_cmp() {
    awk -v a="$1" -v b="$2" 'BEGIN { n = split(a, x, "."); m = split(b, y, "."); r = 0
        for (i = 1; i <= 4; i++) { p = (i <= n) ? x[i] + 0 : 0; q = (i <= m) ? y[i] + 0 : 0
            if (p < q) { r = -1; break } if (p > q) { r = 1; break } }
        print r }'
}
# update_check: just before the game starts. When links.json names a newer Linux version than this app (app_version in
# downloads.txt, written by build-linux.sh), one dialog: Update now / Later / Don't remind me for this version (kept in
# update-skip.txt; a newer version asks again) / What's new (opens the release page, then the dialog comes back). Without
# a one-click update in links.json the first button is Download (opens the release page). Anything missing, late
# (links.json not read within 2 s of this point), invalid or not newer: no dialog, no delay, the game starts.
# Test runs: UA_UPDATE_AUTO=update|download|later|skip|whatsnew answers the dialog (and opens no browser).
UPDATE_SKIP="$SUPPORT/update-skip.txt"
update_check() {
    local mine; mine="$(conf app_version | head -1)"
    case "$mine" in ''|*[!0-9.]*) echo "update check: this build has no version"; return 0 ;; esac
    if [ -z "${LINKS_DONE:-}" ] && [ -n "${LINKS_PRE_PID:-}" ]; then
        # at most 2 s more for the background read: a watchdog stops it after that
        ( sleep 2; kill "$LINKS_PRE_PID" 2>/dev/null ) & local dog=$!
        links_pre_wait
        kill "$dog" 2>/dev/null; wait "$dog" 2>/dev/null
        if [ "${LINKS_PRE_RC:-0}" -gt 128 ]; then
            rm -f "$SUPPORT/links.json.pre"; LINKS_PRE_SRC=""
            echo "update check: links.json is slow to answer; skipped"; return 0
        fi
    fi
    [ -n "${LINKS_DONE:-}" ] || links_load
    [ -n "${L_LATEST:-}" ] || { echo "update check: no version information"; return 0; }
    if [ "$(ver_cmp "$L_LATEST" "$mine")" != 1 ]; then echo "update check: $mine is up to date (latest $L_LATEST)"; return 0; fi
    local skip; skip="$(head -1 "$UPDATE_SKIP" 2>/dev/null)"
    case "$skip" in ''|*[!0-9.]*) skip="" ;; esac
    if [ -n "$skip" ] && [ "$(ver_cmp "$L_LATEST" "$skip")" != 1 ]; then echo "update check: $L_LATEST is out; not reminding (skipped)"; return 0; fi
    local b="${UA_UPDATE_AUTO:-}" up="Update now" dl="Download" later="Later" never="Don't remind me for this version" news="What's new"
    local first="$up" how="Update now downloads it, checks it and starts the new version."
    [ -n "$L_UPD_URL" ] || { first="$dl"; how="Download opens the download page in your browser."; }
    if [ -n "$b" ]; then
        echo "update check: $L_LATEST is out (this app $mine); test answer $b"
        [ "$b" = whatsnew ] && { echo "update page: $L_LATEST_URL (test run: not opened)"; b=later; }
    else
        while :; do
            b="$(dlg "A new version of UA Reforged ($L_LATEST) is out." "You have version $mine. $how Later starts the game now." "" 0 0 "$first" "$later" "$never" "$news")"
            b="${b%%$'\t'*}"
            echo "update check: $L_LATEST is out (this app $mine); chose [${b:-closed}]"
            [ "$b" = "$news" ] || break
            echo "update page: $L_LATEST_URL"; open_with_desktop "$L_LATEST_URL"
        done
    fi
    case "$b" in
        "$up"|update)
            if [ -n "$L_UPD_URL" ]; then update_apply "$mine"; else update_page; fi ;;
        "$dl"|download) update_page ;;
        "$never"|skip) printf '%s\n' "$L_LATEST" > "$UPDATE_SKIP"; echo "update check: no reminder for $L_LATEST" ;;
    esac
    return 0
}
update_page() {
    if [ -n "${UA_UPDATE_AUTO:-}" ]; then echo "update page: $L_LATEST_URL (test run: not opened)"
    else echo "update page: $L_LATEST_URL"; open_with_desktop "$L_LATEST_URL"; fi
}
# update_fail <what went wrong> [extra sentence]: a short notice, then the download page; the current version starts
update_fail() {
    echo "update: not installed - $1"
    rm -rf "$UPDATE_DL"; dl_ui_close
    local msg="UA Reforged could not update itself: $(no_paths "$1").${2:+ $2}

The download page opens now, so you can get the new version there. Your current version starts as usual."
    echo "notice: $msg" | tr '\n' ' '; echo
    [ -n "${UA_UPDATE_AUTO:-}" ] || ask "$msg" "OK" "OK" >/dev/null
    update_page
    return 1
}

# ---------------------------------------------------------------- one-click update (signed, AppImage only)
# links.json "latest.linux.update" = the AppImage's address, a small manifest (name, version, platform, bytes, sha256)
# and an ECDSA P-256 / SHA-256 signature over that manifest, made at release time with the UA Reforged update key (the
# same key as the Mac and Windows apps). Only its public half is in the app (downloads.txt update_key; a build with test
# hooks also takes update_key_test). The app refuses an update whose signature does not check out with one of those
# keys, whose manifest names another platform or a version that is not newer than this app (no downgrade, no replay of
# an old update), or whose file differs from the manifest in size or SHA-256 (the site alone can never push an update).
# Checked with the system's openssl. Without openssl, from the tar.gz folder, or when the AppImage file or its folder
# may not be changed: a short notice and the download page instead.
UPDATE_DL="$SUPPORT/update-download"
UPDATE_PENDING="$SUPPORT/update-pending.txt"
# update_verify <work dir> <this version>: checks the signature and the manifest; sets U_NAME U_VER U_BYTES U_SHA or
# returns 1 with U_WHY
update_verify() {
    local d="$1" mine="$2" k ok=1 line key val n=0 seen=" "
    U_WHY=""; U_NAME=""; U_VER=""; U_PLAT=""; U_BYTES=""; U_SHA=""
    printf '%s' "$L_UPD_MAN" | openssl base64 -d -A > "$d/manifest" 2>/dev/null
    printf '%s' "$L_UPD_SIG" | openssl base64 -d -A > "$d/manifest.sig" 2>/dev/null
    [ -s "$d/manifest" ] && [ -s "$d/manifest.sig" ] || { U_WHY="the update's signature is missing"; return 1; }
    while IFS= read -r k; do
        [ -n "$k" ] || continue
        { echo '-----BEGIN PUBLIC KEY-----'; printf '%s\n' "$k" | fold -w 64; echo '-----END PUBLIC KEY-----'; } > "$d/key.pem"
        if openssl dgst -sha256 -verify "$d/key.pem" -signature "$d/manifest.sig" "$d/manifest" >/dev/null 2>&1; then ok=0; break; fi
    done < <(conf update_key; conf update_key_test)
    rm -f "$d/key.pem"
    [ $ok = 0 ] || { U_WHY="the update's signature is not valid"; return 1; }
    while IFS= read -r line || [ -n "$line" ]; do
        n=$((n + 1))
        if [ $n = 1 ]; then [ "$line" = "UA Reforged update 1" ] || { U_WHY="the update's manifest is not readable"; return 1; }; continue; fi
        key="${line%%$'\t'*}"; val="${line#*$'\t'}"
        case "$seen" in *" $key "*) U_WHY="the update's manifest is not readable"; return 1 ;; esac
        seen="$seen$key "
        case "$key" in name) U_NAME="$val" ;; version) U_VER="$val" ;; platform) U_PLAT="$val" ;; bytes) U_BYTES="$val" ;; sha256) U_SHA="$val" ;; esac
    done < "$d/manifest"
    if ! [[ "$U_NAME" =~ ^[A-Za-z0-9._-]{1,100}$ && "$U_VER" =~ ^[0-9]{1,6}(\.[0-9]{1,6}){1,3}$ && "$U_BYTES" =~ ^[0-9]{1,11}$ && "$U_SHA" =~ ^[0-9a-f]{64}$ ]]; then
        U_WHY="the update's manifest is not readable"; return 1
    fi
    [ "$U_PLAT" = linux ] || { U_WHY="the update is for another system ($U_PLAT)"; return 1; }
    case "$U_NAME" in *.AppImage) ;; *) U_WHY="the update is not an AppImage"; return 1 ;; esac
    [ "$(ver_cmp "$U_VER" "$mine")" = 1 ] || { U_WHY="the update ($U_VER) is not newer than this version ($mine)"; return 1; }
    [ "$U_VER" = "$L_LATEST" ] || { U_WHY="the update ($U_VER) is not the version the site names ($L_LATEST)"; return 1; }
    [ "${L_UPD_URL##*/}" = "$U_NAME" ] || { U_WHY="the update's address does not match its manifest"; return 1; }
    return 0
}

# update_apply <this version>: download, check, put the new AppImage in place of this one (atomic rename; the old file
# stays as a hidden backup next to it until the new one has started), start the new one. Any problem: a short notice,
# the download page, and the current version starts (returns 1). On success this launcher exits.
update_apply() {
    local mine="$1" img dir f rc got tmp bak
    echo "update: $mine -> $L_LATEST from $L_UPD_URL (AppImage ${APPIMAGE:-none})"
    if [ -z "${APPIMAGE:-}" ]; then
        update_fail "this copy was unpacked from the tar.gz" "The AppImage edition can update itself; the tar.gz folder is updated by hand."; return 1
    fi
    img="$(readlink -f "$APPIMAGE" 2>/dev/null)"
    if [ -z "$img" ] || [ ! -f "$img" ]; then update_fail "the AppImage is not where UA Reforged expects it"; return 1; fi
    dir="$(dirname "$img")"
    if [ ! -w "$dir" ] || [ ! -w "$img" ] || [ ! -O "$img" ]; then
        update_fail "it may not change the AppImage file or the folder it is in" "Keep the AppImage in a folder of yours (for example your home folder) to enable one-click updates."; return 1
    fi
    command -v openssl >/dev/null 2>&1 || { update_fail "openssl is not installed, so the update's signature cannot be checked"; return 1; }
    rm -rf "$UPDATE_DL"; DL_DIR="$UPDATE_DL"; mkdir -p "$DL_DIR" || { update_fail "its folder could not be created"; return 1; }
    update_verify "$DL_DIR" "$mine" || { update_fail "$U_WHY"; return 1; }
    echo "update: signature and manifest ok ($U_NAME, $U_VER, $U_BYTES bytes)"
    local free_s free_d; free_s=$(df -Pk "$SUPPORT" | awk 'NR == 2 { print $4 }'); free_d=$(df -Pk "$dir" | awk 'NR == 2 { print $4 }')
    if [ "$((${free_s:-0} * 1024))" -lt "$((U_BYTES * 2))" ] || [ "$((${free_d:-0} * 1024))" -lt "$((U_BYTES * 2))" ]; then
        update_fail "there is not enough free disk space"; return 1
    fi
    dl_ui_start "Updating UA Reforged to $U_VER"
    DL_MAGIC=7f454c46 dl_precheck "$L_UPD_URL" 0 0 "$U_BYTES" || { update_fail "$PC_WHY"; return 1; }
    f="$DL_DIR/$U_NAME"; DL_WHAT="Downloading the update"; DL_BASE=0; DL_TOTAL="$U_BYTES"; DL_MAXSIZE="$U_BYTES"
    dl_fetch "$L_UPD_URL" "$f"; rc=$?; DL_MAXSIZE=""
    if [ $rc -eq 130 ]; then rm -rf "$UPDATE_DL"; dl_ui_close; echo "update: cancelled; the current version starts"; return 1; fi
    [ $rc -eq 0 ] || { update_fail "$DL_WHY"; return 1; }
    dl_status 91 "Checking the update..."
    got=$(fsize "$f")
    [ "$got" = "$U_BYTES" ] || { update_fail "the downloaded file is not the expected size"; return 1; }
    got="$(sha256 "$f")"
    [ "$got" = "$U_SHA" ] || { update_fail "the downloaded file does not match its signature"; return 1; }
    # an AppImage (type 2): an ELF program with "AI" 2 at byte 8
    local hdr; hdr="$(head -c 11 "$f" | od -An -tx1 | tr -d ' \n')"
    [ "${hdr:0:8}" = 7f454c46 ] && [ "${hdr:16:6}" = 414902 ] || { update_fail "the update is not an AppImage"; return 1; }
    # swap: the new file goes next to this one under a hidden name, then replaces it in one rename; the old file stays
    # as a hidden backup (removed by the new version once it has started)
    dl_status 97 "Putting the new version in place..."
    tmp="$dir/.$(basename "$img").update.$$"; bak="$dir/.$(basename "$img")-$mine.update-backup"
    rm -f "$tmp" "$bak"
    if ! cp "$f" "$tmp" 2>>"$DL_DIR/curl.err" || ! chmod 755 "$tmp"; then rm -f "$tmp"; update_fail "the new version could not be put next to this one"; return 1; fi
    [ "$(sha256 "$tmp")" = "$U_SHA" ] || { rm -f "$tmp"; update_fail "the new version could not be put next to this one"; return 1; }
    if ! ln "$img" "$bak" 2>/dev/null && ! cp -p "$img" "$bak" 2>>"$DL_DIR/curl.err"; then
        rm -f "$tmp" "$bak"; update_fail "the old version could not be kept aside"; return 1
    fi
    if ! mv -f "$tmp" "$img"; then rm -f "$tmp" "$bak"; update_fail "the new version could not be put in place"; return 1; fi
    rm -rf "$UPDATE_DL"
    printf '%s\t%s\t%s\n' "$U_VER" "$img" "$bak" > "$UPDATE_PENDING"
    echo "update: installed $U_VER, old version kept at $bak until the new one starts"
    dl_ui_close
    update_relaunch "$img"
    # the new version removes the pending file (and the backup) as soon as it starts; wait up to 60 s for that
    local i=0 n="${UPDATE_START_WAIT:-120}"
    while [ -f "$UPDATE_PENDING" ] && [ $i -lt "$n" ]; do sleep 0.5; i=$((i + 1)); done
    if [ ! -f "$UPDATE_PENDING" ]; then echo "update: the new version has started; this launcher stops"; exit 0; fi
    # the new version did not start: put the old one back
    rm -f "$UPDATE_PENDING"
    if mv -f "$bak" "$img"; then echo "update: the new version did not start; old version restored"
    else echo "update: the new version did not start and the old one could not be restored (backup at $bak)"; fi
    update_fail "the new version did not start, so the current one was put back"
    return 1
}
# update_relaunch <AppImage>: start the new AppImage on its own (it gets this environment, so test runs keep their UA_*
# settings; the AppImage runtime sets its own APPDIR / APPIMAGE)
update_relaunch() {
    echo "update: starting $1"
    ( cd "$HOME" 2>/dev/null; exec env -u APPDIR -u APPIMAGE -u ARGV0 -u OWD $(command -v setsid) "$1" </dev/null >/dev/null 2>&1 ) &
    disown $! 2>/dev/null
}
# update_finish: at every start. When an update has just put this AppImage in place, tell the old launcher (it waits for
# the pending file to go) and remove the old version's backup.
update_finish() {
    [ -f "$UPDATE_PENDING" ] || return 0
    local v app bak me mine
    IFS=$'\t' read -r v app bak < "$UPDATE_PENDING"
    me="$(readlink -f "${APPIMAGE:-/nonexistent}" 2>/dev/null)"; mine="$(conf app_version | head -1)"
    if [ "$v" = "$mine" ] && [ -n "$me" ] && [ "$app" = "$me" ]; then
        rm -f "$UPDATE_PENDING"
        case "$bak" in "$(dirname "$me")"/.*.update-backup) rm -f "$bak" ;; esac
        echo "update: now running $mine; the old version was removed"
    fi
}

# ---------------------------------------------------------------- progress window + cancel
# The window reads $UI_DIR/status. During "Set up everything" (SETUP_ON) one window stays open for every step: each
# step's 0-100 % is mapped into its share of the whole (SETUP_BASE + SETUP_SPAN) and SETUP_STEP is the stage line.
dl_status() {
    local p="$1" t="$2" line
    if [ -n "$SETUP_ON" ]; then
        [ "$t" = done ] && return 0
        p=$(( SETUP_BASE + p * SETUP_SPAN / 100 )); line="$p"$'\t'"$t"$'\t'"$SETUP_STEP"
    else line="$p"$'\t'"$t"; fi
    mkdir -p "$UI_DIR" && printf '%s\n' "$line" > "$UI_DIR/status.tmp" && mv -f "$UI_DIR/status.tmp" "$UI_DIR/status"
}
dl_ui_start() { # dl_ui_start <title>
    if [ -n "$SETUP_ON" ] && [ -n "${UI_PID:-}" ]; then dl_status 0 "Checking the download link..."; return 0; fi
    UI_PID=""; [ -n "$SETUP_ON" ] || UI_DIR="$DL_DIR"; rm -f "$UI_DIR/cancel"; dl_status 0 "Checking the download link..."
    if [ -z "${UA_PACK_NOUI:-}" ]; then
        "$DIALOG" progress --font "$FONT" --title "$1" --dir "$UI_DIR" >/dev/null 2>&1 </dev/null &
        UI_PID=$!; disown "$UI_PID" 2>/dev/null
    fi
}
# dl_ui_end: a step is over (the setup window stays open for the next step); dl_ui_close: the window goes now
dl_ui_end() { [ -n "$SETUP_ON" ] && return 0; dl_ui_close; }
dl_ui_close() { [ -n "${UI_PID:-}" ] && kill "$UI_PID" 2>/dev/null; UI_PID=""; [ -n "${UI_DIR:-}" ] && rm -f "$UI_DIR/status" "$UI_DIR/cancel"; return 0; }
dl_wait() { # wait for a background job; 130 = the player pressed Cancel
    local pid=$1
    while kill -0 "$pid" 2>/dev/null; do
        if [ -f "$UI_DIR/cancel" ]; then kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; return 130; fi
        [ -n "${DL_PROGRESS:-}" ] && "$DL_PROGRESS"
        sleep 0.5
    done
    wait "$pid"
}
dl_progress() {
    local have; have=$(fsize "$DL_FILE"); local now=$((DL_BASE + have))
    if [ "${DL_TOTAL:-0}" -gt 0 ]; then dl_status "$((now * 90 / DL_TOTAL))" "$DL_WHAT: $(human $now) of $(human $DL_TOTAL)"
    else dl_status 5 "$DL_WHAT: $(human $now)"; fi
}

# ---------------------------------------------------------------- link check: dl_precheck <url> <min> <max> <exact>
# sets PC_SIZE (0 = unknown), PC_RANGES (1/0); on failure returns 1 with PC_WHY in plain words
dl_precheck() {
    local url="$1" min="${2:-0}" max="${3:-0}" exact="${4:-0}" h="$DL_DIR/check.h" b="$DL_DIR/check.b" w rc code type
    PC_SIZE=0; PC_RANGES=0; PC_WHY=""; ST_DETAIL=""
    w="$("$CURL" -sS -L --max-time 20 --connect-timeout 10 --max-redirs 8 -r 0-1023 -A "$DL_UA" \
         -D "$h" -o "$b" -w '%{http_code}\t%{content_type}' "$url" </dev/null 2>"$DL_DIR/check.err")"; rc=$?
    code="${w%%$'\t'*}"; type="$(printf '%s' "${w#*$'\t'}" | tr 'A-Z' 'a-z')"
    if [ $rc -ne 0 ]; then
        ST_DETAIL=$rc
        case $rc in 6) PC_WHY="the server name could not be found" ;; 7) PC_WHY="the server could not be reached" ;;
            28) PC_WHY="the server did not answer in time" ;; 35|51|60) PC_WHY="the secure connection failed" ;;
            *) PC_WHY="the connection failed (curl $rc)" ;; esac
        echo "link check: $url -> $PC_WHY"; return 1
    fi
    case "$code" in 200|206) ;; *) ST_DETAIL="$code"; PC_WHY="the server answered HTTP $code instead of the file"; echo "link check: $url -> $PC_WHY"; return 1 ;; esac
    case "$type" in text/html*|application/xhtml*) PC_WHY="the link now leads to a web page, not to the file"; echo "link check: $url -> $PC_WHY ($type)"; return 1 ;; esac
    if [ "$(head -c 4 "$b" | od -An -tx1 | tr -d ' \n')" != "${DL_MAGIC:-504b0304}" ]; then
        PC_WHY="the link does not lead to a ${DL_MAGIC:+program}${DL_MAGIC:-zip} file"; echo "link check: $url -> $PC_WHY"; return 1
    fi
    # the last response's headers (redirects come first)
    local cr cl; cr="$(awk '/^HTTP\//{r=""} tolower($1)=="content-range:"{r=$2" "$3} END{print r}' "$h" | tr -d '\r')"
    cl="$(awk '/^HTTP\//{l=""} tolower($1)=="content-length:"{l=$2} END{print l}' "$h" | tr -d '\r')"
    if [ "$code" = 206 ]; then PC_RANGES=1; PC_SIZE="${cr##*/}"; else PC_SIZE="${cl:-0}"; fi
    case "$PC_SIZE" in ''|*[!0-9]*) PC_SIZE=0 ;; esac
    if [ "$PC_SIZE" -gt 0 ]; then
        if [ "$min" -gt 0 ] && [ "$PC_SIZE" -lt "$min" ]; then PC_WHY="the file on the server is far too small ($(human $PC_SIZE))"; fi
        if [ "$max" -gt 0 ] && [ "$PC_SIZE" -gt "$max" ]; then PC_WHY="the file on the server is far too big ($(human $PC_SIZE))"; fi
        if [ "$exact" -gt 0 ] && [ "$PC_SIZE" -ne "$exact" ]; then PC_WHY="the file on the server is not the expected one (size $PC_SIZE, expected $exact)"; fi
        [ -n "$PC_WHY" ] && { echo "link check: $url -> $PC_WHY"; return 1; }
    fi
    echo "link check: $url ok (HTTP $code, ${type:-no type}, size ${PC_SIZE:-?}, ranges $PC_RANGES)"
    return 0
}

# ---------------------------------------------------------------- download: dl_fetch <url> <file>
# 2 automatic retries after a stall (no bytes for 30 s) or a dropped connection; resumes when the server allows it.
# returns 0, 130 (cancelled) or 1 with DL_WHY
dl_fetch() {
    local url="$1" f="$2" try=0 rc resume
    DL_WHY=""; DL_FILE="$f"; DL_PROGRESS=dl_progress
    while :; do
        resume=(); [ "$PC_RANGES" = 1 ] && [ -s "$f" ] && resume=(-C -)
        [ "$PC_RANGES" = 1 ] || rm -f "$f"
        "$CURL" -fL -sS --connect-timeout 15 --speed-limit 1 --speed-time 30 --max-redirs 8 \
            ${DL_MAXSIZE:+--max-filesize "$DL_MAXSIZE"} -A "$DL_UA" "${resume[@]}" -o "$f" "$url" </dev/null 2>>"$DL_DIR/curl.err" &
        dl_wait $!; rc=$?
        [ $rc -eq 0 ] && break
        [ $rc -eq 130 ] && { DL_PROGRESS=""; return 130; }
        [ $rc -eq 33 ] && { PC_RANGES=0; rm -f "$f"; }
        ST_DETAIL=$rc
        case $rc in
            28) DL_WHY="the download stalled (no data for 30 seconds)" ;;
            18|52|55|56|92) DL_WHY="the connection dropped during the download" ;;
            22) DL_WHY="the server stopped serving the file ($(tail -1 "$DL_DIR/curl.err" 2>/dev/null | sed 's/^curl: ([0-9]*) //'))" ;;
            63) DL_WHY="the file on the server is far too big" ;;
            *) DL_WHY="the download failed (curl $rc)" ;;
        esac
        try=$((try + 1))
        echo "download: $url -> $DL_WHY (attempt $try)"
        [ $try -gt 2 ] && { DL_PROGRESS=""; return 1; }
        [ $rc -eq 22 ] && [ $try -ge 1 ] && [ ! -s "$f" ] && { DL_PROGRESS=""; return 1; }
        sleep 2
    done
    DL_PROGRESS=""; return 0
}

# ======================================================================== Get the game
# game_valid <dir>: the folder holds the original game's DATA and LEVELS with the key files (any letter case)
game_valid() {
    local d="$1" data levels
    data="$(find "$d" -maxdepth 1 -type d -iname DATA | head -1)"; levels="$(find "$d" -maxdepth 1 -type d -iname LEVELS | head -1)"
    [ -n "$data" ] && [ -n "$levels" ] || return 1
    [ -n "$(find "$data" -maxdepth 1 -type f -iname world.ini | head -1)" ] || return 1
    for s in SET1 SCRIPTS SOUNDS; do [ -n "$(find "$data" -maxdepth 1 -type d -iname $s | head -1)" ] || return 1; done
    local single; single="$(find "$levels" -maxdepth 1 -type d -iname SINGLE | head -1)"
    [ -n "$single" ] && [ "$(find "$single" -type f | head -20 | wc -l)" -ge 10 ]
}

# game_broken <why>: clean up, explain, offer the help page. returns 2 (help page opened: the launcher stops)
# or 3 (choose a folder instead)
game_broken() {
    rm -rf "$GAME_DL"; dl_ui_close
    echo "get the game: broken - $1"
    stats_error game_download_failed "${ST_DETAIL:-}"
    local b="${UA_NOTICE_AUTO:-}" msg="The game download link isn't working right now (it's hosted by the Metropolis Dawn community). We'll open the UA Reforged help page with other ways to get it.

What went wrong: $(no_paths "$1")."
    echo "notice: $msg" | tr '\n' ' '; echo
    [ -n "$b" ] || b="$(ask "$msg" "Open help page" "Choose a folder instead" "Open help page")"
    case "$b" in folder|"Choose a folder instead") return 3 ;; esac
    open_help "${L_HELP:-https://uareforged.pages.dev/downloads/#nogame-h}"; return 2
}

# game_get: prints nothing; on success sets GAME. returns 0 ok, 1 cancelled (back to the choices),
# 2 help page opened (stop), 3 choose a folder instead
game_get() {
    [ -n "${LINKS_DONE:-}" ] || links_load
    local url="$L_GAME_URL" min="${L_GAME_MIN:-100000000}" max="${L_GAME_MAX:-5000000000}"
    # "Set up everything" is the consent; on its own game_get asks here
    if [ -z "${UA_GAME_AUTO:-}" ] && [ -z "$SETUP_ON" ] && [ -z "${UA_SETUP_AUTO:-}" ]; then
        local note="UA Reforged will download the OpenUA Standalone Package (the full original game plus the Metropolis Dawn add-on, about $(game_size)) straight from the Metropolis Dawn community's server. It is hosted by the Metropolis Dawn community, not by UA Reforged, and the game files belong to Microsoft.

It is saved in UA Reforged's own folder."
        [ -n "$L_MESSAGE" ] && note="$note

$L_MESSAGE"
        [ "$(ask "$note" "Download" "Cancel" "Download")" = "Download" ] || return 1
    fi
    DL_DIR="$GAME_DL"; mkdir -p "$DL_DIR" || return 1
    local free_k; free_k=$(df -Pk "$SUPPORT" | awk 'NR == 2 { print $4 }')
    [ "$((free_k * 1024))" -lt 1500000000 ] && { game_broken "there is not enough free disk space (it needs about 1.5 GB)"; return $?; }
    dl_ui_start "Urban Assault (from the Metropolis Dawn community)"
    dl_precheck "$url" "$min" "$max" 0 || { game_broken "$PC_WHY"; return $?; }
    local f="$DL_DIR/openua-package.zip"
    DL_WHAT="Downloading the game"; DL_BASE=0; DL_TOTAL="$PC_SIZE"; DL_MAXSIZE="$max"
    dl_fetch "$url" "$f"; local rc=$?; DL_MAXSIZE=""
    if [ $rc -eq 130 ]; then dl_ui_close; echo "get the game: cancelled (the part stays for a resume)"; return 1; fi
    [ $rc -eq 0 ] || { game_broken "$DL_WHY"; return $?; }
    local got; got=$(fsize "$f")
    [ "$got" -ge "$min" ] || { game_broken "the downloaded file is far too small ($(human $got))"; return $?; }
    dl_status 92 "Unpacking the game..."
    rm -rf "$DL_DIR/unpack"; mkdir -p "$DL_DIR/unpack"
    unzip_to "$f" "$DL_DIR/unpack" </dev/null >>"$DL_DIR/curl.err" 2>&1 &
    dl_wait $!; rc=$?
    if [ $rc -eq 130 ]; then rm -rf "$DL_DIR/unpack"; dl_ui_close; echo "get the game: cancelled while unpacking (the download stays)"; return 1; fi
    [ $rc -eq 0 ] || { game_broken "the downloaded file is damaged (it does not unpack)"; return $?; }
    local root="" d
    while IFS= read -r d; do
        d="$(dirname "$d")"; game_valid "$d" && { root="$d"; break; }
    done < <(find "$DL_DIR/unpack" -maxdepth 4 -type d -iname DATA)
    [ -n "$root" ] || { game_broken "the download has no complete DATA and LEVELS folders"; return $?; }
    dl_status 98 "Setting up..."
    local rel="${root#"$DL_DIR/unpack"}"
    rm -rf "$GAME_ORIG"; mv "$DL_DIR/unpack" "$GAME_ORIG" || { game_broken "the game folder could not be created"; return $?; }
    GAME="$GAME_ORIG$rel"
    dl_status 100 done; [ -n "$SETUP_ON" ] || sleep 1; dl_ui_end; rm -rf "$GAME_DL"
    echo "get the game: installed in $GAME"
    return 0
}

# ======================================================================== Reforge pack
pack_broken() { # explain, offer the help page; the game always starts (1998 look). [code [number]] for the stats
    rm -rf "$PACK_DL"; dl_ui_close
    echo "reforge pack: broken - $1"
    stats_error "${2:-pack_download_failed}" "${3:-${ST_DETAIL:-}}"
    local b="${UA_NOTICE_AUTO:-}" msg="The Reforge pack download isn't working right now. The game starts with its 1998 look; UA Reforged offers the pack again on a later start.

What went wrong: $(no_paths "$1")."
    echo "notice: $msg" | tr '\n' ' '; echo
    [ -n "$b" ] || b="$(ask "$msg" "Open help page" "Try again later" "Open help page")"
    case "$b" in help|"Open help page") open_help "${L_PACK_HELP:-https://uareforged.pages.dev/downloads/#install-h}" ;; esac
    return 1
}

pack_total() { local t=0 n b s; while IFS="$(printf '\t')" read -r n b s; do t=$((t + b)); done <<< "$L_PACK_PARTS"; echo $t; }

pack_download() {
    local total done_b=0 name bytes sum f got rc part=0
    total=$(pack_total); ST_DETAIL=""
    DL_DIR="$PACK_DL"; mkdir -p "$DL_DIR" || return 1
    local free_k; free_k=$(df -Pk "$SUPPORT" | awk 'NR == 2 { print $4 }')
    if [ "$((free_k * 1024))" -lt "$((total * 28 / 10))" ]; then
        pack_broken "there is not enough free disk space (it needs about $(human $((total * 28 / 10))))"; return 1
    fi
    dl_ui_start "Reforge pack"
    # 1. check every link first, then download + check every part (a checked part is not fetched again)
    while IFS="$(printf '\t')" read -r name bytes sum; do
        [ -f "$DL_DIR/$name.ok" ] && continue
        dl_precheck "$L_PACK_URL$name" 0 0 "$bytes" || { pack_broken "$PC_WHY"; return 1; }
    done <<< "$L_PACK_PARTS"
    while IFS="$(printf '\t')" read -r name bytes sum; do
        f="$DL_DIR/$name"; part=$((part + 1))
        if [ -f "$f.ok" ] && [ -f "$f" ]; then done_b=$((done_b + bytes)); continue; fi
        [ "$(fsize "$f")" -gt "$bytes" ] && rm -f "$f"
        echo "reforge pack: downloading $L_PACK_URL$name"
        dl_precheck "$L_PACK_URL$name" 0 0 "$bytes" >/dev/null || { pack_broken "$PC_WHY"; return 1; }
        DL_WHAT="Downloading the Reforge pack"; DL_BASE=$done_b; DL_TOTAL=$total
        if [ "$(fsize "$f")" -lt "$bytes" ]; then
            dl_fetch "$L_PACK_URL$name" "$f"; rc=$?
            [ $rc -eq 130 ] && { dl_ui_close; return 130; }
            [ $rc -eq 0 ] || { pack_broken "$DL_WHY"; return 1; }
        fi
        dl_status "$(( (done_b + bytes) * 90 / total ))" "Checking $name..."
        got="$(sha256 "$f")"
        [ "$got" = "$sum" ] || { pack_broken "a downloaded part is damaged or not the expected file ($name)" pack_checksum_mismatch $part; return 1; }
        : > "$f.ok"; done_b=$((done_b + bytes))
    done <<< "$L_PACK_PARTS"
    # 2. unpack next to the download, rebuild the shared files and check them, then move into place
    dl_status 91 "Unpacking..."
    rm -rf "$DL_DIR/unpack"; mkdir -p "$DL_DIR/unpack"
    while IFS="$(printf '\t')" read -r name bytes sum; do
        unzip_to "$DL_DIR/$name" "$DL_DIR/unpack" </dev/null >/dev/null &
        dl_wait $!; rc=$?
        [ $rc -eq 130 ] && { rm -rf "$DL_DIR/unpack"; dl_ui_close; return 130; }
        [ $rc -eq 0 ] || { pack_broken "a downloaded part does not unpack ($name)"; return 1; }
    done <<< "$L_PACK_PARTS"
    local new="$DL_DIR/unpack/reforge-pack"
    [ -f "$new/install-links.sh" ] || { pack_broken "the download is not a Reforge pack"; return 1; }
    dl_status 95 "Setting up the pack (about a minute)..."
    PATH="$SHIM:$PATH" /bin/sh "$new/install-links.sh" --verify </dev/null &
    dl_wait $!; rc=$?
    [ $rc -eq 130 ] && { rm -rf "$DL_DIR/unpack"; dl_ui_close; return 130; }
    [ $rc -eq 0 ] || { pack_broken "the pack's files did not check out"; return 1; }
    conf version | head -1 > "$new/PACK-VERSION"
    if [ -e "$PACK" ]; then rm -rf "$PACK.old"; mv "$PACK" "$PACK.old" || { pack_broken "the old pack folder could not be replaced"; return 1; }; fi
    mv "$new" "$PACK" || { pack_broken "the pack folder could not be created"; return 1; }
    rm -rf "$PACK.old"
    dl_status 100 done; [ -n "$SETUP_ON" ] || sleep 1; dl_ui_end; rm -rf "$PACK_DL"
    echo "reforge pack: installed $(cat "$PACK/PACK-VERSION" 2>/dev/null)"
    return 0
}

# Offer the pack when none is installed. Never fatal: every failure starts the game with the 1998 look.
pack_offer() {
    [ -d "$PACK/hd-models" ] && return 0
    [ -n "$PACK_ASKED" ] && return 0
    [ -f "$DL_CONF" ] || return 0
    [ -f "$PACK_OFF" ] && [ -z "${UA_PACK_AUTO:-}" ] && return 0
    [ -n "${LINKS_DONE:-}" ] || links_load
    case "$L_PACK_URL" in ''|*PACK-URL-NOT-SET*) echo "reforge pack: none installed; this build has no download address"; return 0 ;; esac
    local total have=0 name bytes sum
    total=$(pack_total)
    while IFS="$(printf '\t')" read -r name bytes sum; do
        [ -f "$PACK_DL/$name" ] && have=$((have + $(fsize "$PACK_DL/$name")))
    done <<< "$L_PACK_PARTS"
    local answer="${UA_PACK_AUTO:-}"
    if [ -z "$answer" ]; then
        local msg verb="Download"
        if [ "$have" -gt 0 ]; then
            msg="The Reforge pack download stopped at $(human $have) of $(human $total). Resume it now?"; verb="Resume download"
        else
            msg="Download the Reforge pack ($(human $total))? It adds the new look: remastered units, buildings and skies, HD textures, restored videos and sharp menus. Without it the game keeps its 1998 look."
        fi
        msg="$msg

It is saved in UA Reforged's own folder ($(pack_disk) once installed). Your Urban Assault folder is not changed."
        case "$(ask "$msg" "$verb" "Don't ask again" "Not now" "$verb")" in
            "Don't ask again") : > "$PACK_OFF"; echo "reforge pack: offer turned off"; return 0 ;;
            "$verb") answer=yes ;;
            *) answer=no ;;
        esac
    fi
    [ "$answer" = yes ] || { echo "reforge pack: not now"; return 0; }
    pack_get; return 0
}
# pack_get: download + install now (the offer, the launcher's "Get the Reforge pack", or "Set up everything")
pack_get() {
    PACK_ASKED=1; rm -f "$PACK_OFF"
    pack_download; local rc=$?
    case $rc in
        0) ;;
        130) echo "reforge pack: download cancelled (the parts stay for a resume)"
             [ -z "${UA_PACK_AUTO:-}${UA_SETUP_AUTO:-}${UA_PLAY_AUTO:-}${UA_GAME_AUTO:-}" ] && ask "Download stopped. The game starts with the 1998 look; next time UA Reforged offers to resume the download." "OK" "OK" >/dev/null ;;
        *) echo "reforge pack: not installed (1998 look)" ;;
    esac
    return $rc
}

# ======================================================================== Set up everything (first launch)
# Sizes for the dialogs come from downloads.txt (docs/SIZES.md is the table they come from).
game_size() { local b; b="$(conf game_bytes | head -1)"; human "${b:-445000000}"; }
pack_disk() { local b; b="$(conf pack_disk_bytes | head -1)"; human "${b:-2400000000}"; }
pack_ready() { # the pack can be downloaded: this build (or links.json) has an address for it
    [ -f "$DL_CONF" ] || return 1
    [ -n "${LINKS_DONE:-}" ] || links_load
    case "$L_PACK_URL" in ''|*PACK-URL-NOT-SET*) return 1 ;; esac
    [ -n "$L_PACK_PARTS" ]
}

# setup_first: the first launch with no game folder. One dialog (Set up everything / I already have the game /
# Quit, and a checkbox for the Reforge pack), then one progress window for every download.
# Sets GAME, or exits the launcher cleanly (nothing half set up: the next start asks again).
setup_first() {
    is_game_dir "$GAME" && return 0
    [ -n "${LINKS_DONE:-}" ] || links_load
    local can_pack=0 withpack pick r text check
    pack_ready && [ ! -d "$PACK/hd-models" ] && can_pack=1
    while ! is_game_dir "$GAME"; do
        pick="${UA_SETUP_AUTO:-}"; withpack=1
        if [ -z "$pick" ] && [ -n "${UA_GAME_AUTO:-}" ]; then
            case "$UA_GAME_AUTO" in get|"Get the game") pick=everything; [ "${UA_PACK_AUTO:-}" = no ] && pick=game-only ;; *) pick=quit ;; esac
        fi
        if [ -n "$pick" ]; then
            case "$pick" in game-only|folder-only) withpack=0 ;; esac
            echo "setup: test answer $pick"
        else
            check=""
            text="UA Reforged needs the original Urban Assault game files. It can fetch everything for you:

- The game: the free OpenUA Standalone Package ($(game_size)), the full 1998 game plus the Metropolis Dawn add-on, straight from the Metropolis Dawn community's server. They host it, not UA Reforged; the game files belong to Microsoft."
            if [ $can_pack = 1 ]; then
                text="$text
- The Reforge pack ($(human "$(pack_total)")): the new look, with remastered units, buildings and skies, HD textures, restored videos and sharp menus."
                check="Include the Reforge pack ($(human "$(pack_total)") download, $(pack_disk) installed)"
            fi
            text="$text

Everything is saved in UA Reforged's own folder. Already have the game? Choose its folder instead."
            [ -n "$L_MESSAGE" ] && text="$text

$L_MESSAGE"
            r="$(dlg "Set up UA Reforged" "$text" "$check" 1 0 "Set up everything" "I already have the game" "Quit")"
            IFS=$'\t' read -r pick withpack _ <<< "$r"
            echo "setup: chose [${pick:-closed}], pack ${withpack:-0}"
            case "$pick" in "Set up everything") pick=everything ;; "I already have the game") pick=folder ;; *) pick=quit ;; esac
        fi
        [ $can_pack = 1 ] || withpack=0
        case "$pick" in
            everything|game-only)
                setup_run get "$withpack"; case $? in
                    0) ;;
                    1) [ -n "${UA_SETUP_AUTO:-}${UA_GAME_AUTO:-}" ] && exit 0 ;;
                    2) echo "stopped: help page opened, nothing set up"; exit 0 ;;
                    3) if game_pick; then setup_run folder "$withpack"; elif [ -n "${UA_SETUP_AUTO:-}${UA_GAME_AUTO:-}" ]; then exit 0; fi ;;
                esac ;;
            folder|folder-only)
                if game_pick; then setup_run folder "$withpack"; elif [ -n "${UA_SETUP_AUTO:-}" ]; then exit 0; fi ;;
            *) echo "stopped at the first-launch choice"; exit 0 ;;
        esac
    done
}
game_choose() { setup_first; }   # (the older name)

# setup_run get|folder <pack 0|1>: the downloads in one progress window with the overall progress and the current
# step. Returns like game_get (0 ok, 1 cancelled, 2 help page opened, 3 choose a folder instead). The pack never
# stops the game: on a problem it shows its own notice and the game starts with the 1998 look.
setup_run() {
    local kind="$1" withpack="$2" gb=0 pb=0 steps=0 n=0 rc total
    [ "$kind" = get ] && { gb="$(conf game_bytes | head -1)"; gb="${gb:-445000000}"; }
    [ "$withpack" = 1 ] && pb=$(pack_total)
    if [ "$withpack" != 1 ] && [ ! -d "$PACK/hd-models" ] && pack_ready; then
        : > "$PACK_OFF"; PACK_ASKED=1; echo "setup: Reforge pack skipped (the launcher offers it)"
    fi
    [ "$gb" -gt 0 ] && steps=$((steps + 1)); [ "$pb" -gt 0 ] && steps=$((steps + 1))
    [ $steps -gt 0 ] || return 0
    total=$((gb + pb))
    SETUP_ON=1; UI_DIR="$SETUP_PROGRESS"; UI_PID=""; rm -rf "$UI_DIR"; mkdir -p "$UI_DIR"
    SETUP_BASE=0; SETUP_SPAN=100; SETUP_STEP=""
    dl_ui_start "Setting up UA Reforged"
    if [ "$gb" -gt 0 ]; then
        n=$((n + 1)); SETUP_STEP="Step $n of $steps: the game ($(human "$gb"))"; SETUP_BASE=0; SETUP_SPAN=$(( gb * 100 / total ))
        echo "setup: $SETUP_STEP"
        game_get; rc=$?
        if [ $rc -ne 0 ]; then SETUP_ON=""; dl_ui_close; rm -rf "$SETUP_PROGRESS"; return $rc; fi
    fi
    if [ "$pb" -gt 0 ]; then
        n=$((n + 1)); SETUP_STEP="Step $n of $steps: the Reforge pack ($(human "$pb"))"; SETUP_BASE=$(( gb * 100 / total )); SETUP_SPAN=$(( 100 - SETUP_BASE ))
        echo "setup: $SETUP_STEP"
        pack_get
    fi
    SETUP_ON=""; dl_ui_close; rm -rf "$SETUP_PROGRESS"
    echo "setup: done"
    return 0
}

# ======================================================================== the launcher (every start)
# play_menu: one Play button per campaign (when the game has Metropolis Dawn; the last choice is the default button),
# and Get the Reforge pack while none is installed (Resume after a stopped download). Buttons rather than a list,
# so it works with a game controller on a TV. The launcher is the pack offer, so pack_offer asks nothing more this
# launch. Sets campaign=base|ghor|taer, or exits (Quit).
play_menu() {
    local sel=0 r b md=0 can_pack text have pb name bytes sum pbtn
    PACK_ASKED=1
    if [ -d "$GAME/Ghor_env" ] || [ -d "$RUN/Ghor_env" ]; then md=1; fi
    case "$(cat "$SUPPORT/campaign.txt" 2>/dev/null)" in ghor) sel=1 ;; taer) sel=2 ;; esac
    [ $md = 1 ] || sel=0
    while :; do
        can_pack=0; [ ! -d "$PACK/hd-models" ] && pack_ready && can_pack=1
        have=0; pbtn="Get the Reforge pack"
        if [ $can_pack = 1 ]; then
            while IFS="$(printf '\t')" read -r name bytes sum; do
                [ -f "$PACK_DL/$name" ] && have=$((have + $(fsize "$PACK_DL/$name")))
            done <<< "$L_PACK_PARTS"
            [ "$have" -gt 0 ] && pbtn="Resume the Reforge pack"
        fi
        b="${UA_PLAY_AUTO:-}"
        if [ -n "$b" ]; then
            echo "launcher: test answer $b"; UA_PLAY_AUTO=play   # a test run answers once, then plays
        else
            text="Ready to play."
            [ $md = 1 ] && text="Ready to play. Metropolis Dawn is the expansion, with two more campaigns."
            pb="$(human "$(pack_total)")"
            if [ $can_pack = 1 ] && [ "$have" -gt 0 ]; then text="$text

The Reforge pack download stopped at $(human "$have") of $pb. Resume it to get the new look; until then the game has its 1998 look."
            elif [ $can_pack = 1 ]; then text="$text

The Reforge pack is not installed, so the game has its 1998 look. Get the Reforge pack adds the new look ($pb download)."
            fi
            local btns=("Play")
            [ $md = 1 ] && btns=("Play Urban Assault" "Play Metropolis Dawn: Ghorkov" "Play Metropolis Dawn: Taerkasten")
            [ $can_pack = 1 ] && btns+=("$pbtn"); btns+=("Quit")
            r="$(dlg "UA Reforged" "$text" "" 0 "$sel" "${btns[@]}")"
            b="${r%%$'\t'*}"
            echo "launcher: chose [${b:-closed}]"
        fi
        case "$b" in
            Play|play) break ;;
            "Play Urban Assault") sel=0; break ;;
            "Play Metropolis Dawn: Ghorkov") sel=1; break ;;
            "Play Metropolis Dawn: Taerkasten") sel=2; break ;;
            "Get the Reforge pack"|"Resume the Reforge pack"|pack) pack_get ;;
            *) echo "stopped at the launcher"; exit 0 ;;
        esac
    done
    campaign=base
    if [ $md = 1 ]; then case "$sel" in 1) campaign=ghor ;; 2) campaign=taer ;; esac; fi
    printf '%s\n' "$campaign" > "$SUPPORT/campaign.txt"
}

game_pick() {
    local g
    if [ -n "${UA_TEST_FOLDER:-}" ]; then
        g="$UA_TEST_FOLDER"; echo "folder picker (test run): a folder was chosen"
    else
        [ -n "${UA_NOTICE_AUTO:-}" ] && { echo "folder picker (test run: skipped)"; exit 0; }
        local prompt="Choose your Urban Assault folder (it contains DATA and LEVELS). UA Reforged only reads it."
        if command -v zenity >/dev/null 2>&1; then
            g="$(zenity --file-selection --directory --title="$prompt" 2>/dev/null)" || return 1
        elif command -v kdialog >/dev/null 2>&1; then
            g="$(kdialog --title "$prompt" --getexistingdirectory "$HOME" 2>/dev/null)" || return 1
        else
            # no folder picker on this desktop: the game folder can be named on the command line instead
            echo "folder picker: neither zenity nor kdialog is installed"
            ask "This desktop has no folder picker (zenity or kdialog). Start UA Reforged once from a terminal with your game folder:

UA-Reforged.AppImage --game-dir <your Urban Assault folder>

(the folder that contains DATA and LEVELS). Or choose Set up everything to download the game." "OK" "OK" >/dev/null
            return 1
        fi
    fi
    g="${g%/}"
    if is_game_dir "$g"; then GAME="$g"; return 0; fi
    [ -n "${UA_TEST_FOLDER:-}" ] && { echo "folder picker (test run): not a game folder"; return 1; }
    stats_error game_folder_invalid
    ask "That folder has no DATA and LEVELS folders. Pick the folder of your Urban Assault install." "OK" "OK" >/dev/null
    return 1
}

# ======================================================================== anonymous usage stats (docs/usage-stats.md)
# stats_ask: the question, once (when usage-stats.txt doesn't exist yet). The focus starts on "No thanks" (Return,
# Esc or closing = No; a controller's A on the focused button), so only a deliberate choice says yes. Only the answer is written here; the game makes the random install number and does
# all the sending. Test runs: UA_STATS_AUTO=yes|no|dismiss answers it; UA_CAMPAIGN without UA_STATS_AUTO skips it.
STATS_HEADING="Help make UA Reforged better?"
STATS_TEXT="Share anonymous stats and crash reports, and we'll use them to fix bugs and keep the game running smoothly. Nothing personal is ever sent, and you can change your mind anytime in Options.

Want the details? uareforged.pages.dev/privacy"
STATS_YES="Yes, happy to help"
STATS_NO="No thanks"
stats_ask() {
    local f="$SUPPORT/usage-stats.txt" b ans=no
    [ -f "$f" ] && return 0
    b="${UA_STATS_AUTO:-}"
    if [ -n "$b" ]; then
        echo "usage stats: test answer $b"
    elif [ -n "${UA_CAMPAIGN:-}" ]; then
        echo "usage stats: not asked (test run)"; return 0
    else
        b="$("$DIALOG" ask --font "$FONT" --title "UA Reforged" --heading "$STATS_HEADING" --text "$STATS_TEXT" \
             --default 1 -- "$STATS_YES" "$STATS_NO" 2>/dev/null </dev/null | cut -f1)"
    fi
    case "$b" in "$STATS_YES"|yes) ans=yes ;; esac
    printf '# UA Reforged: anonymous usage stats (Options > Send anonymous usage stats)\nanswer = %s\n' "$ans" > "$f.tmp" && mv -f "$f.tmp" "$f"
    echo "usage stats: answer $ans (${b:-dismissed})"
}

# stats_error <code> [number]: a problem the game reports at its next start, only while the player has said yes.
# One line per problem (a code from the fixed list, a number), nothing else; the game keeps at most 20 events.
stats_error() {
    grep -qx 'answer = yes' "$SUPPORT/usage-stats.txt" 2>/dev/null || return 0
    local d=""
    case "${2:-}" in ''|*[!0-9]*) ;; *) d="$(printf '\tdetail=%s' "$2")" ;; esac
    printf 'event=error\tcode=%s%s\n' "$1" "$d" >> "$SUPPORT/usage-stats-queue.txt"
    echo "usage stats: $1 kept for the game"
}

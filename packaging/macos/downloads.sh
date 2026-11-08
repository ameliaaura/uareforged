# UA Reforged - downloads the app can make for the player (sourced by the app launcher, bash):
#   * "Get the game": the community OpenUA Standalone Package, fetched straight from the Metropolis Dawn
#     community's own server on the player's Mac (UA Reforged never hosts, mirrors or ships those files);
#   * the Reforge pack (the new look), checked against SHA-256 sums shipped in the app.
#
# Where from: Contents/Resources/downloads.txt (built-in values). Before a download the app reads a small
# links.json from the UA Reforged site (links_url) so a broken link can be fixed without a new app. That file is
# only parsed (links.js: strict types, https only, an allowlist of hosts); it can never run anything. If it is
# missing or invalid the built-in values are used.
#
# Failsafe: every link is checked first (a 1 KB ranged GET: must answer 2xx, must not be a web page, must start
# like a zip, must have a sane size). During a download a stall of 30 s or a dropped connection is retried twice
# (resuming where the server allows it). Any break cleans up the partial files and shows a plain notice with
# "Open help page" (the site's help) or another way on; the app is never left half set up.
#
# First launch is one step ("Set up everything", setup_first): one dialog for the game + the Reforge pack (or "I already
# have the game", a checkbox to skip the pack, "Install on my Quest"), then one progress window for both downloads.
# Every later start shows a small launcher (play_menu): the campaign, Play, Install on my Quest, and Get the Reforge
# pack while none is installed. Dialogs never show file paths (a player's path holds their account name); the log does.
#
# Testing only: UA_LINKS_URL, UA_GAME_URL, UA_PACK_URL override addresses (UA_LINKS_ALLOW_LOCAL=1 also accepts
# http://127.0.0.1 in links.json); UA_SETUP_AUTO=everything|game-only|folder|folder-only|quest|quit answers the
# first-launch dialog (UA_GAME_AUTO=get = "everything", with the pack unless UA_PACK_AUTO=no; UA_TEST_FOLDER=<dir>
# answers the folder picker); UA_PACK_AUTO=yes|no answers the pack offer; UA_NOTICE_AUTO=help|folder|later answers
# the notices (and opens no browser); UA_PLAY_AUTO=play|quest|pack|quit answers the launcher once (UA_CAMPAIGN skips
# it); UA_PACK_NOUI=1 shows no progress window; UA_DIALOG_SNAP=<folder> saves each setup/launcher dialog as a PNG
# there and answers it with its default (the usage stats question: dismissed = no); UA_STATS_AUTO=yes|no|dismiss
# answers the usage stats question.

DL_CONF="$CONTENTS/Resources/downloads.txt"
PACK_DL="$SUPPORT/reforge-pack-download"
GAME_DL="$SUPPORT/game-download"
GAME_ORIG="$SUPPORT/game-original"
PACK_OFF="$SUPPORT/reforge-pack-offer-off"
DL_UA="UA-Reforged (macOS)"

SETUP_PROGRESS="$SUPPORT/setup-progress"
SETUP_ON=""        # set while "Set up everything" runs: one progress window for every step
PACK_ASKED=""      # the pack was offered (or chosen) this launch: no second offer
ICON="$CONTENTS/Resources/AppIcon.icns"

conf() { awk -F'\t' -v k="$1" '$1 == k { sub(/^[^\t]*\t/, ""); print }' "$DL_CONF"; }
human() { awk -v b="$1" 'BEGIN { if (b >= 1e9) printf "%.1f GB", b / 1e9; else if (b >= 1e6) printf "%d MB", b / 1e6; else printf "%d KB", (b + 999) / 1000 }'; }

# ask <text> <default button> <button>...: a dialog; prints the button (empty when closed). Text goes in as an
# argument, never into the script, so no text can change what runs.
ask() {
    /usr/bin/osascript - "$@" 2>/dev/null <<'EOF'
on run argv
    set t to item 1 of argv
    set d to item 2 of argv
    set bs to items 3 thru -1 of argv
    try
        return button returned of (display dialog t with title "UA Reforged" buttons bs default button d)
    on error
        return ""
    end try
end run
EOF
}

# no_paths <text>: what a dialog may show. A player's paths hold their account name (it shows when they stream or share
# a screenshot), so our folders get friendly names and any other path under the home folder is left out.
no_paths() {
    local t="$1" a="UA Reforged's folder" b="the UA Reforged app" c="your home folder"
    t="${t//"$SUPPORT"/$a}"; t="${t//"$CONTENTS"/$b}"; t="${t//"$HOME"/$c}"
    printf '%s' "$t" | sed -E 's#/(Users|Volumes|private|var|tmp)/[^ ,;:)"]*#a file on this Mac#g'
}
# dlg <heading> <text> <check label> <check 0|1> <menu label> <menu items> <menu index> <buttons...>:
# the larger dialog (dialog.js). Prints "<button>\t<check>\t<menu index>".
dlg() { /usr/bin/osascript -l JavaScript "$CONTENTS/Resources/dialog.js" "$ICON" "$@" 2>/dev/null </dev/null; }
# show_in_finder <file or folder>: reveal it in a Finder window (the player sees it; no path in our text)
show_in_finder() { [ -n "${UA_NOTICE_AUTO:-}" ] && { echo "show in Finder: $1 (test run: not opened)"; return; }; /usr/bin/open -R "$1"; }

open_help() {
    if [ -n "${UA_NOTICE_AUTO:-}" ]; then echo "help page: $1 (test run: not opened)"; return; fi
    echo "help page: $1"; /usr/bin/open "$1"
}

# ---------------------------------------------------------------- links: built-in values, then the site's links.json
links_load() {
    L_GAME_URL="$(conf game_url | head -1)"; L_GAME_MIN="$(conf game_min_bytes | head -1)"
    L_GAME_MAX="$(conf game_max_bytes | head -1)"
    L_PACK_URL="$(conf url | head -1)"; L_PACK_PARTS="$(conf part)"
    L_HELP="$(conf help_url | head -1)"; L_PACK_HELP="$(conf pack_help_url | head -1)"; L_MESSAGE=""
    L_QUEST_APK_URL=""; L_QUEST_APK=""; L_QUEST_HELP=""; L_ADB_MAC=""
    local src="${UA_LINKS_URL:-$(conf links_url | head -1)}" tmp="$SUPPORT/links.json.tmp" out proto="=https"
    local allow; allow="$(conf allow_host | tr '\n' ' ')"
    local loc=0; [ -n "${UA_LINKS_ALLOW_LOCAL:-}" ] && { loc=1; proto="=https,http"; }
    rm -f "$tmp"
    if [ -n "$src" ] && /usr/bin/curl -fsSL --proto "$proto" --proto-redir "$proto" --max-time 8 --connect-timeout 5 \
            --max-filesize 16384 -A "$DL_UA" -o "$tmp" "$src" </dev/null 2>/dev/null; then
        if out="$(/usr/bin/osascript -l JavaScript "$CONTENTS/Resources/links.js" "$tmp" "$allow" "$loc" 2>&1)"; then
            local k v parts=""
            while IFS="$(printf '\t')" read -r k v; do
                case "$k" in
                    game_url) L_GAME_URL="$v" ;; game_min_bytes) L_GAME_MIN="$v" ;; pack_url) L_PACK_URL="$v" ;;
                    help_url) L_HELP="$v" ;; pack_help_url) L_PACK_HELP="$v" ;; message) L_MESSAGE="$v" ;;
                    quest_apk_url) L_QUEST_APK_URL="$v" ;; quest_apk) L_QUEST_APK="$v" ;; quest_help_url) L_QUEST_HELP="$v" ;;
                    adb_mac) L_ADB_MAC="$v" ;;
                    part) parts="$parts$v"$'\n' ;;
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
    [ -n "${UA_QUEST_APK_URL:-}" ] && L_QUEST_APK_URL="$UA_QUEST_APK_URL"
    case "$L_PACK_URL" in */) ;; *) L_PACK_URL="$L_PACK_URL/" ;; esac
    LINKS_DONE=1
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
        /usr/bin/osascript -l JavaScript "$CONTENTS/Resources/progress.js" "$UI_DIR" "$1" >/dev/null 2>&1 </dev/null &
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
    local have; have=$(stat -f %z "$DL_FILE" 2>/dev/null || echo 0); local now=$((DL_BASE + have))
    if [ "${DL_TOTAL:-0}" -gt 0 ]; then dl_status "$((now * 90 / DL_TOTAL))" "$DL_WHAT: $(human $now) of $(human $DL_TOTAL)"
    else dl_status 5 "$DL_WHAT: $(human $now)"; fi
}

# ---------------------------------------------------------------- link check: dl_precheck <url> <min> <max> <exact>
# sets PC_SIZE (0 = unknown), PC_RANGES (1/0); on failure returns 1 with PC_WHY in plain words
dl_precheck() {
    local url="$1" min="${2:-0}" max="${3:-0}" exact="${4:-0}" h="$DL_DIR/check.h" b="$DL_DIR/check.b" w rc code type
    PC_SIZE=0; PC_RANGES=0; PC_WHY=""; ST_DETAIL=""
    w="$(/usr/bin/curl -sS -L --max-time 20 --connect-timeout 10 --max-redirs 8 -r 0-1023 -A "$DL_UA" \
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
    if [ "$(head -c 4 "$b" | od -An -tx1 | tr -d ' \n')" != "504b0304" ]; then
        PC_WHY="the link does not lead to a zip file"; echo "link check: $url -> $PC_WHY"; return 1
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
        /usr/bin/curl -fL -sS --connect-timeout 15 --speed-limit 1 --speed-time 30 --max-redirs 8 \
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
    local free_k; free_k=$(df -k "$SUPPORT" | awk 'NR == 2 { print $4 }')
    [ "$((free_k * 1024))" -lt 1500000000 ] && { game_broken "there is not enough free disk space (it needs about 1.5 GB)"; return $?; }
    dl_ui_start "Urban Assault (from the Metropolis Dawn community)"
    dl_precheck "$url" "$min" "$max" 0 || { game_broken "$PC_WHY"; return $?; }
    local f="$DL_DIR/openua-package.zip"
    DL_WHAT="Downloading the game"; DL_BASE=0; DL_TOTAL="$PC_SIZE"; DL_MAXSIZE="$max"
    dl_fetch "$url" "$f"; local rc=$?; DL_MAXSIZE=""
    if [ $rc -eq 130 ]; then dl_ui_close; echo "get the game: cancelled (the part stays for a resume)"; return 1; fi
    [ $rc -eq 0 ] || { game_broken "$DL_WHY"; return $?; }
    local got; got=$(stat -f %z "$f" 2>/dev/null || echo 0)
    [ "$got" -ge "$min" ] || { game_broken "the downloaded file is far too small ($(human $got))"; return $?; }
    dl_status 92 "Unpacking the game..."
    rm -rf "$DL_DIR/unpack"; mkdir -p "$DL_DIR/unpack"
    /usr/bin/ditto -x -k "$f" "$DL_DIR/unpack" </dev/null 2>>"$DL_DIR/curl.err" &
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
    local free_k; free_k=$(df -k "$SUPPORT" | awk 'NR == 2 { print $4 }')
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
        [ "$(stat -f %z "$f" 2>/dev/null || echo 0)" -gt "$bytes" ] && rm -f "$f"
        echo "reforge pack: downloading $L_PACK_URL$name"
        dl_precheck "$L_PACK_URL$name" 0 0 "$bytes" >/dev/null || { pack_broken "$PC_WHY"; return 1; }
        DL_WHAT="Downloading the Reforge pack"; DL_BASE=$done_b; DL_TOTAL=$total
        if [ "$(stat -f %z "$f" 2>/dev/null || echo 0)" -lt "$bytes" ]; then
            dl_fetch "$L_PACK_URL$name" "$f"; rc=$?
            [ $rc -eq 130 ] && { dl_ui_close; return 130; }
            [ $rc -eq 0 ] || { pack_broken "$DL_WHY"; return 1; }
        fi
        dl_status "$(( (done_b + bytes) * 90 / total ))" "Checking $name..."
        got="$(/usr/bin/shasum -a 256 "$f" | cut -d' ' -f1)"
        [ "$got" = "$sum" ] || { pack_broken "a downloaded part is damaged or not the expected file ($name)" pack_checksum_mismatch $part; return 1; }
        : > "$f.ok"; done_b=$((done_b + bytes))
    done <<< "$L_PACK_PARTS"
    # 2. unpack next to the download, rebuild the shared files and check them, then move into place
    dl_status 91 "Unpacking..."
    rm -rf "$DL_DIR/unpack"; mkdir -p "$DL_DIR/unpack"
    while IFS="$(printf '\t')" read -r name bytes sum; do
        /usr/bin/ditto -x -k "$DL_DIR/$name" "$DL_DIR/unpack" </dev/null &
        dl_wait $!; rc=$?
        [ $rc -eq 130 ] && { rm -rf "$DL_DIR/unpack"; dl_ui_close; return 130; }
        [ $rc -eq 0 ] || { pack_broken "a downloaded part does not unpack ($name)"; return 1; }
    done <<< "$L_PACK_PARTS"
    local new="$DL_DIR/unpack/reforge-pack"
    [ -f "$new/install-links.sh" ] || { pack_broken "the download is not a Reforge pack"; return 1; }
    dl_status 95 "Setting up the pack (about a minute)..."
    /bin/sh "$new/install-links.sh" --verify </dev/null &
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
        [ -f "$PACK_DL/$name" ] && have=$((have + $(stat -f %z "$PACK_DL/$name" 2>/dev/null || echo 0)))
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
# Install on my Quest / Quit, and a checkbox for the Reforge pack), then one progress window for every download.
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

• The game: the free OpenUA Standalone Package ($(game_size)), the full 1998 game plus the Metropolis Dawn add-on, straight from the Metropolis Dawn community's server. They host it, not UA Reforged; the game files belong to Microsoft."
            if [ $can_pack = 1 ]; then
                text="$text
• The Reforge pack ($(human "$(pack_total)")): the new look, with remastered units, buildings and skies, HD textures, restored videos and sharp menus."
                check="Include the Reforge pack ($(human "$(pack_total)") download, $(pack_disk) installed)"
            fi
            text="$text

Everything is saved in UA Reforged's own folder. Already have the game? Choose its folder instead."
            [ -n "$L_MESSAGE" ] && text="$text

$L_MESSAGE"
            q_usb_seen 2>/dev/null && text="$text

A Meta Quest is plugged in: choose Install on my Quest to put UA Reforged VR on it."
            r="$(dlg "Set up UA Reforged" "$text" "$check" 1 "" "" 0 "Set up everything" "I already have the game" "Install on my Quest…" "Quit")"
            IFS=$'\t' read -r pick withpack _ <<< "$r"
            echo "setup: chose [${pick:-closed}], pack ${withpack:-0}"
            case "$pick" in "Set up everything") pick=everything ;; "I already have the game") pick=folder ;; "Install on my Quest…") pick=quest ;; *) pick=quit ;; esac
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
            quest)
                quest_install; [ $? -eq 2 ] && { echo "stopped after the Quest install"; exit 0; }
                [ -n "${UA_SETUP_AUTO:-}" ] && { echo "stopped after the Quest install (test run)"; exit 0; } ;;
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
# play_menu: the campaign (when the game has Metropolis Dawn; the last choice is remembered), Play, Install on my
# Quest, and Get the Reforge pack while none is installed (Resume after a stopped download). The launcher is the
# pack offer, so pack_offer asks nothing more this launch. Sets campaign=base|ghor|taer, or exits (Quit).
play_menu() {
    local items="" label="" sel=0 r b idx md=0 can_pack text have pb name bytes sum pbtn
    PACK_ASKED=1
    if [ -d "$GAME/Ghor_env" ] || [ -d "$RUN/Ghor_env" ]; then
        md=1; label="Campaign:"; items="Urban Assault|Metropolis Dawn: Ghorkov|Metropolis Dawn: Taerkasten"
    fi
    case "$(cat "$SUPPORT/campaign.txt" 2>/dev/null)" in ghor) sel=1 ;; taer) sel=2 ;; esac
    while :; do
        can_pack=0; [ ! -d "$PACK/hd-models" ] && pack_ready && can_pack=1
        have=0; pbtn="Get the Reforge pack…"
        if [ $can_pack = 1 ]; then
            while IFS="$(printf '\t')" read -r name bytes sum; do
                [ -f "$PACK_DL/$name" ] && have=$((have + $(stat -f %z "$PACK_DL/$name" 2>/dev/null || echo 0)))
            done <<< "$L_PACK_PARTS"
            [ "$have" -gt 0 ] && pbtn="Resume the Reforge pack…"
        fi
        b="${UA_PLAY_AUTO:-}"; idx=$sel
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
            local btns=("Play" "Install on my Quest…"); [ $can_pack = 1 ] && btns+=("$pbtn"); btns+=("Quit")
            r="$(dlg "UA Reforged" "$text" "" 0 "$label" "$items" "$sel" "${btns[@]}")"
            IFS=$'\t' read -r b _ idx <<< "$r"
            echo "launcher: chose [${b:-closed}]"
        fi
        case "$b" in
            Play|play) break ;;
            "Install on my Quest…"|quest) quest_install; [ $? -eq 2 ] && { echo "stopped after the Quest install"; exit 0; } ;;
            "Get the Reforge pack…"|"Resume the Reforge pack…"|pack) pack_get ;;
            *) echo "stopped at the launcher"; exit 0 ;;
        esac
        sel="${idx:-0}"
    done
    campaign=base
    if [ $md = 1 ]; then case "${idx:-0}" in 1) campaign=ghor ;; 2) campaign=taer ;; esac; fi
    printf '%s\n' "$campaign" > "$SUPPORT/campaign.txt"
}

game_pick() {
    local g
    if [ -n "${UA_TEST_FOLDER:-}" ]; then
        g="$UA_TEST_FOLDER"; echo "folder picker (test run): a folder was chosen"
    else
        [ -n "${UA_NOTICE_AUTO:-}" ] && { echo "folder picker (test run: skipped)"; exit 0; }
        g="$(/usr/bin/osascript -e 'POSIX path of (choose folder with prompt "Choose your Urban Assault folder (it contains DATA and LEVELS). UA Reforged only reads it.")' 2>/dev/null)" || return 1
    fi
    g="${g%/}"
    if is_game_dir "$g"; then GAME="$g"; return 0; fi
    [ -n "${UA_TEST_FOLDER:-}" ] && { echo "folder picker (test run): not a game folder"; return 1; }
    stats_error game_folder_invalid
    ask "That folder has no DATA and LEVELS folders. Pick the folder of your Urban Assault install." "OK" "OK" >/dev/null
    return 1
}

# ======================================================================== anonymous usage stats (docs/usage-stats.md)
# stats_ask: the question, once (when usage-stats.txt doesn't exist yet). Nothing is pre-selected: Return presses
# nothing, Esc or closing = No. Only the answer is written here; the game makes the random install number and does
# all the sending. Test runs: UA_STATS_AUTO=yes|no|dismiss answers it; UA_CAMPAIGN without UA_STATS_AUTO skips it.
STATS_HEADING="Help make UA Reforged better?"
STATS_TEXT="With anonymous usage stats on, UA Reforged sends a crash report if the game crashes and a short performance summary after each mission, so we can find and fix crashes, slowdowns and bugs quickly. It also counts how many people play.

No name, no IP, no files, no personal data, and it's completely optional. You can change it anytime in Options."
STATS_YES="Yes, send anonymous stats"
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
        b="$(/usr/bin/osascript -l JavaScript "$CONTENTS/Resources/consent.js" "$ICON" "$STATS_HEADING" "$STATS_TEXT" "$STATS_YES" "$STATS_NO" 2>/dev/null </dev/null)"
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

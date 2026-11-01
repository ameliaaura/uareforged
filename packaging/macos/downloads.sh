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
# Testing only: UA_LINKS_URL, UA_GAME_URL, UA_PACK_URL override addresses (UA_LINKS_ALLOW_LOCAL=1 also accepts
# http://127.0.0.1 in links.json); UA_GAME_AUTO=get answers the first-launch choice; UA_PACK_AUTO=yes|no answers
# the pack offer; UA_NOTICE_AUTO=help|folder|later answers the notices (and opens no browser); UA_PACK_NOUI=1 shows
# no progress window.

DL_CONF="$CONTENTS/Resources/downloads.txt"
PACK_DL="$SUPPORT/reforge-pack-download"
GAME_DL="$SUPPORT/game-download"
GAME_ORIG="$SUPPORT/game-original"
PACK_OFF="$SUPPORT/reforge-pack-offer-off"
DL_UA="UA-Reforged (macOS)"

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
                    part) parts="$parts$v"$'\n' ;;
                esac
            done <<< "$out"
            [ -n "$parts" ] && L_PACK_PARTS="${parts%$'\n'}"
            echo "links: $src accepted"
        else
            echo "links: $src rejected (${out##*: }); built-in links used"
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

# ---------------------------------------------------------------- progress window + cancel
dl_status() { printf '%s\t%s\n' "$1" "$2" > "$DL_DIR/status.tmp" && mv -f "$DL_DIR/status.tmp" "$DL_DIR/status"; }
dl_ui_start() { # dl_ui_start <title>
    UI_PID=""; rm -f "$DL_DIR/cancel"; dl_status 0 "Checking the download link..."
    if [ -z "${UA_PACK_NOUI:-}" ]; then
        /usr/bin/osascript -l JavaScript "$CONTENTS/Resources/progress.js" "$DL_DIR" "$1" >/dev/null 2>&1 </dev/null &
        UI_PID=$!
    fi
}
dl_ui_end() { [ -n "${UI_PID:-}" ] && kill "$UI_PID" 2>/dev/null; UI_PID=""; rm -f "$DL_DIR/status" "$DL_DIR/cancel"; }
dl_wait() { # wait for a background job; 130 = the player pressed Cancel
    local pid=$1
    while kill -0 "$pid" 2>/dev/null; do
        if [ -f "$DL_DIR/cancel" ]; then kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; return 130; fi
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
    PC_SIZE=0; PC_RANGES=0; PC_WHY=""
    w="$(/usr/bin/curl -sS -L --max-time 20 --connect-timeout 10 --max-redirs 8 -r 0-1023 -A "$DL_UA" \
         -D "$h" -o "$b" -w '%{http_code}\t%{content_type}' "$url" </dev/null 2>"$DL_DIR/check.err")"; rc=$?
    code="${w%%$'\t'*}"; type="$(printf '%s' "${w#*$'\t'}" | tr 'A-Z' 'a-z')"
    if [ $rc -ne 0 ]; then
        case $rc in 6) PC_WHY="the server name could not be found" ;; 7) PC_WHY="the server could not be reached" ;;
            28) PC_WHY="the server did not answer in time" ;; 35|51|60) PC_WHY="the secure connection failed" ;;
            *) PC_WHY="the connection failed (curl $rc)" ;; esac
        echo "link check: $url -> $PC_WHY"; return 1
    fi
    case "$code" in 200|206) ;; *) PC_WHY="the server answered HTTP $code instead of the file"; echo "link check: $url -> $PC_WHY"; return 1 ;; esac
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
    rm -rf "$GAME_DL"; dl_ui_end
    echo "get the game: broken - $1"
    local b="${UA_NOTICE_AUTO:-}"
    if [ -z "$b" ]; then
        b="$(ask "The game download link isn't working right now (it's hosted by the Metropolis Dawn community). We'll open the UA Reforged help page with other ways to get it.

What went wrong: $1." "Open help page" "Choose a folder instead" "Open help page")"
    fi
    case "$b" in folder|"Choose a folder instead") return 3 ;; esac
    open_help "${L_HELP:-https://uareforged.pages.dev/downloads/#nogame-h}"; return 2
}

# game_get: prints nothing; on success sets GAME. returns 0 ok, 1 cancelled (back to the choices),
# 2 help page opened (stop), 3 choose a folder instead
game_get() {
    [ -n "${LINKS_DONE:-}" ] || links_load
    local url="$L_GAME_URL" min="${L_GAME_MIN:-100000000}" max="${L_GAME_MAX:-5000000000}"
    if [ -z "${UA_GAME_AUTO:-}" ]; then
        local note="UA Reforged will download the OpenUA Standalone Package (the full original game plus the Metropolis Dawn add-on, about 445 MB) straight from the Metropolis Dawn community's server. It is hosted by the Metropolis Dawn community, not by UA Reforged, and the game files belong to Microsoft.

It goes to ~/Library/Application Support/UA Reforged/game-original."
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
    if [ $rc -eq 130 ]; then dl_ui_end; echo "get the game: cancelled (the part stays for a resume)"; return 1; fi
    [ $rc -eq 0 ] || { game_broken "$DL_WHY"; return $?; }
    local got; got=$(stat -f %z "$f" 2>/dev/null || echo 0)
    [ "$got" -ge "$min" ] || { game_broken "the downloaded file is far too small ($(human $got))"; return $?; }
    dl_status 92 "Unpacking the game..."
    rm -rf "$DL_DIR/unpack"; mkdir -p "$DL_DIR/unpack"
    /usr/bin/ditto -x -k "$f" "$DL_DIR/unpack" </dev/null 2>>"$DL_DIR/curl.err" &
    dl_wait $!; rc=$?
    if [ $rc -eq 130 ]; then rm -rf "$DL_DIR/unpack"; dl_ui_end; return 1; fi
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
    dl_status 100 done; sleep 1; dl_ui_end; rm -rf "$GAME_DL"
    echo "get the game: installed in $GAME"
    return 0
}

# ======================================================================== Reforge pack
pack_broken() { # explain, offer the help page; the game always starts (1998 look)
    rm -rf "$PACK_DL"; dl_ui_end
    echo "reforge pack: broken - $1"
    local b="${UA_NOTICE_AUTO:-}"
    if [ -z "$b" ]; then
        b="$(ask "The Reforge pack download isn't working right now. The game starts with its 1998 look; UA Reforged offers the pack again on a later start.

What went wrong: $1." "Open help page" "Try again later" "Open help page")"
    fi
    case "$b" in help|"Open help page") open_help "${L_PACK_HELP:-https://uareforged.pages.dev/downloads/#install-h}" ;; esac
    return 1
}

pack_total() { local t=0 n b s; while IFS="$(printf '\t')" read -r n b s; do t=$((t + b)); done <<< "$L_PACK_PARTS"; echo $t; }

pack_download() {
    local total done_b=0 name bytes sum f got rc
    total=$(pack_total)
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
        f="$DL_DIR/$name"
        if [ -f "$f.ok" ] && [ -f "$f" ]; then done_b=$((done_b + bytes)); continue; fi
        [ "$(stat -f %z "$f" 2>/dev/null || echo 0)" -gt "$bytes" ] && rm -f "$f"
        echo "reforge pack: downloading $L_PACK_URL$name"
        dl_precheck "$L_PACK_URL$name" 0 0 "$bytes" >/dev/null || { pack_broken "$PC_WHY"; return 1; }
        DL_WHAT="Downloading the Reforge pack"; DL_BASE=$done_b; DL_TOTAL=$total
        if [ "$(stat -f %z "$f" 2>/dev/null || echo 0)" -lt "$bytes" ]; then
            dl_fetch "$L_PACK_URL$name" "$f"; rc=$?
            [ $rc -eq 130 ] && { dl_ui_end; return 130; }
            [ $rc -eq 0 ] || { pack_broken "$DL_WHY"; return 1; }
        fi
        dl_status "$(( (done_b + bytes) * 90 / total ))" "Checking $name..."
        got="$(/usr/bin/shasum -a 256 "$f" | cut -d' ' -f1)"
        [ "$got" = "$sum" ] || { pack_broken "a downloaded part is damaged or not the expected file ($name)"; return 1; }
        : > "$f.ok"; done_b=$((done_b + bytes))
    done <<< "$L_PACK_PARTS"
    # 2. unpack next to the download, rebuild the shared files and check them, then move into place
    dl_status 91 "Unpacking..."
    rm -rf "$DL_DIR/unpack"; mkdir -p "$DL_DIR/unpack"
    while IFS="$(printf '\t')" read -r name bytes sum; do
        /usr/bin/ditto -x -k "$DL_DIR/$name" "$DL_DIR/unpack" </dev/null &
        dl_wait $!; rc=$?
        [ $rc -eq 130 ] && { rm -rf "$DL_DIR/unpack"; dl_ui_end; return 130; }
        [ $rc -eq 0 ] || { pack_broken "a downloaded part does not unpack ($name)"; return 1; }
    done <<< "$L_PACK_PARTS"
    local new="$DL_DIR/unpack/reforge-pack"
    [ -f "$new/install-links.sh" ] || { pack_broken "the download is not a Reforge pack"; return 1; }
    dl_status 95 "Setting up the pack (about a minute)..."
    /bin/sh "$new/install-links.sh" --verify </dev/null &
    dl_wait $!; rc=$?
    [ $rc -eq 130 ] && { rm -rf "$DL_DIR/unpack"; dl_ui_end; return 130; }
    [ $rc -eq 0 ] || { pack_broken "the pack's files did not check out"; return 1; }
    conf version | head -1 > "$new/PACK-VERSION"
    if [ -e "$PACK" ]; then rm -rf "$PACK.old"; mv "$PACK" "$PACK.old" || { pack_broken "the old pack folder could not be replaced"; return 1; }; fi
    mv "$new" "$PACK" || { pack_broken "the pack folder could not be created"; return 1; }
    rm -rf "$PACK.old"
    dl_status 100 done; sleep 1; dl_ui_end; rm -rf "$PACK_DL"
    echo "reforge pack: installed $(cat "$PACK/PACK-VERSION" 2>/dev/null)"
    return 0
}

# Offer the pack when none is installed. Never fatal: every failure starts the game with the 1998 look.
pack_offer() {
    [ -d "$PACK/hd-models" ] && return 0
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

It goes to ~/Library/Application Support/UA Reforged. Your Urban Assault folder is not changed."
        case "$(ask "$msg" "$verb" "Don't ask again" "Not now" "$verb")" in
            "Don't ask again") : > "$PACK_OFF"; echo "reforge pack: offer turned off"; return 0 ;;
            "$verb") answer=yes ;;
            *) answer=no ;;
        esac
    fi
    [ "$answer" = yes ] || { echo "reforge pack: not now"; return 0; }
    rm -f "$PACK_OFF"
    pack_download; local rc=$?
    case $rc in
        0) ;;
        130) echo "reforge pack: download cancelled (the parts stay for a resume)"
             [ -z "${UA_PACK_AUTO:-}" ] && ask "Download stopped. The game starts with the 1998 look; next time UA Reforged offers to resume the download." "OK" "OK" >/dev/null ;;
        *) echo "reforge pack: not installed (1998 look)" ;;
    esac
    return 0
}

# First launch with no game folder: "I have the game" (the folder picker) or "Get the game".
# Sets GAME, or exits the launcher cleanly (nothing half set up: the next start asks again).
game_choose() {
    local pick
    while ! is_game_dir "$GAME"; do
        pick="${UA_GAME_AUTO:-}"
        if [ -z "$pick" ]; then
            pick="$(ask "UA Reforged needs the original Urban Assault game files.

Do you have the game on this Mac? If not, UA Reforged can fetch the community OpenUA Standalone Package (about 445 MB) from the Metropolis Dawn community for you." "Get the game" "Quit" "I have the game - choose folder" "Get the game")"
        fi
        case "$pick" in
            get|"Get the game")
                game_get; case $? in
                    0) ;; 1) [ -n "${UA_GAME_AUTO:-}" ] && exit 0 ;;
                    2) echo "stopped: help page opened, nothing set up"; exit 0 ;;
                    3) game_pick || [ -z "${UA_GAME_AUTO:-}" ] || exit 0 ;;
                esac ;;
            "I have the game - choose folder") game_pick ;;
            *) echo "stopped at the first-launch choice"; exit 0 ;;
        esac
    done
}

game_pick() {
    [ -n "${UA_NOTICE_AUTO:-}" ] && { echo "folder picker (test run: skipped)"; exit 0; }
    local g
    g="$(/usr/bin/osascript -e 'POSIX path of (choose folder with prompt "Choose your Urban Assault folder (it contains DATA and LEVELS). UA Reforged only reads it.")' 2>/dev/null)" || return 1
    g="${g%/}"
    if is_game_dir "$g"; then GAME="$g"; return 0; fi
    ask "That folder has no DATA and LEVELS folders. Pick the folder of your Urban Assault install." "OK" "OK" >/dev/null
    return 1
}

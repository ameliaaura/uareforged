#!/bin/bash
# UA Reforged (Linux) - the launcher: AppRun in the AppImage, ./ua-reforged in the tar.gz folder.
# A port of packaging/macos/launcher.sh; the same steps and the same files, in the Linux places.
#
# The package ships no game data. On first launch the player gets the community game package ("Set up everything")
# or picks their own Urban Assault folder (the one with DATA/ and LEVELS/). That folder is only read, never written.
#
# The engine reads and writes everything relative to its working directory, so the launcher builds one per user:
#
#   ${XDG_DATA_HOME:-~/.local/share}/UA Reforged/game/
#
# Writable state (profiles, settings, the engine log ENV/ypa_log.txt) is COPIED there once from the player's
# folder; every read-only data dir is a symlink back to the player's folder, refreshed on every launch. The
# engine's own shaders (res/) come from the package, because the Reforged renderer needs its versions; fonts/
# merges both.
#
# Optional Reforge pack: ${XDG_DATA_HOME:-~/.local/share}/UA Reforged/reforge-pack/ (hd-models/, hd-textures/,
# hd-fmv/, ui/, ground/ inside it). Without it the game draws the 1998 look. The pack stores files that many levels
# share once; on first launch (and after a pack update) the launcher runs the pack's install-links.sh, which
# rebuilds the per-level layout as hard links. Downloads, the first-launch setup and the small launcher dialog live
# in downloads.sh; every address is in usr/share/ua-reforged/downloads.txt (+ the site's links.json).
#
# Dialogs show no file paths: a player's path holds their account name. launcher.log keeps the paths.
#
# Anonymous usage stats (docs/usage-stats.md): asked once after the setup ("Help make UA Reforged better?"); off
# unless the player says yes; Options in the game changes it. The answer lives in usage-stats.txt next to
# launcher.log; the game (UA_SETTINGS_DIR) does the sending.
#
# Command line: --game-dir <folder> uses (and remembers) that game folder; --add-to-menu adds UA Reforged to the
# desktop's application menu; --help. Everything else goes to the engine.
# Environment (for testing): UA_GAME_DIR=<folder> skips the first-launch setup, UA_CAMPAIGN=base|ghor|taer skips the
# launcher (and, without UA_STATS_AUTO, the stats question); the switches listed in downloads.sh answer the dialogs.

if [ -n "${APPDIR:-}" ] && [ -x "$APPDIR/usr/bin/UA_source" ]; then HERE="$APPDIR"
else HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"; fi
SHARE="$HERE/usr/share/ua-reforged"
ENGINE="$HERE/usr/bin/UA_source"
DIALOG="$HERE/usr/bin/ua-dialog"
OWN="$SHARE/engine"
DATA_HOME="${XDG_DATA_HOME:-}"; case "$DATA_HOME" in /*) ;; *) DATA_HOME="$HOME/.local/share" ;; esac
SUPPORT="$DATA_HOME/UA Reforged"
RUN="$SUPPORT/game"
PACK="$SUPPORT/reforge-pack"
CONF="$SUPPORT/game-folder.txt"
LOG="$SUPPORT/launcher.log"
SHIM="$SUPPORT/.tools"

WRITABLE="ENV SAVE Ghorsave Taersave Ghor_env Taer_env NUCLEUS.INI joyconf.txt"
OWNED="res fonts hd-models hd-textures hd-fmv"   # built by the launcher, never linked straight to the player's folder

# before the log exists: the terminal (if any) and a dialog
alert() {
    echo "UA Reforged: $1" >&2
    "$DIALOG" ask --font "$OWN/fonts/LiberationSans-Regular.ttf" --title "UA Reforged" --text "$1" -- OK >/dev/null 2>&1 </dev/null
}
fail() { alert "$1"; exit 1; }

ARGS=(); GAME_ARG=""; MENU=0
while [ $# -gt 0 ]; do
    case "$1" in
        --game-dir) GAME_ARG="${2:-}"; shift ;;
        --game-dir=*) GAME_ARG="${1#--game-dir=}" ;;
        --add-to-menu) MENU=1 ;;
        -h|--help)
            cat <<'EOF'
UA Reforged for Linux. Start it without arguments: the first start sets everything up.
  --game-dir <folder>   use your own Urban Assault folder (the one with DATA and LEVELS); remembered
  --add-to-menu         add UA Reforged to your desktop's application menu (for this AppImage / folder)
Your saves, settings, the game download and the Reforge pack live in ~/.local/share/UA Reforged/
(launcher.log there and game/ENV/ypa_log.txt are the logs).
EOF
            exit 0 ;;
        *) ARGS+=("$1") ;;
    esac
    shift
done
set -- ${ARGS[@]+"${ARGS[@]}"}

[ "$(uname -m)" = x86_64 ] || fail "This build needs a 64-bit PC (x86_64). ARM computers are not supported."
mkdir -p "$RUN" || fail "UA Reforged could not create its folder in your home folder. Check that your disk is not full."

if [ $MENU = 1 ]; then
    target="${APPIMAGE:-$HERE/ua-reforged}"
    mkdir -p "$DATA_HOME/applications" "$DATA_HOME/icons/hicolor/256x256/apps"
    cp "$HERE/ua-reforged.png" "$DATA_HOME/icons/hicolor/256x256/apps/ua-reforged.png"
    sed -e "s|^Exec=.*|Exec=\"$target\"|" "$HERE/ua-reforged.desktop" > "$DATA_HOME/applications/ua-reforged.desktop"
    echo "UA Reforged added to the application menu (it starts $target)."
    exit 0
fi

exec >>"$LOG" 2>&1
echo "=== $(date '+%Y-%m-%d %H:%M:%S') launch from $HERE (${XDG_SESSION_TYPE:-?} session, DISPLAY=${DISPLAY:-} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-})"

# the window's class / app id = the .desktop file's name, so the desktop shows the right icon and name
export SDL_VIDEO_X11_WMCLASS=ua-reforged SDL_VIDEO_WAYLAND_WMCLASS=ua-reforged
# the pack's install-links.sh checks files with shasum; some distributions only have sha256sum
if ! command -v shasum >/dev/null 2>&1; then
    mkdir -p "$SHIM"
    printf '#!/bin/sh\n# shasum -a 256 <file> for systems without perl shasum\n[ "$1" = -a ] && shift 2\nexec sha256sum "$@"\n' > "$SHIM/shasum"
    chmod 755 "$SHIM/shasum"
fi

# is_game_dir <folder>: has DATA and LEVELS (any letter case: Linux file names are case-sensitive)
is_game_dir() {
    [ -n "$1" ] && [ -d "$1" ] || return 1
    [ -n "$(find "$1" -maxdepth 1 -type d -iname DATA -print -quit 2>/dev/null)" ] &&
    [ -n "$(find "$1" -maxdepth 1 -type d -iname LEVELS -print -quit 2>/dev/null)" ]
}

GAME="${UA_GAME_DIR:-}"
if [ -n "$GAME_ARG" ]; then
    GAME="$(cd "$GAME_ARG" 2>/dev/null && pwd)" || GAME=""
    is_game_dir "$GAME" || fail "That folder has no DATA and LEVELS folders. Pick the folder of your Urban Assault install."
fi
[ -z "$GAME" ] && [ -f "$CONF" ] && GAME="$(cat "$CONF")"
# downloads the app can make: "Get the game" (from the Metropolis Dawn community) and the Reforge pack
. "$SHARE/downloads.sh"
# no game folder yet: "Set up UA Reforged" (the game + the Reforge pack in one step, or your own folder)
setup_first
[ -n "${UA_GAME_DIR:-}" ] || printf '%s\n' "$GAME" > "$CONF"
echo "game folder: $GAME"
# anonymous usage stats: asked once (off unless the player says yes); the game reads the answer from here
stats_ask
export UA_SETTINGS_DIR="$SUPPORT"
# the launcher: campaign, Play, Get the Reforge pack (UA_CAMPAIGN skips it)
campaign="${UA_CAMPAIGN:-}"
[ -n "$campaign" ] || play_menu

# writable state: copied once from the player's folder
for name in $WRITABLE; do
    if [ ! -e "$RUN/$name" ] && [ -e "$GAME/$name" ]; then
        cp -RL "$GAME/$name" "$RUN/$name" || fail "Could not copy $name from your game folder."
        chmod -R u+w "$RUN/$name"
    fi
done

# read-only game data: symlinks into the player's folder (never the writable or owned names)
for src in "$GAME"/*; do
    name="$(basename "$src")"
    case " $WRITABLE $OWNED " in *" $name "*) continue ;; esac
    if [ -L "$RUN/$name" ] || [ ! -e "$RUN/$name" ]; then ln -sfn "$src" "$RUN/$name"; fi
done
# the engine's own shaders (the Reforged renderer needs its versions). The AppImage's folder changes every start,
# so these links are refreshed each time.
if [ -L "$RUN/res" ] || [ ! -e "$RUN/res" ]; then ln -sfn "$OWN/res" "$RUN/res"; fi
# fonts: the engine's free fonts, plus any extra fonts your game folder brings (same names win)
[ -L "$RUN/fonts" ] && rm -f "$RUN/fonts"
mkdir -p "$RUN/fonts"
for f in "$OWN/fonts"/* "$GAME/fonts"/*; do [ -f "$f" ] && ln -sfn "$f" "$RUN/fonts/$(basename "$f")"; done
# the optional Reforge pack: offer the one-click download when none is installed
pack_offer
if [ -f "$PACK/LINKS.tsv" ] && { [ ! -f "$PACK/.links-done" ] || [ "$PACK/LINKS.tsv" -nt "$PACK/.links-done" ]; }; then
    echo "reforge pack: rebuilding shared files (install-links.sh)"
    PATH="$SHIM:$PATH" /bin/sh "$PACK/install-links.sh" || alert "The Reforge pack is incomplete (see launcher.log). The game starts with what is there."
fi
if [ -d "$PACK/hd-models" ]; then
    ln -sfn "$PACK/hd-models" "$RUN/hd-models"
    export UA_HDMODELS_DIR="$RUN/hd-models"
    [ -d "$PACK/hd-models/sky" ] && export UA_SKY_DIR="$PACK/hd-models/sky"
    echo "reforge pack: $PACK"
else
    [ -L "$RUN/hd-models" ] && rm -f "$RUN/hd-models"
    echo "reforge pack: none (1998 models)"
fi
# pack folders the engine reads from its working directory: the pack's copy wins over the game folder's
for name in hd-textures hd-fmv; do
    if [ -d "$PACK/$name" ]; then ln -sfn "$PACK/$name" "$RUN/$name"
    elif [ -L "$RUN/$name" ] || [ ! -e "$RUN/$name" ]; then
        if [ -e "$GAME/$name" ]; then ln -sfn "$GAME/$name" "$RUN/$name"; else rm -f "$RUN/$name"; fi
    fi
done
[ -d "$PACK/ui" ] && export UA_UI_DIR="$PACK/ui/"
[ -f "$PACK/ground/pack.txt" ] && export UA_R2_GROUND_DIR="$PACK/ground"

ENVARGS=()
case "$campaign" in
    ghor) ENVARGS=(-env Ghor_env) ;;
    taer) ENVARGS=(-env Taer_env) ;;
esac

# no network side features in this build
unset UA_RADIO UA_RADIO_LLM

cd "$RUN" || fail "UA Reforged could not open its game folder."
echo "campaign=$campaign"
if [ ${#ENVARGS[@]} -gt 0 ]; then "$ENGINE" "${ENVARGS[@]}" "$@"; else "$ENGINE" "$@"; fi
rc=$?
echo "engine exit $rc"
if [ $rc -ne 0 ]; then
    echo "stopped: the engine exited with code $rc"
    msg="The game stopped unexpectedly (code $rc). Its log file has the details."
    echo "notice: $msg"
    if [ -n "${UA_NOTICE_AUTO:-}" ]; then [ "$UA_NOTICE_AUTO" = help ] && show_in_folder "$RUN/ENV/ypa_log.txt"
    elif [ "$(ask "$msg" "OK" "Show the log folder" "OK")" = "Show the log folder" ]; then show_in_folder "$RUN/ENV/ypa_log.txt"; fi
    exit 1
fi
exit 0

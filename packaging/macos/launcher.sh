#!/bin/bash
# UA Reforged - app launcher (Contents/MacOS/UA Reforged).
#
# The app ships no game data. On first launch the player picks their own Urban Assault
# folder (the one with DATA/ and LEVELS/). That folder is only read, never written.
#
# The engine reads and writes everything relative to its working directory, so the
# launcher builds one per user:
#
#   ~/Library/Application Support/UA Reforged/game/
#
# Writable state (profiles, settings, the engine log ENV/ypa_log.txt) is COPIED there once
# from the player's folder; every read-only data dir is a symlink back to the player's
# folder, refreshed on every launch. The engine's own shaders (res/) come from the app
# bundle, because the Reforged renderer needs its versions; fonts/ merges both.
#
# Optional Reforge pack: ~/Library/Application Support/UA Reforged/reforge-pack/
# (hd-models/, hd-textures/, hd-fmv/, ui/, ground/ inside it). Without it the game draws the 1998 look.
# The pack stores files that many levels share once; on first launch (and after a pack update) the
# launcher runs the pack's install-links.sh, which rebuilds the per-level layout as hard links.
# First launch with no game folder: one "Set up UA Reforged" step fetches the community OpenUA Standalone Package
# (from the Metropolis Dawn community's own server) and the Reforge pack in one go, or uses the player's own folder.
# Every later start shows a small launcher: the campaign, Play, Install on my Quest, and Get the Reforge pack while
# none is installed. All of it lives in downloads.sh; every address is in Contents/Resources/downloads.txt
# (+ the site's links.json).
#
# "Install on my Quest" (quest.sh): installs UA Reforged VR on a Meta Quest plugged in by USB. On the first-launch
# dialog and the launcher, offered by itself when one is plugged in at launch; the Option key while opening the app
# (or --quest) still works too.
#
# Dialogs show no file paths: a player's path holds their account name. launcher.log keeps the paths.
#
# Environment (for testing): UA_GAME_DIR=<folder> skips the first-launch setup,
# UA_CAMPAIGN=base|ghor|taer skips the launcher. Arguments go to the engine.

CONTENTS="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="$CONTENTS/MacOS/UA_source"
OWN="$CONTENTS/Resources/engine"
SUPPORT="$HOME/Library/Application Support/UA Reforged"
RUN="$SUPPORT/game"
PACK="$SUPPORT/reforge-pack"
CONF="$SUPPORT/game-folder.txt"
LOG="$SUPPORT/launcher.log"

WRITABLE="ENV SAVE Ghorsave Taersave Ghor_env Taer_env NUCLEUS.INI joyconf.txt"
OWNED="res fonts hd-models hd-textures hd-fmv"   # built by the launcher, never linked straight to the player's folder

alert() {
    /usr/bin/osascript -e "display alert \"UA Reforged\" message \"$1\" as critical" >/dev/null 2>&1
}
fail() { alert "$1"; exit 1; }

[ "$(uname -m)" = arm64 ] || fail "This build needs a Mac with Apple Silicon (M1 or later). Intel Macs are not supported."
mkdir -p "$RUN" || fail "UA Reforged could not create its folder in your Library. Check that your disk is not full."
exec >>"$LOG" 2>&1
echo "=== $(date '+%Y-%m-%d %H:%M:%S') launch from $CONTENTS"

# the Option key held while opening the app, or --quest: offer "Install on my Quest" this time
QUEST_ASKED=0
ARGS=()
for a in "$@"; do if [ "$a" = --quest ]; then QUEST_ASKED=1; else ARGS+=("$a"); fi; done
set -- ${ARGS[@]+"${ARGS[@]}"}
[ "$(/usr/bin/osascript -l JavaScript -e 'ObjC.import("AppKit"); ($.NSEvent.modifierFlags & 524288) ? "1" : "0"' 2>/dev/null)" = 1 ] && QUEST_ASKED=1

is_game_dir() { [ -d "$1/DATA" ] && [ -d "$1/LEVELS" ]; }

GAME="${UA_GAME_DIR:-}"
[ -z "$GAME" ] && [ -f "$CONF" ] && GAME="$(cat "$CONF")"
# downloads the app can make: "Get the game" (from the Metropolis Dawn community) and the Reforge pack
. "$CONTENTS/Resources/downloads.sh"
# "Install on my Quest": UA Reforged VR onto a Meta Quest over USB
. "$CONTENTS/Resources/quest.sh"
# no game folder yet: "Set up UA Reforged" (the game + the Reforge pack in one step, or your own folder)
setup_first
[ -n "${UA_GAME_DIR:-}" ] || printf '%s\n' "$GAME" > "$CONF"
echo "game folder: $GAME"
# a Meta Quest plugged in (or Option held): offer to install UA Reforged VR on it
quest_offer
# the launcher: campaign, Play, Install on my Quest, Get the Reforge pack (UA_CAMPAIGN skips it)
campaign="${UA_CAMPAIGN:-}"
[ -n "$campaign" ] || play_menu

# writable state: copied once from the player's folder
for name in $WRITABLE; do
    if [ ! -e "$RUN/$name" ] && [ -e "$GAME/$name" ]; then
        cp -RH "$GAME/$name" "$RUN/$name" || fail "Could not copy $name from your game folder."
        chmod -R u+w "$RUN/$name"
    fi
done

# read-only game data: symlinks into the player's folder (never the writable or owned names)
for src in "$GAME"/*; do
    name="$(basename "$src")"
    case " $WRITABLE $OWNED " in *" $name "*) continue ;; esac
    if [ -L "$RUN/$name" ] || [ ! -e "$RUN/$name" ]; then ln -sfn "$src" "$RUN/$name"; fi
done
# the engine's own shaders (the Reforged renderer needs its versions)
if [ -L "$RUN/res" ] || [ ! -e "$RUN/res" ]; then ln -sfn "$OWN/res" "$RUN/res"; fi
# fonts: the engine's free fonts, plus any extra fonts your game folder brings (same names win)
[ -L "$RUN/fonts" ] && rm -f "$RUN/fonts"
mkdir -p "$RUN/fonts"
for f in "$OWN/fonts"/* "$GAME/fonts"/*; do [ -f "$f" ] && ln -sfn "$f" "$RUN/fonts/$(basename "$f")"; done
# the optional Reforge pack: offer the one-click download when none is installed
pack_offer
if [ -f "$PACK/LINKS.tsv" ] && { [ ! -f "$PACK/.links-done" ] || [ "$PACK/LINKS.tsv" -nt "$PACK/.links-done" ]; }; then
    echo "reforge pack: rebuilding shared files (install-links.sh)"
    /bin/sh "$PACK/install-links.sh" || alert "The Reforge pack is incomplete (see launcher.log). The game starts with what is there."
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
    if [ -n "${UA_NOTICE_AUTO:-}" ]; then [ "$UA_NOTICE_AUTO" = help ] && show_in_finder "$RUN/ENV/ypa_log.txt"
    elif [ "$(ask "$msg" "OK" "Show log in Finder" "OK")" = "Show log in Finder" ]; then show_in_finder "$RUN/ENV/ypa_log.txt"; fi
    exit 1
fi
exit 0

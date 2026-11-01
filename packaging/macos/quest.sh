# UA Reforged - "Install on my Quest" (sourced by the app launcher after downloads.sh, bash).
#
# Installs UA Reforged VR (the Meta Quest app) on a headset plugged into this Mac with a USB-C cable, and can
# copy this Mac's game files over too, so the headset does not download the game again. Nothing else on the
# headset is touched: no other app is removed, no setting is changed.
#
# How it is offered: when a Meta Quest is plugged in at launch (seen on USB, no tools needed) and it does not
# have this version yet; hold the Option key while opening the app to get the choice at any time; or start the
# app with --quest.
#
# adb: Google's Android SDK Platform-Tools. Google's licence for them does not allow passing them on, so they are
# not in the app: on first use the app downloads Google's own zip from dl.google.com (a pinned release, checked
# by size and SHA-256, adb_mac in downloads.txt) into ~/Library/Application Support/UA Reforged/platform-tools.
# The VR app (quest_apk_url, checked by size and SHA-256 against quest_apk) is cached in .../quest.
# Both can be changed by the site's links.json, under the same rules as every other link.
#
# Testing only: UA_QUEST=install runs the flow at launch, UA_QUEST=only runs it and quits, UA_QUEST=offer makes
# the usual launch offer and quits after it; UA_QUEST_APK=<file>
# uses a local APK instead of the download (it must still match quest_apk, unless UA_QUEST_APK_SHA256 gives
# another checksum); UA_QUEST_ADB=<adb> uses another adb (no download); UA_QUEST_AUTO=yes|no answers the
# questions (and UA_NOTICE_AUTO the notices, as for the other downloads); UA_QUEST_COPY=yes|no answers the game
# files offer; UA_QUEST_USB=0|1 overrides the USB check (UA_QUEST_USB_SERIAL the serial it reports).

Q_PKG="$(conf quest_package | head -1)"; Q_PKG="${Q_PKG:-io.github.ameliaaura.uareforged.vr}"
Q_DIR="$SUPPORT/quest"
Q_PT="$SUPPORT/platform-tools"
Q_OFF="$SUPPORT/quest-offer-off"
Q_DONE="$SUPPORT/quest/installed.txt"   # serial<TAB>versionCode of headsets this Mac installed (no repeat offers)
Q_REMOTE="/sdcard/Android/data/$Q_PKG/files/ua"

q_ask() { # q_ask <text> <default> <button>...: the dialog, or the test answer
    if [ -n "${UA_QUEST_AUTO:-}" ]; then
        local d="$2" b; printf 'quest: asked "%s"\n' "$(printf '%s' "$1" | tr '\n' ' ' | tr -s ' ')" >&2; shift 2
        if [ "$UA_QUEST_AUTO" = yes ]; then b="$d"; else for b in "$@"; do [ "$b" != "$d" ] && break; done; fi
        printf '%s' "$b"
        echo "quest: answered [$b] (test run)" >&2; return
    fi
    ask "$@"
}

# a Meta headset on USB (vendor 0x2833), with or without developer mode
q_usb_seen() {
    [ -n "${UA_QUEST_USB:-}" ] && { [ "$UA_QUEST_USB" = 1 ]; return; }
    /usr/sbin/ioreg -p IOUSB -l -w0 2>/dev/null | grep -q '"idVendor" = 10291$'
}
q_usb_serials() { [ -n "${UA_QUEST_USB_SERIAL:-}" ] && { echo "$UA_QUEST_USB_SERIAL"; return; }; /usr/sbin/ioreg -p IOUSB -l -w0 2>/dev/null | awk '/"idVendor" = 10291$/{m=1} m && /"USB Serial Number"/{gsub(/.*= "|"$/,""); print; m=0}'; }

# q_run <seconds> <out file> <command...>: runs a command with a time limit; 124 = it took too long
q_run() {
    local t=$1 out=$2; shift 2
    "$@" >"$out" 2>&1 </dev/null &
    local pid=$! n=0
    while kill -0 $pid 2>/dev/null; do
        [ $n -ge $((t * 4)) ] && { kill $pid 2>/dev/null; wait $pid 2>/dev/null; return 124; }
        sleep 0.25; n=$((n + 1))
    done
    wait $pid
}
q_adb() { local t=$1; shift; q_run "$t" "$Q_DIR/adb.out" "$ADB" "$@"; }

# ---------------------------------------------------------------- links (built-in, then links.json via links_load)
q_links() {
    [ -n "${LINKS_DONE:-}" ] || links_load
    Q_APK_URL="${L_QUEST_APK_URL:-$(conf quest_apk_url | head -1)}"
    local a="${L_QUEST_APK:-$(conf quest_apk | head -1)}"
    IFS="$(printf '\t')" read -r Q_APK_NAME Q_APK_BYTES Q_APK_SHA Q_APK_CODE Q_APK_VER <<< "$a"
    Q_HELP="${L_QUEST_HELP:-$(conf quest_help_url | head -1)}"; Q_HELP="${Q_HELP:-https://uareforged.pages.dev/downloads/#quest-computer-h}"
    Q_DEVHELP="$(conf quest_devmode_url | head -1)"; Q_DEVHELP="${Q_DEVHELP:-https://uareforged.pages.dev/downloads/#quest-devmode-h}"
    IFS="$(printf '\t')" read -r Q_ADB_URL Q_ADB_BYTES Q_ADB_SHA <<< "${L_ADB_MAC:-$(conf adb_mac | head -1)}"
}

# q_broken <why>: the failsafe notice (help page or back to the game). returns 2 if the help page was opened
q_broken() {
    dl_ui_end 2>/dev/null
    echo "quest: broken - $1"
    local b="${UA_NOTICE_AUTO:-}"
    if [ -z "$b" ]; then
        b="$(ask "UA Reforged VR could not be installed on your headset. Nothing on the headset was changed.

What went wrong: $1." "Open help page" "Play on this Mac" "Open help page")"
    fi
    case "$b" in help|"Open help page") open_help "$Q_HELP"; return 2 ;; esac
    return 1
}

# ---------------------------------------------------------------- adb: Google's own zip, downloaded on first use
q_adb_ready() {
    if [ -n "${UA_QUEST_ADB:-}" ]; then ADB="$UA_QUEST_ADB"; [ -x "$ADB" ]; return; fi
    ADB="$Q_PT/platform-tools/adb"
    [ -x "$ADB" ] && [ "$(cat "$Q_PT/.zip-sha256" 2>/dev/null)" = "$Q_ADB_SHA" ] && return 0
    [ -n "$Q_ADB_URL" ] || { Q_WHY="this build has no download address for Google's platform tools"; return 1; }
    DL_DIR="$Q_PT.download"; mkdir -p "$DL_DIR" || { Q_WHY="the folder for Google's platform tools could not be created"; return 1; }
    dl_ui_start "Google's Android platform tools (adb)"
    dl_precheck "$Q_ADB_URL" 0 0 "$Q_ADB_BYTES" || { Q_WHY="Google's platform tools download: $PC_WHY"; rm -rf "$DL_DIR"; return 1; }
    local f="$DL_DIR/platform-tools.zip" rc
    DL_WHAT="Downloading Google's platform tools"; DL_BASE=0; DL_TOTAL="$Q_ADB_BYTES"; DL_MAXSIZE="$Q_ADB_BYTES"
    dl_fetch "$Q_ADB_URL" "$f"; rc=$?; DL_MAXSIZE=""
    [ $rc -eq 130 ] && { dl_ui_end; rm -rf "$DL_DIR"; Q_WHY="cancelled"; return 130; }
    [ $rc -eq 0 ] || { Q_WHY="Google's platform tools download: $DL_WHY"; rm -rf "$DL_DIR"; return 1; }
    dl_status 95 "Checking Google's platform tools..."
    local got; got="$(/usr/bin/shasum -a 256 "$f" | cut -d' ' -f1)"
    [ "$got" = "$Q_ADB_SHA" ] || { echo "quest: platform tools sha256 $got"; Q_WHY="the downloaded platform tools are not the expected file"; rm -rf "$DL_DIR"; return 1; }
    rm -rf "$DL_DIR/x"; mkdir -p "$DL_DIR/x"
    /usr/bin/ditto -x -k "$f" "$DL_DIR/x" </dev/null || { Q_WHY="the downloaded platform tools do not unpack"; rm -rf "$DL_DIR"; return 1; }
    [ -x "$DL_DIR/x/platform-tools/adb" ] || { Q_WHY="the downloaded platform tools have no adb"; rm -rf "$DL_DIR"; return 1; }
    rm -rf "$Q_PT"; mv "$DL_DIR/x" "$Q_PT" && printf '%s\n' "$got" > "$Q_PT/.zip-sha256"
    rm -rf "$DL_DIR"; dl_ui_end
    echo "quest: platform tools installed in $Q_PT"
    [ -x "$ADB" ]
}

# ---------------------------------------------------------------- the headset
# q_find: sets Q_SER, or Q_STATE = none | nodev (on USB, no developer mode) | unauthorized | offline | many
q_find() {
    Q_SER=""; Q_STATE=none; Q_MODEL=""
    q_adb 20 start-server || true
    local tries=0 line ser st n=0 un=0 off=0
    while :; do
        q_adb 10 devices -l; n=0; un=0; off=0; Q_SER=""
        while read -r ser st rest; do
            [ -n "$ser" ] || continue
            case "$ser" in List|\*) continue ;; esac
            # Quest headsets only (phones and other Android devices are left alone)
            case " $rest " in *model:Quest*|*device:eureka*|*device:panther*|*device:hollywood*|*device:seacliff*|*device:monterey*|*product:eureka*|*product:panther*) ;;
                *) case "$st" in unauthorized|offline) ;; *) continue ;; esac ;; esac
            case "$st" in
                device) n=$((n + 1)); Q_SER="$ser"; Q_MODEL="$(printf '%s' "$rest" | sed -n 's/.*model:\([^ ]*\).*/\1/p' | tr _ ' ')" ;;
                unauthorized) un=$((un + 1)) ;;
                offline) off=$((off + 1)) ;;
            esac
        done < "$Q_DIR/adb.out"
        if [ $n -gt 1 ]; then Q_STATE=many; Q_SER=""; return 1; fi
        [ $n -eq 1 ] && { Q_STATE=device; return 0; }
        # a freshly started adb can need a few seconds to see the headset
        tries=$((tries + 1)); [ $tries -ge 4 ] && break
        sleep 1.5
    done
    if [ $un -gt 0 ]; then Q_STATE=unauthorized
    elif [ $off -gt 0 ]; then Q_STATE=offline
    elif q_usb_seen; then Q_STATE=nodev
    fi
    return 1
}

# q_installed: Q_HAVE = the versionCode on the headset (empty = not installed), Q_HAVE_NAME = its versionName
q_installed() {
    Q_HAVE=""; Q_HAVE_NAME=""
    q_adb 20 -s "$Q_SER" shell pm list packages "$Q_PKG" || return 1
    grep -q "^package:$Q_PKG\$" <(tr -d '\r' < "$Q_DIR/adb.out") || return 0
    q_adb 20 -s "$Q_SER" shell dumpsys package "$Q_PKG" || return 1
    Q_HAVE="$(tr -d '\r' < "$Q_DIR/adb.out" | sed -n 's/.*versionCode=\([0-9][0-9]*\).*/\1/p' | head -1)"
    Q_HAVE_NAME="$(tr -d '\r' < "$Q_DIR/adb.out" | sed -n 's/.*versionName=\([^ ]*\).*/\1/p' | head -1)"
    return 0
}

# q_apk: Q_APK = a checked copy of the VR app (cached, a local test file, or downloaded now)
q_apk() {
    local want="${UA_QUEST_APK_SHA256:-$Q_APK_SHA}" got
    mkdir -p "$Q_DIR"
    if [ -n "${UA_QUEST_APK:-}" ]; then
        Q_APK="$UA_QUEST_APK"
        got="$(/usr/bin/shasum -a 256 "$Q_APK" 2>/dev/null | cut -d' ' -f1)"
        [ "$got" = "$want" ] || { Q_WHY="the local VR app file is not the expected one"; return 1; }
        echo "quest: local APK $Q_APK (checksum ok)"; return 0
    fi
    Q_APK="$Q_DIR/$Q_APK_NAME"
    if [ -f "$Q_APK" ] && [ "$(cat "$Q_APK.ok" 2>/dev/null)" = "$want" ]; then echo "quest: cached APK ok"; return 0; fi
    case "$Q_APK_URL" in ''|*APK-URL-NOT-SET*) Q_WHY="this version of UA Reforged has no download address for the VR app yet"; return 1 ;; esac
    DL_DIR="$Q_DIR/download"; mkdir -p "$DL_DIR"
    dl_ui_start "UA Reforged VR (for Meta Quest)"
    dl_precheck "$Q_APK_URL" 0 0 "$Q_APK_BYTES" || { Q_WHY="the VR app download: $PC_WHY"; rm -rf "$DL_DIR"; return 1; }
    local f="$DL_DIR/$Q_APK_NAME" rc
    DL_WHAT="Downloading UA Reforged VR"; DL_BASE=0; DL_TOTAL="$Q_APK_BYTES"; DL_MAXSIZE="$Q_APK_BYTES"
    dl_fetch "$Q_APK_URL" "$f"; rc=$?; DL_MAXSIZE=""
    [ $rc -eq 130 ] && { dl_ui_end; rm -rf "$DL_DIR"; Q_WHY="cancelled"; return 130; }
    [ $rc -eq 0 ] || { Q_WHY="the VR app download: $DL_WHY"; rm -rf "$DL_DIR"; return 1; }
    got="$(/usr/bin/shasum -a 256 "$f" | cut -d' ' -f1)"
    [ "$got" = "$want" ] || { echo "quest: APK sha256 $got"; Q_WHY="the downloaded VR app is damaged or not the expected file"; rm -rf "$DL_DIR"; return 1; }
    rm -f "$Q_APK" "$Q_APK.ok"; mv "$f" "$Q_APK" && printf '%s\n' "$got" > "$Q_APK.ok"
    rm -rf "$DL_DIR"; dl_ui_end
    return 0
}

# q_install: adb install -r (an update keeps the headset's game files and settings: same package, same key)
q_install() {
    DL_DIR="$Q_DIR/work"; mkdir -p "$DL_DIR"
    dl_ui_start "Installing UA Reforged VR on your headset"
    dl_status 5 "Installing on the headset..."
    local secs; secs=$(( Q_APK_BYTES / 2000000 + 8 )); Q_T0=$SECONDS; Q_SECS=$secs
    DL_PROGRESS=q_install_progress
    "$ADB" -s "$Q_SER" install -r "$Q_APK" >"$Q_DIR/install.out" 2>&1 </dev/null &
    dl_wait $!; local rc=$?; DL_PROGRESS=""
    tr -d '\r' < "$Q_DIR/install.out" | sed 's/^/quest: adb: /'
    [ $rc -eq 130 ] && { dl_ui_end; Q_WHY="cancelled"; return 130; }
    if grep -q '^Success' "$Q_DIR/install.out"; then dl_status 100 done; sleep 1; dl_ui_end; return 0; fi
    dl_ui_end
    local f; f="$(grep -o 'INSTALL_[A-Z_]*' "$Q_DIR/install.out" | head -1)"
    case "$f" in
        INSTALL_FAILED_UPDATE_INCOMPATIBLE|INSTALL_PARSE_FAILED_INCONSISTENT_CERTIFICATES)
            Q_WHY="the headset has a copy of UA Reforged VR signed by someone else. Uninstall it in the headset first (App Library > Unknown Sources > UA Reforged VR > Uninstall), then try again" ;;
        INSTALL_FAILED_INSUFFICIENT_STORAGE) Q_WHY="the headset is out of storage space" ;;
        INSTALL_FAILED_VERSION_DOWNGRADE) Q_WHY="the headset already has a newer UA Reforged VR" ;;
        INSTALL_FAILED_USER_RESTRICTED|INSTALL_FAILED_VERIFICATION_FAILURE) Q_WHY="the headset refused the install. Put it on and accept any prompt, then try again" ;;
        "") if grep -qi 'device .* not found\|no devices\|device offline\|closed' "$Q_DIR/install.out"; then Q_WHY="the headset was disconnected during the install"
            else Q_WHY="the install did not finish ($(tr -d '\r' < "$Q_DIR/install.out" | grep -v '^Performing' | tail -1 | cut -c1-120))"; fi ;;
        *) Q_WHY="the headset refused the app ($f)" ;;
    esac
    return 1
}
q_install_progress() {
    local e=$(( SECONDS - Q_T0 )) p
    p=$(( 5 + e * 90 / Q_SECS )); [ $p -gt 95 ] && p=95
    dl_status "$p" "Installing on the headset... ($e s)"
}

# ---------------------------------------------------------------- the game files (optional)
# The VR app reads the game from its own folder ($Q_REMOTE); when DATA and LEVELS are there on its first start it
# skips "Get the game". Copied: the player's game folder minus this Mac's own folders and Windows programs.
q_remote_has_game() {
    q_adb 20 -s "$Q_SER" shell "ls '$Q_REMOTE' 2>/dev/null" || return 1
    tr -d '\r' < "$Q_DIR/adb.out" | grep -qix 'data' && tr -d '\r' < "$Q_DIR/adb.out" | grep -qix 'levels'
}
q_copy_list() { # the top-level entries of the game folder worth copying
    local e n
    for e in "$GAME"/*; do
        [ -e "$e" ] || continue
        n="$(basename "$e")"
        case "$n" in res|.setup|hd-models|hd-textures|hd-fmv|reforge-pack*|*.exe|*.EXE|*.dll|*.DLL|*.lnk) continue ;; esac
        printf '%s\n' "$e"
    done
}
q_copy_game() {
    local list kb=0 e free
    list="$(q_copy_list)"
    while IFS= read -r e; do [ -n "$e" ] && kb=$(( kb + $(du -skL "$e" 2>/dev/null | cut -f1) )); done <<< "$list"
    q_adb 20 -s "$Q_SER" shell "df -k /sdcard | tail -1"
    free="$(tr -d '\r' < "$Q_DIR/adb.out" | awk '{print $4}')"
    case "$free" in ''|*[!0-9]*) free=0 ;; esac
    [ "$free" -gt 0 ] && [ "$free" -lt $(( kb + 3000000 )) ] && { Q_WHY="the headset does not have enough free space for the game files (they need $(human $((kb * 1024))) plus room for the pack)"; return 1; }
    DL_DIR="$Q_DIR/work"; mkdir -p "$DL_DIR"
    dl_ui_start "Copying your game files to the headset"
    q_adb 20 -s "$Q_SER" shell "mkdir -p '$Q_REMOTE'" || { Q_WHY="the game folder on the headset could not be created"; return 1; }
    Q_KB=$kb; Q_LAST=0; DL_PROGRESS=q_copy_progress
    local rc=0
    while IFS= read -r e; do
        [ -n "$e" ] || continue
        echo "quest: copying $(basename "$e")"
        "$ADB" -s "$Q_SER" push "$e" "$Q_REMOTE/" >"$Q_DIR/push.out" 2>&1 </dev/null &
        dl_wait $!; rc=$?
        [ $rc -eq 0 ] || break
    done <<< "$list"
    DL_PROGRESS=""
    [ $rc -eq 130 ] && { dl_ui_end; Q_WHY="cancelled"; return 130; }
    [ $rc -eq 0 ] || { dl_ui_end; tr -d '\r' < "$Q_DIR/push.out" | tail -2 | sed 's/^/quest: adb: /'; Q_WHY="the copy stopped ($(tr -d '\r' < "$Q_DIR/push.out" | tail -1 | cut -c1-100))"; return 1; }
    q_remote_has_game || { dl_ui_end; Q_WHY="the game files did not arrive on the headset"; return 1; }
    dl_status 100 done; sleep 1; dl_ui_end
    echo "quest: game files copied ($(human $((kb * 1024))))"
    return 0
}
q_copy_progress() {
    [ $(( SECONDS - Q_LAST )) -ge 2 ] || return 0; Q_LAST=$SECONDS
    local have; have="$("$ADB" -s "$Q_SER" shell "du -sk '$Q_REMOTE' 2>/dev/null" </dev/null 2>/dev/null | tr -d '\r' | awk '{print $1}')"
    case "$have" in ''|*[!0-9]*) have=0 ;; esac
    [ "$Q_KB" -gt 0 ] && dl_status "$(( have * 98 / Q_KB > 98 ? 98 : have * 98 / Q_KB ))" "Copying the game files: $(human $((have * 1024))) of $(human $((Q_KB * 1024)))"
}
q_offer_copy() {
    is_game_dir "$GAME" || return 0
    q_remote_has_game && { echo "quest: the headset already has game files"; return 0; }
    local a="${UA_QUEST_COPY:-}"
    if [ -z "$a" ]; then
        local kb=0 e
        while IFS= read -r e; do [ -n "$e" ] && kb=$(( kb + $(du -skL "$e" 2>/dev/null | cut -f1) )); done <<< "$(q_copy_list)"
        case "$(q_ask "Also copy the game files from this Mac to the headset ($(human $((kb * 1024))))?

Then UA Reforged VR does not need to download the game again (about 445 MB). It still offers the Reforge pack on its first start." "Copy the game files" "Not now" "Copy the game files")" in
            "Copy the game files") a=yes ;; *) a=no ;;
        esac
    fi
    [ "$a" = yes ] || { echo "quest: game files not copied"; return 0; }
    q_copy_game; local rc=$?
    case $rc in
        0) Q_COPIED=1 ;;
        130) echo "quest: game copy cancelled" ;;
        *) echo "quest: game copy failed - $Q_WHY"
           [ -z "${UA_NOTICE_AUTO:-}" ] && ask "The game files could not be copied: $Q_WHY.

The VR app is installed. On its first start it offers to download the game instead." "OK" "OK" >/dev/null ;;
    esac
    return 0
}

# ---------------------------------------------------------------- the whole flow
# returns 0 (go on: play on this Mac) or 2 (stop: help page opened or the player quit)
quest_install() {
    mkdir -p "$Q_DIR"; Q_COPIED=""
    q_links
    echo "quest: install flow (VR app $Q_APK_VER, code $Q_APK_CODE)"
    local server_was=0; /usr/bin/nc -z 127.0.0.1 5037 >/dev/null 2>&1 && server_was=1
    local a
    a="$(q_ask "Install UA Reforged VR on your Meta Quest

1. Turn on Developer Mode for the headset (once, in the Meta Horizon phone app).
2. Plug the headset into this Mac with a USB-C cable and turn it on.

UA Reforged downloads the VR app (about $(human "${Q_APK_BYTES:-14000000}")) and, the first time, Google's Android platform tools (adb, about $(human "${Q_ADB_BYTES:-16000000}"), from Google)." "Install" "Open help page" "Cancel" "Install")"
    case "$a" in
        Install) ;;
        "Open help page") open_help "$Q_HELP"; return 2 ;;
        *) echo "quest: cancelled at the start"; return 0 ;;
    esac
    local rc
    q_adb_ready; rc=$?
    [ $rc -eq 130 ] && { echo "quest: cancelled"; return 0; }
    [ $rc -eq 0 ] || { q_broken "$Q_WHY"; return $?; }
    # find the headset; every state has its own plain answer and a "Try again"
    while ! q_find; do
        echo "quest: headset state $Q_STATE"
        local msg b2="Try again" b3="Cancel" help="$Q_HELP"
        case "$Q_STATE" in
            unauthorized) msg="The headset is connected, but it has not allowed this Mac yet.

Put the headset on and allow USB debugging (tick \"Always allow from this computer\"). Then choose Try again." ;;
            nodev) msg="The headset is connected, but Developer Mode is not on.

Turn it on in the Meta Horizon phone app (Devices > your headset > Headset settings > Developer mode), restart the headset, plug it in again and choose Try again. The help page shows the steps."; help="$Q_DEVHELP" ;;
            offline) msg="The headset is connected but not answering. Unplug the cable, plug it in again, put the headset on if it asks anything, and choose Try again." ;;
            many) msg="More than one headset is plugged in. Unplug all but one and choose Try again." ;;
            *) msg="No headset found.

Plug the headset into this Mac with a USB-C cable that carries data (some charging cables don't), turn it on, and choose Try again." ;;
        esac
        a="$(q_ask "$msg" "Try again" "Open help page" "$b3" "$b2")"
        [ -n "${UA_QUEST_AUTO:-}" ] && a="Cancel"   # a test run asks once
        case "$a" in
            "Try again") continue ;;
            "Open help page") open_help "$help"; q_stop_server $server_was; return 2 ;;
            *) echo "quest: stopped (headset $Q_STATE)"; q_stop_server $server_was; return 0 ;;
        esac
    done
    echo "quest: headset $Q_SER (${Q_MODEL:-Quest}) ready"
    q_installed || { q_broken "the headset stopped answering"; rc=$?; q_stop_server $server_was; return $rc; }
    local verb=Install
    if [ -n "$Q_HAVE" ]; then
        echo "quest: installed versionCode $Q_HAVE ($Q_HAVE_NAME), this one $Q_APK_CODE"
        if [ "$Q_HAVE" -ge "${Q_APK_CODE:-0}" ]; then
            echo "quest: already up to date"
            printf '%s\t%s\n' "$Q_SER" "$Q_HAVE" >> "$Q_DONE"
            q_offer_copy
            a="$(q_ask "UA Reforged VR ${Q_HAVE_NAME:-} is already on your headset; it is up to date.

In the headset: App Library > Unknown Sources > UA Reforged VR" "Play on this Mac" "Quit" "Play on this Mac")"
            q_stop_server $server_was
            [ "$a" = Quit ] && return 2; return 0
        fi
        a="$(q_ask "An update for UA Reforged VR is available: ${Q_HAVE_NAME:-$Q_HAVE} is on your headset, this is $Q_APK_VER.

Update it now? Your game files, the Reforge pack and your saves on the headset stay." "Update" "Not now" "Update")"
        [ "$a" = Update ] || { echo "quest: update not now"; q_stop_server $server_was; return 0; }
        verb=Update
    fi
    q_apk; rc=$?
    [ $rc -eq 130 ] && { echo "quest: cancelled"; q_stop_server $server_was; return 0; }
    [ $rc -eq 0 ] || { q_broken "$Q_WHY"; rc=$?; q_stop_server $server_was; return $rc; }
    q_install; rc=$?
    [ $rc -eq 130 ] && { echo "quest: install cancelled"; q_stop_server $server_was; return 0; }
    [ $rc -eq 0 ] || { q_broken "$Q_WHY"; rc=$?; q_stop_server $server_was; return $rc; }
    q_installed; echo "quest: $verb done: versionCode ${Q_HAVE:-?} on $Q_SER"
    printf '%s\t%s\n' "$Q_SER" "${Q_HAVE:-$Q_APK_CODE}" >> "$Q_DONE"
    q_offer_copy
    local done_msg="UA Reforged VR $Q_APK_VER is on your headset."
    [ "$verb" = Update ] && done_msg="UA Reforged VR is updated to $Q_APK_VER."
    [ -n "$Q_COPIED" ] && done_msg="$done_msg Your game files are on it too."
    a="$(q_ask "$done_msg

In the headset: App Library > Unknown Sources > UA Reforged VR" "Play on this Mac" "Quit" "Play on this Mac")"
    q_stop_server $server_was
    [ "$a" = Quit ] && return 2
    return 0
}
q_stop_server() { [ "$1" = 1 ] || "$ADB" kill-server >/dev/null 2>&1 </dev/null; }

# At launch: the Option key, --quest, or a Meta headset on USB that this Mac has not set up with this version.
quest_offer() {
    [ -f "$DL_CONF" ] || return 0
    local mode="${UA_QUEST:-}" stop=""
    [ "$mode" = offer ] && { mode=""; stop=1; }
    if [ -z "$mode" ] && [ "${QUEST_ASKED:-0}" = 1 ]; then
        case "$(q_ask "What would you like to do?" "Install on my Quest" "Install on my Quest" "Play")" in "Install on my Quest") mode=install ;; *) return 0 ;; esac
    fi
    if [ -z "$mode" ]; then
        [ -f "$Q_OFF" ] && { [ -n "$stop" ] && { echo "quest: offer is off (test run)"; exit 0; }; return 0; }
        q_usb_seen || { [ -n "$stop" ] && { echo "quest: no headset on USB (test run)"; exit 0; }; return 0; }
        q_links
        local s seen=0 known=1
        while IFS= read -r s; do
            [ -n "$s" ] || continue; seen=1
            awk -F'\t' -v s="$s" -v c="${Q_APK_CODE:-0}" '$1 == s && $2 >= c { f = 1 } END { exit !f }' "$Q_DONE" 2>/dev/null || known=0
        done <<< "$(q_usb_serials)"
        [ $seen = 1 ] && [ $known = 1 ] && { [ -n "$stop" ] && { echo "quest: known headset (test run)"; exit 0; }; return 0; }
        echo "quest: a Meta headset is plugged in"
        case "$(q_ask "A Meta Quest headset is plugged into this Mac.

Install UA Reforged VR on it? It takes a minute; nothing else on the headset changes." "Install on my Quest" "Don't ask again" "Not now" "Install on my Quest")" in
            "Install on my Quest") mode=install ;;
            "Don't ask again") : > "$Q_OFF"; echo "quest: offer turned off (hold Option at launch to install later)"; [ -n "$stop" ] && exit 0; return 0 ;;
            *) echo "quest: not now"; [ -n "$stop" ] && exit 0; return 0 ;;
        esac
    fi
    case "$mode" in
        install|only)
            quest_install; local rc=$?
            { [ "$mode" = only ] || [ -n "$stop" ]; } && { echo "quest: done (test run)"; exit 0; }
            [ $rc -eq 2 ] && { echo "stopped after the Quest install"; exit 0; } ;;
    esac
    return 0
}

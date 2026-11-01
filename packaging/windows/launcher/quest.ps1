# UA Reforged - "Install on my Quest" (dot-sourced by UA-Reforged.ps1 after downloads.ps1, Windows PowerShell 5.1).
# The Windows twin of packaging/macos/quest.sh: same addresses, same checks, same messages.
#
# Installs UA Reforged VR (the Meta Quest app) on a headset plugged into this PC with a USB-C cable, and can copy
# this PC's game files over too, so the headset does not download the game again. Nothing else on the headset is
# touched: no other app is removed, no setting is changed.
#
# How it is offered: when a Meta Quest is plugged in at launch (seen on USB, no tools needed) and it does not have
# this version yet; the Start menu shortcut "Install UA Reforged VR on my Quest" (-UaQuest); or hold Shift while
# starting UA Reforged.
#
# adb: Google's Android SDK Platform-Tools. Google's licence for them does not allow passing them on, so they are
# not in the installer: on first use the app downloads Google's own zip from dl.google.com (a pinned release,
# checked by size and SHA-256, adb_win in downloads.txt) into %LOCALAPPDATA%\UA Reforged\platform-tools.
# The VR app (quest_apk_url, checked by size and SHA-256 against quest_apk) is cached in ...\quest.
# Both can be changed by the site's links.json, under the same rules as every other link.
#
# Testing only: UA_QUEST=install runs the flow at launch, UA_QUEST=only runs it and quits, UA_QUEST=offer makes the
# usual launch offer and quits after it; UA_QUEST_APK=<file> uses
# a local APK instead of the download (it must still match quest_apk, unless UA_QUEST_APK_SHA256 gives another
# checksum); UA_QUEST_APK_URL=<url> another address; UA_QUEST_ADB=<adb.exe> another adb (no download);
# UA_QUEST_AUTO=yes|no answers the questions (UA_NOTICE_AUTO the notices); UA_QUEST_COPY=yes|no answers the game
# files offer; UA_QUEST_USB=0|1 overrides the USB check (UA_QUEST_USB_SERIAL the serial it reports).

$script:Q_PKG = Conf1 'quest_package'; if (-not $script:Q_PKG) { $script:Q_PKG = 'io.github.ameliaaura.uareforged.vr' }
$script:Q_DIR = Join-Path $SUPPORT 'quest'
$script:Q_PT = Join-Path $SUPPORT 'platform-tools'
$script:Q_OFF = Join-Path $SUPPORT 'quest-offer-off'
$script:Q_DONE = Join-Path $script:Q_DIR 'installed.txt'   # serial<TAB>versionCode of headsets this PC installed
$script:Q_REMOTE = "/sdcard/Android/data/$($script:Q_PKG)/files/ua"
$script:L_QUEST_APK_URL = ''; $script:L_QUEST_APK = ''; $script:L_QUEST_HELP = ''; $script:L_ADB_WIN = ''
$script:ADB = ''; $script:Q_WHY = ''; $script:Q_SER = ''; $script:Q_STATE = ''; $script:Q_MODEL = ''

function Q-Ask([string]$text, [string]$default, [string[]]$buttons) {
    if ($env:UA_QUEST_AUTO) {
        Log ('quest: asked "{0}"' -f (($text -replace '\s+', ' ').Trim()))
        $b = $default
        if ($env:UA_QUEST_AUTO -ne 'yes') { foreach ($x in $buttons) { if ($x -ne $default) { $b = $x; break } } }
        Log "quest: answered [$b] (test run)"
        return $b
    }
    return (Ask $text $default $buttons)
}

# a Meta headset on USB (vendor 0x2833), with or without developer mode
function Q-UsbDevices {
    try { return @(Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction Stop | Where-Object { $_.DeviceID -like 'USB\VID_2833*' }) } catch { return @() }
}
function Q-UsbSeen {
    if ($env:UA_QUEST_USB) { return ($env:UA_QUEST_USB -eq '1') }
    return (@(Q-UsbDevices).Count -gt 0)
}
function Q-UsbSerials {
    if ($env:UA_QUEST_USB_SERIAL) { return @($env:UA_QUEST_USB_SERIAL) }
    foreach ($d in @(Q-UsbDevices)) { $p = $d.DeviceID.Split('\'); if ($p.Count -eq 3 -and $p[2] -notmatch '&') { $p[2] } }
}

# Q-Run <seconds> <args>: runs adb hidden with a time limit (the window keeps answering).
# Sets $script:Q_OUT (stdout + stderr); returns the exit code, 124 when it took too long.
function Q-Run([int]$secs, [string[]]$a) {
    $script:Q_OUT = ''
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $script:ADB
    $psi.Arguments = (($a | ForEach-Object { Quote-Arg ([string]$_) }) -join ' ')
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    try { $p = [System.Diagnostics.Process]::Start($psi) } catch { $script:Q_OUT = "$_"; return 2 }
    $o = $p.StandardOutput.ReadToEndAsync(); $e = $p.StandardError.ReadToEndAsync()
    $t0 = [DateTime]::UtcNow
    while (-not $p.HasExited) {
        if ($script:DL_CANCEL) { try { $p.Kill() } catch { }; return 130 }
        if (([DateTime]::UtcNow - $t0).TotalSeconds -gt $secs) { try { $p.Kill() } catch { }; return 124 }
        if ($script:DL_PROGRESS) { & $script:DL_PROGRESS }
        Pump; Start-Sleep -Milliseconds 200
    }
    $p.WaitForExit()
    # a starting adb server can keep the pipes open: take what arrived
    try { if ($o.Wait(3000)) { $script:Q_OUT += $o.Result } } catch { }
    try { if ($e.Wait(1000)) { $script:Q_OUT += $e.Result } } catch { }
    $script:Q_OUT = $script:Q_OUT.Replace("`r", '')
    return $p.ExitCode
}

function Q-Links {
    if (-not $script:LINKS_DONE) { Links-Load }
    $script:Q_APK_URL = if ($script:L_QUEST_APK_URL) { $script:L_QUEST_APK_URL } else { Conf1 'quest_apk_url' }
    if ($env:UA_QUEST_APK_URL) { $script:Q_APK_URL = $env:UA_QUEST_APK_URL }
    $a = if ($script:L_QUEST_APK) { $script:L_QUEST_APK } else { Conf1 'quest_apk' }
    $c = $a.Split("`t")
    $script:Q_APK_NAME = $c[0]; $script:Q_APK_BYTES = [long]0; $script:Q_APK_SHA = ''; $script:Q_APK_CODE = 0; $script:Q_APK_VER = ''
    if ($c.Count -ge 5) { $script:Q_APK_BYTES = [long]$c[1]; $script:Q_APK_SHA = $c[2]; $script:Q_APK_CODE = [int]$c[3]; $script:Q_APK_VER = $c[4] }
    $script:Q_HELP = if ($script:L_QUEST_HELP) { $script:L_QUEST_HELP } else { Conf1 'quest_help_url' }
    if (-not $script:Q_HELP) { $script:Q_HELP = 'https://uareforged.pages.dev/downloads/#quest-computer-h' }
    $script:Q_DEVHELP = Conf1 'quest_devmode_url'
    if (-not $script:Q_DEVHELP) { $script:Q_DEVHELP = 'https://uareforged.pages.dev/downloads/#quest-devmode-h' }
    $a = if ($script:L_ADB_WIN) { $script:L_ADB_WIN } else { Conf1 'adb_win' }
    $c = $a.Split("`t")
    $script:Q_ADB_URL = $c[0]; $script:Q_ADB_BYTES = [long]0; $script:Q_ADB_SHA = ''
    if ($c.Count -ge 3) { $script:Q_ADB_BYTES = [long]$c[1]; $script:Q_ADB_SHA = $c[2] }
}

# Q-Broken <why>: the failsafe notice. returns 2 if the help page was opened (stop), else 1
function Q-Broken([string]$why) {
    Dl-UiEnd
    Log "quest: broken - $why"
    $b = '' + $env:UA_NOTICE_AUTO
    if (-not $b) {
        $b = Ask ("UA Reforged VR could not be installed on your headset. Nothing on the headset was changed.`r`n`r`nWhat went wrong: $why.") 'Open help page' @('Play on this PC', 'Open help page')
    }
    if ($b -eq 'help' -or $b -eq 'Open help page') { Open-Help $script:Q_HELP; return 2 }
    return 1
}

# ---------------------------------------------------------------- adb: Google's own zip, downloaded on first use
function Q-AdbReady {
    if ($env:UA_QUEST_ADB) { $script:ADB = $env:UA_QUEST_ADB; return [int](-not [IO.File]::Exists($script:ADB)) }
    $script:ADB = Join-Path $script:Q_PT 'platform-tools\adb.exe'
    $mark = Join-Path $script:Q_PT '.zip-sha256'
    if ([IO.File]::Exists($script:ADB) -and [IO.File]::Exists($mark) -and ([IO.File]::ReadAllText($mark).Trim() -eq $script:Q_ADB_SHA)) { return 0 }
    if (-not $script:Q_ADB_URL) { $script:Q_WHY = "this build has no download address for Google's platform tools"; return 1 }
    $script:DL_DIR = $script:Q_PT + '.download'
    if (Test-Path -LiteralPath $script:DL_DIR) { Remove-Tree $script:DL_DIR }
    [void][IO.Directory]::CreateDirectory($script:DL_DIR)
    Dl-UiStart "Google's Android platform tools (adb)"
    if (-not (Dl-Precheck $script:Q_ADB_URL 0 0 $script:Q_ADB_BYTES)) { $script:Q_WHY = "Google's platform tools download: $($script:PC_WHY)"; Remove-Tree $script:DL_DIR; return 1 }
    $f = Join-Path $script:DL_DIR 'platform-tools.zip'
    $script:DL_WHAT = "Downloading Google's platform tools"; $script:DL_BASE = [long]0; $script:DL_TOTAL = $script:Q_ADB_BYTES; $script:DL_MAXSIZE = $script:Q_ADB_BYTES
    $rc = Dl-Fetch $script:Q_ADB_URL $f; $script:DL_MAXSIZE = $null
    if ($rc -eq 130) { Dl-UiEnd; Remove-Tree $script:DL_DIR; $script:Q_WHY = 'cancelled'; return 130 }
    if ($rc -ne 0) { $script:Q_WHY = "Google's platform tools download: $($script:DL_WHY)"; Remove-Tree $script:DL_DIR; return 1 }
    Dl-Status 95 "Checking Google's platform tools..."
    $got = Get-Sha256 $f
    if ($got -ne $script:Q_ADB_SHA) { Log "quest: platform tools sha256 $got"; $script:Q_WHY = 'the downloaded platform tools are not the expected file'; Remove-Tree $script:DL_DIR; return 1 }
    $x = Join-Path $script:DL_DIR 'x'
    if ((Expand-Zip $f $x) -ne 0) { $script:Q_WHY = 'the downloaded platform tools do not unpack'; Remove-Tree $script:DL_DIR; return 1 }
    if (-not [IO.File]::Exists((Join-Path $x 'platform-tools\adb.exe'))) { $script:Q_WHY = 'the downloaded platform tools have no adb'; Remove-Tree $script:DL_DIR; return 1 }
    if (Test-Path -LiteralPath $script:Q_PT) { Remove-Tree $script:Q_PT }
    Move-Dir $x $script:Q_PT
    [IO.File]::WriteAllText($mark, $got + "`r`n")
    Remove-Tree $script:DL_DIR; Dl-UiEnd
    Log "quest: platform tools installed in $($script:Q_PT)"
    return [int](-not [IO.File]::Exists($script:ADB))
}

# ---------------------------------------------------------------- the headset
# Q-Find: sets Q_SER, or Q_STATE = none | nodev (on USB, no developer mode or no USB driver) | unauthorized | offline | many
function Q-Find {
    $script:Q_SER = ''; $script:Q_STATE = 'none'; $script:Q_MODEL = ''
    [void](Q-Run 20 @('start-server'))
    $un = 0; $off = 0
    for ($try = 0; $try -lt 4; $try++) {
        [void](Q-Run 10 @('devices', '-l'))
        $n = 0; $un = 0; $off = 0; $ser = ''
        foreach ($line in $script:Q_OUT.Split("`n")) {
            $w = @($line.Trim() -split '\s+')
            if ($w.Count -lt 2 -or $w[0] -eq 'List' -or $w[0] -eq '*') { continue }
            $rest = ' ' + (($w | Select-Object -Skip 2) -join ' ') + ' '
            $quest = $rest -match ' (model:Quest|device:eureka|device:panther|device:hollywood|device:seacliff|device:monterey|product:eureka|product:panther)'
            if (-not $quest -and $w[1] -ne 'unauthorized' -and $w[1] -ne 'offline') { continue }   # phones and other devices are left alone
            switch ($w[1]) {
                'device' { $n++; $ser = $w[0]; if ($rest -match ' model:(\S+)') { $script:Q_MODEL = $Matches[1].Replace('_', ' ') } }
                'unauthorized' { $un++ }
                'offline' { $off++ }
            }
        }
        if ($n -gt 1) { $script:Q_STATE = 'many'; return $false }
        if ($n -eq 1) { $script:Q_SER = $ser; $script:Q_STATE = 'device'; return $true }
        for ($k = 0; $k -lt 6; $k++) { Pump; Start-Sleep -Milliseconds 250 }   # a freshly started adb can need a few seconds
    }
    if ($un -gt 0) { $script:Q_STATE = 'unauthorized' }
    elseif ($off -gt 0) { $script:Q_STATE = 'offline' }
    elseif (Q-UsbSeen) { $script:Q_STATE = 'nodev' }
    return $false
}

# Q-Installed: Q_HAVE = the versionCode on the headset (0 = not installed); $false if the headset stopped answering
function Q-Installed {
    $script:Q_HAVE = 0; $script:Q_HAVE_NAME = ''
    if ((Q-Run 20 @('-s', $script:Q_SER, 'shell', 'pm', 'list', 'packages', $script:Q_PKG)) -ne 0) { return $false }
    if (-not ($script:Q_OUT.Split("`n") -contains "package:$($script:Q_PKG)")) { return $true }
    if ((Q-Run 20 @('-s', $script:Q_SER, 'shell', 'dumpsys', 'package', $script:Q_PKG)) -ne 0) { return $false }
    if ($script:Q_OUT -match 'versionCode=(\d+)') { $script:Q_HAVE = [int]$Matches[1] }
    if ($script:Q_OUT -match 'versionName=(\S+)') { $script:Q_HAVE_NAME = $Matches[1] }
    return $true
}

# Q-Apk: Q_APK = a checked copy of the VR app (cached, a local test file, or downloaded now). 0 ok, 1, 130
function Q-Apk {
    $want = if ($env:UA_QUEST_APK_SHA256) { $env:UA_QUEST_APK_SHA256 } else { $script:Q_APK_SHA }
    [void][IO.Directory]::CreateDirectory($script:Q_DIR)
    if ($env:UA_QUEST_APK) {
        $script:Q_APK = $env:UA_QUEST_APK
        $got = if ([IO.File]::Exists($script:Q_APK)) { Get-Sha256 $script:Q_APK } else { '' }
        if ($got -ne $want) { $script:Q_WHY = 'the local VR app file is not the expected one'; return 1 }
        Log "quest: local APK $($script:Q_APK) (checksum ok)"; return 0
    }
    $script:Q_APK = Join-Path $script:Q_DIR $script:Q_APK_NAME
    $ok = $script:Q_APK + '.ok'
    if ([IO.File]::Exists($script:Q_APK) -and [IO.File]::Exists($ok) -and ([IO.File]::ReadAllText($ok).Trim() -eq $want)) { Log 'quest: cached APK ok'; return 0 }
    if (-not $script:Q_APK_URL -or $script:Q_APK_URL -like '*APK-URL-NOT-SET*') { $script:Q_WHY = 'this version of UA Reforged has no download address for the VR app yet'; return 1 }
    $script:DL_DIR = Join-Path $script:Q_DIR 'download'
    if (Test-Path -LiteralPath $script:DL_DIR) { Remove-Tree $script:DL_DIR }
    [void][IO.Directory]::CreateDirectory($script:DL_DIR)
    Dl-UiStart 'UA Reforged VR (for Meta Quest)'
    if (-not (Dl-Precheck $script:Q_APK_URL 0 0 $script:Q_APK_BYTES)) { $script:Q_WHY = "the VR app download: $($script:PC_WHY)"; Remove-Tree $script:DL_DIR; return 1 }
    $f = Join-Path $script:DL_DIR $script:Q_APK_NAME
    $script:DL_WHAT = 'Downloading UA Reforged VR'; $script:DL_BASE = [long]0; $script:DL_TOTAL = $script:Q_APK_BYTES; $script:DL_MAXSIZE = $script:Q_APK_BYTES
    $rc = Dl-Fetch $script:Q_APK_URL $f; $script:DL_MAXSIZE = $null
    if ($rc -eq 130) { Dl-UiEnd; Remove-Tree $script:DL_DIR; $script:Q_WHY = 'cancelled'; return 130 }
    if ($rc -ne 0) { $script:Q_WHY = "the VR app download: $($script:DL_WHY)"; Remove-Tree $script:DL_DIR; return 1 }
    $got = Get-Sha256 $f
    if ($got -ne $want) { Log "quest: APK sha256 $got"; $script:Q_WHY = 'the downloaded VR app is damaged or not the expected file'; Remove-Tree $script:DL_DIR; return 1 }
    foreach ($x in @($script:Q_APK, $ok)) { if ([IO.File]::Exists($x)) { [IO.File]::Delete($x) } }
    [IO.File]::Move($f, $script:Q_APK); [IO.File]::WriteAllText($ok, $got + "`r`n")
    Remove-Tree $script:DL_DIR; Dl-UiEnd
    return 0
}

# Q-Install: adb install -r (an update keeps the headset's game files and settings: same package, same key)
function Q-Install {
    $script:DL_DIR = Join-Path $script:Q_DIR 'work'; [void][IO.Directory]::CreateDirectory($script:DL_DIR)
    Dl-UiStart 'Installing UA Reforged VR on your headset'
    Dl-Status 5 'Installing on the headset...'
    $script:Q_T0 = [DateTime]::UtcNow; $script:Q_SECS = [int]($script:Q_APK_BYTES / 2000000) + 8
    $script:DL_PROGRESS = {
        $e = [int]([DateTime]::UtcNow - $script:Q_T0).TotalSeconds
        Dl-Status ([math]::Min(95, 5 + $e * 90 / $script:Q_SECS)) "Installing on the headset... ($e s)"
    }
    $rc = Q-Run 600 @('-s', $script:Q_SER, 'install', '-r', $script:Q_APK)
    $script:DL_PROGRESS = $null
    foreach ($l in $script:Q_OUT.Split("`n")) { if ($l) { Log "quest: adb: $l" } }
    if ($rc -eq 130) { Dl-UiEnd; $script:Q_WHY = 'cancelled'; return 130 }
    if ($script:Q_OUT -match '(?m)^Success') { Dl-Status 100 'done'; Dl-UiEnd; return 0 }
    Dl-UiEnd
    $f = if ($script:Q_OUT -match '(INSTALL_[A-Z_]+)') { $Matches[1] } else { '' }
    switch -regex ($f) {
        '^(INSTALL_FAILED_UPDATE_INCOMPATIBLE|INSTALL_PARSE_FAILED_INCONSISTENT_CERTIFICATES)$' {
            $script:Q_WHY = 'the headset has a copy of UA Reforged VR signed by someone else. Uninstall it in the headset first (App Library > Unknown Sources > UA Reforged VR > Uninstall), then try again'; break }
        '^INSTALL_FAILED_INSUFFICIENT_STORAGE$' { $script:Q_WHY = 'the headset is out of storage space'; break }
        '^INSTALL_FAILED_VERSION_DOWNGRADE$' { $script:Q_WHY = 'the headset already has a newer UA Reforged VR'; break }
        '^(INSTALL_FAILED_USER_RESTRICTED|INSTALL_FAILED_VERIFICATION_FAILURE)$' { $script:Q_WHY = 'the headset refused the install. Put it on and accept any prompt, then try again'; break }
        '^$' {
            if ($rc -eq 124) { $script:Q_WHY = 'the install did not finish in time' }
            elseif ($script:Q_OUT -match 'not found|no devices|offline|closed') { $script:Q_WHY = 'the headset was disconnected during the install' }
            else { $last = @($script:Q_OUT.Split("`n") | Where-Object { $_ -and $_ -notlike 'Performing*' }); $t = if ($last.Count) { $last[-1] } else { "adb $rc" }; $script:Q_WHY = "the install did not finish ($($t.Substring(0, [math]::Min(120, $t.Length))))" }
            break }
        default { $script:Q_WHY = "the headset refused the app ($f)" }
    }
    return 1
}

# ---------------------------------------------------------------- the game files (optional)
# The VR app reads the game from its own folder (Q_REMOTE); when DATA and LEVELS are there on its first start it skips
# "Get the game". Copied: the player's game folder minus this PC's own folders and Windows programs.
function Q-RemoteHasGame {
    if ((Q-Run 20 @('-s', $script:Q_SER, 'shell', "ls '$($script:Q_REMOTE)' 2>/dev/null")) -ne 0) { return $false }
    $names = @($script:Q_OUT.Split("`n") | ForEach-Object { $_.Trim().ToLowerInvariant() })
    return (($names -contains 'data') -and ($names -contains 'levels'))
}
function Q-CopyList {
    foreach ($e in [IO.Directory]::GetFileSystemEntries($script:GAME)) {
        $n = [IO.Path]::GetFileName($e)
        if ($n.StartsWith('.') -or @('res', 'hd-models', 'hd-textures', 'hd-fmv') -contains $n -or $n -like 'reforge-pack*') { continue }
        $x = [IO.Path]::GetExtension($n).ToLowerInvariant()
        if (@('.exe', '.dll', '.lnk') -contains $x) { continue }
        $e
    }
}
function Q-Size([string]$p) {
    if ([IO.File]::Exists($p)) { return (New-Object IO.FileInfo($p)).Length }
    $t = [long]0
    try { foreach ($f in [IO.Directory]::EnumerateFiles($p, '*', 'AllDirectories')) { $t += (New-Object IO.FileInfo($f)).Length } } catch { }
    return $t
}
function Q-CopyGame {
    $list = @(Q-CopyList); $bytes = [long]0
    foreach ($e in $list) { $bytes += Q-Size $e }
    [void](Q-Run 20 @('-s', $script:Q_SER, 'shell', 'df -k /sdcard | tail -1'))
    $w = @($script:Q_OUT.Trim() -split '\s+'); $free = [long]0
    if ($w.Count -ge 4) { [void][long]::TryParse($w[3], [ref]$free) }
    if ($free -gt 0 -and $free * 1024 -lt $bytes + 3000000000) { $script:Q_WHY = "the headset does not have enough free space for the game files (they need $(Human $bytes) plus room for the pack)"; return 1 }
    $script:DL_DIR = Join-Path $script:Q_DIR 'work'; [void][IO.Directory]::CreateDirectory($script:DL_DIR)
    Dl-UiStart 'Copying your game files to the headset'
    if ((Q-Run 20 @('-s', $script:Q_SER, 'shell', "mkdir -p '$($script:Q_REMOTE)'")) -ne 0) { Dl-UiEnd; $script:Q_WHY = 'the game folder on the headset could not be created'; return 1 }
    $script:Q_BYTES = $bytes; $script:Q_LAST = [DateTime]::MinValue
    $script:DL_PROGRESS = {
        if (([DateTime]::UtcNow - $script:Q_LAST).TotalSeconds -lt 2) { return }
        $script:Q_LAST = [DateTime]::UtcNow
        $save = $script:Q_OUT; $prog = $script:DL_PROGRESS; $script:DL_PROGRESS = $null
        $have = [long]0
        if ((Q-Run 10 @('-s', $script:Q_SER, 'shell', "du -sk '$($script:Q_REMOTE)'")) -eq 0) { [void][long]::TryParse((@($script:Q_OUT.Trim() -split '\s+'))[0], [ref]$have) }
        $script:Q_OUT = $save; $script:DL_PROGRESS = $prog
        if ($script:Q_BYTES -gt 0) { Dl-Status ([math]::Min(98, $have * 1024 * 98 / $script:Q_BYTES)) ("Copying the game files: {0} of {1}" -f (Human ($have * 1024)), (Human $script:Q_BYTES)) }
    }
    $rc = 0
    foreach ($e in $list) {
        Log "quest: copying $([IO.Path]::GetFileName($e))"
        $rc = Q-Run 3600 @('-s', $script:Q_SER, 'push', $e, "$($script:Q_REMOTE)/")
        if ($rc -ne 0) { break }
    }
    $script:DL_PROGRESS = $null
    if ($rc -eq 130) { Dl-UiEnd; $script:Q_WHY = 'cancelled'; return 130 }
    if ($rc -ne 0) {
        Dl-UiEnd
        $last = @($script:Q_OUT.Split("`n") | Where-Object { $_ }); $t = if ($last.Count) { $last[-1] } else { "adb $rc" }
        $script:Q_WHY = "the copy stopped ($($t.Substring(0, [math]::Min(100, $t.Length))))"; return 1
    }
    if (-not (Q-RemoteHasGame)) { Dl-UiEnd; $script:Q_WHY = 'the game files did not arrive on the headset'; return 1 }
    Dl-Status 100 'done'; Dl-UiEnd
    Log "quest: game files copied ($(Human $bytes))"
    return 0
}
function Q-OfferCopy {
    if (-not $script:GAME -or -not (Is-GameDir $script:GAME)) { return }
    if (Q-RemoteHasGame) { Log 'quest: the headset already has game files'; return }
    $a = '' + $env:UA_QUEST_COPY
    if (-not $a) {
        $bytes = [long]0; foreach ($e in @(Q-CopyList)) { $bytes += Q-Size $e }
        $r = Q-Ask ("Also copy the game files from this PC to the headset ($(Human $bytes))?`r`n`r`nThen UA Reforged VR does not need to download the game again (about 445 MB). It still offers the Reforge pack on its first start.") 'Copy the game files' @('Not now', 'Copy the game files')
        $a = if ($r -eq 'Copy the game files') { 'yes' } else { 'no' }
    }
    if ($a -ne 'yes') { Log 'quest: game files not copied'; return }
    $rc = Q-CopyGame
    if ($rc -eq 0) { $script:Q_COPIED = $true }
    elseif ($rc -eq 130) { Log 'quest: game copy cancelled' }
    else {
        Log "quest: game copy failed - $($script:Q_WHY)"
        if (-not $env:UA_NOTICE_AUTO) { [void](Ask ("The game files could not be copied: $($script:Q_WHY).`r`n`r`nThe VR app is installed. On its first start it offers to download the game instead.") 'OK' @('OK')) }
    }
}

function Q-StopServer([bool]$was) { if (-not $was -and $script:ADB) { [void](Q-Run 10 @('kill-server')) } }
function Q-Remember([int]$code) {
    try { [IO.File]::AppendAllText($script:Q_DONE, "$($script:Q_SER)`t$code`r`n") } catch { }
}

# ---------------------------------------------------------------- the whole flow
# returns 0 (go on: play on this PC) or 2 (stop: help page opened or the player quit)
function Quest-Install {
    [void][IO.Directory]::CreateDirectory($script:Q_DIR); $script:Q_COPIED = $false
    Q-Links
    Log "quest: install flow (VR app $($script:Q_APK_VER), code $($script:Q_APK_CODE))"
    $was = $false
    try { $c = New-Object Net.Sockets.TcpClient; $c.Connect('127.0.0.1', 5037); $was = $c.Connected; $c.Close() } catch { }
    $adbMb = if ($script:Q_ADB_BYTES) { Human $script:Q_ADB_BYTES } else { '8 MB' }
    $a = Q-Ask ("Install UA Reforged VR on your Meta Quest`r`n`r`n1. Turn on Developer Mode for the headset (once, in the Meta Horizon phone app).`r`n2. Plug the headset into this PC with a USB-C cable and turn it on.`r`n`r`nUA Reforged downloads the VR app (about $(Human $script:Q_APK_BYTES)) and, the first time, Google's Android platform tools (adb, about $adbMb, from Google).") 'Install' @('Cancel', 'Open help page', 'Install')
    if ($a -eq 'Open help page') { Open-Help $script:Q_HELP; return 2 }
    if ($a -ne 'Install') { Log 'quest: cancelled at the start'; return 0 }
    $rc = Q-AdbReady
    if ($rc -eq 130) { Log 'quest: cancelled'; return 0 }
    if ($rc -ne 0) { return (Q-Broken $script:Q_WHY) }
    while (-not (Q-Find)) {
        Log "quest: headset state $($script:Q_STATE)"
        $help = $script:Q_HELP
        switch ($script:Q_STATE) {
            'unauthorized' { $msg = "The headset is connected, but it has not allowed this PC yet.`r`n`r`nPut the headset on and allow USB debugging (tick ""Always allow from this computer""). Then choose Try again." }
            'nodev' { $msg = "The headset is connected, but Developer Mode is not on, or Windows is missing the headset's USB driver.`r`n`r`nTurn Developer Mode on in the Meta Horizon phone app (Devices > your headset > Headset settings > Developer mode) and restart the headset. If it is on already, install Meta's ""Oculus ADB Drivers"" (linked on the help page). Then plug the headset in again and choose Try again."; $help = $script:Q_DEVHELP }
            'offline' { $msg = 'The headset is connected but not answering. Unplug the cable, plug it in again, put the headset on if it asks anything, and choose Try again.' }
            'many' { $msg = 'More than one headset is plugged in. Unplug all but one and choose Try again.' }
            default { $msg = "No headset found.`r`n`r`nPlug the headset into this PC with a USB-C cable that carries data (some charging cables don't), turn it on, and choose Try again." }
        }
        $a = Q-Ask $msg 'Try again' @('Cancel', 'Open help page', 'Try again')
        if ($env:UA_QUEST_AUTO) { $a = 'Cancel' }   # a test run asks once
        if ($a -eq 'Try again') { continue }
        Q-StopServer $was
        if ($a -eq 'Open help page') { Open-Help $help; return 2 }
        Log "quest: stopped (headset $($script:Q_STATE))"; return 0
    }
    Log "quest: headset $($script:Q_SER) ($(if ($script:Q_MODEL) { $script:Q_MODEL } else { 'Quest' })) ready"
    if (-not (Q-Installed)) { $r = Q-Broken 'the headset stopped answering'; Q-StopServer $was; return $r }
    $verb = 'Install'
    if ($script:Q_HAVE -gt 0) {
        Log "quest: installed versionCode $($script:Q_HAVE) ($($script:Q_HAVE_NAME)), this one $($script:Q_APK_CODE)"
        if ($script:Q_HAVE -ge $script:Q_APK_CODE) {
            Log 'quest: already up to date'
            Q-Remember $script:Q_HAVE
            Q-OfferCopy
            $a = Q-Ask ("UA Reforged VR $($script:Q_HAVE_NAME) is already on your headset; it is up to date.`r`n`r`nIn the headset: App Library > Unknown Sources > UA Reforged VR") 'Play on this PC' @('Quit', 'Play on this PC')
            Q-StopServer $was
            if ($a -eq 'Quit') { return 2 }; return 0
        }
        $have = if ($script:Q_HAVE_NAME) { $script:Q_HAVE_NAME } else { $script:Q_HAVE }
        $a = Q-Ask ("An update for UA Reforged VR is available: $have is on your headset, this is $($script:Q_APK_VER).`r`n`r`nUpdate it now? Your game files, the Reforge pack and your saves on the headset stay.") 'Update' @('Not now', 'Update')
        if ($a -ne 'Update') { Log 'quest: update not now'; Q-StopServer $was; return 0 }
        $verb = 'Update'
    }
    $rc = Q-Apk
    if ($rc -eq 130) { Log 'quest: cancelled'; Q-StopServer $was; return 0 }
    if ($rc -ne 0) { $r = Q-Broken $script:Q_WHY; Q-StopServer $was; return $r }
    $rc = Q-Install
    if ($rc -eq 130) { Log 'quest: install cancelled'; Q-StopServer $was; return 0 }
    if ($rc -ne 0) { $r = Q-Broken $script:Q_WHY; Q-StopServer $was; return $r }
    [void](Q-Installed)
    Log "quest: $verb done: versionCode $($script:Q_HAVE) on $($script:Q_SER)"
    Q-Remember ([math]::Max($script:Q_HAVE, $script:Q_APK_CODE))
    Q-OfferCopy
    $msg = if ($verb -eq 'Update') { "UA Reforged VR is updated to $($script:Q_APK_VER)." } else { "UA Reforged VR $($script:Q_APK_VER) is on your headset." }
    if ($script:Q_COPIED) { $msg += ' Your game files are on it too.' }
    $a = Q-Ask ("$msg`r`n`r`nIn the headset: App Library > Unknown Sources > UA Reforged VR") 'Play on this PC' @('Quit', 'Play on this PC')
    Q-StopServer $was
    if ($a -eq 'Quit') { return 2 }
    return 0
}

# At launch: Shift held (or the Start menu shortcut), or a Meta headset on USB that this PC has not set up with this version.
# $only: the Start menu shortcut (quit afterwards, no game folder needed).
function Quest-Offer([bool]$asked, [bool]$only) {
    if (-not [IO.File]::Exists($script:DL_CONF)) { return }
    $mode = '' + $env:UA_QUEST; $stop = $false
    if ($mode -eq 'offer') { $mode = ''; $stop = $true }
    if ($only) { $mode = 'only' }
    if (-not $mode -and $asked) {
        if ((Q-Ask 'What would you like to do?' 'Install on my Quest' @('Play', 'Install on my Quest')) -eq 'Install on my Quest') { $mode = 'install' } else { return }
    }
    if (-not $mode) {
        if ([IO.File]::Exists($script:Q_OFF)) { if ($stop) { Log 'quest: offer is off (test run)'; exit 0 }; return }
        if (-not (Q-UsbSeen)) { if ($stop) { Log 'quest: no headset on USB (test run)'; exit 0 }; return }
        Q-Links
        $done = @(); if ([IO.File]::Exists($script:Q_DONE)) { $done = @([IO.File]::ReadAllLines($script:Q_DONE)) }
        $serials = @(Q-UsbSerials); $known = $serials.Count -gt 0
        foreach ($s in $serials) {
            $hit = $false
            foreach ($l in $done) { $c = $l.Split("`t"); if ($c.Count -ge 2 -and $c[0] -eq $s -and [int]$c[1] -ge $script:Q_APK_CODE) { $hit = $true } }
            if (-not $hit) { $known = $false }
        }
        if ($known) { if ($stop) { Log 'quest: known headset (test run)'; exit 0 }; return }
        Log 'quest: a Meta headset is plugged in'
        $a = Q-Ask ("A Meta Quest headset is plugged into this PC.`r`n`r`nInstall UA Reforged VR on it? It takes a minute; nothing else on the headset changes.") 'Install on my Quest' @('Not now', "Don't ask again", 'Install on my Quest')
        if ($a -eq "Don't ask again") { [IO.File]::WriteAllText($script:Q_OFF, "off`r`n"); Log 'quest: offer turned off (the Start menu has "Install UA Reforged VR on my Quest")'; if ($stop) { exit 0 }; return }
        if ($a -ne 'Install on my Quest') { Log 'quest: not now'; if ($stop) { exit 0 }; return }
        $mode = 'install'
    }
    $rc = Quest-Install
    if ($mode -eq 'only' -or $stop) { Log 'quest: done'; exit 0 }
    if ($rc -eq 2) { Log 'stopped after the Quest install'; exit 0 }
}

# UA Reforged - Windows launcher (started by "UA Reforged.exe" with Windows PowerShell 5.1, no console window).
#
# The app ships no game data. On first launch the player picks their own Urban Assault folder (the one with DATA
# and LEVELS), or lets the app fetch the community OpenUA Standalone Package. That folder is only read, never
# written.
#
# The engine reads and writes everything relative to its working directory, so the launcher builds one per user:
#
#   %LOCALAPPDATA%\UA Reforged\game\
#
# Writable state (profiles, settings, the engine log ENV\ypa_log.txt) is COPIED there once from the player's folder;
# every read-only data folder is a directory junction back to the player's folder, refreshed on every launch (small
# loose files are hard-linked or copied). The engine's own shaders (res\) come from the install folder, because the
# Reforged renderer needs its versions; fonts\ merges both.
#
# Optional Reforge pack: %LOCALAPPDATA%\UA Reforged\reforge-pack\ (hd-models\, hd-textures\, hd-fmv\, ui\, ground\).
# Without it the game draws the 1998 look. The pack stores files that many levels share once; on first launch (and
# after a pack update) the launcher rebuilds the per-level layout as NTFS hard links (copies where that fails).
# First launch with no game folder: one "Set up UA Reforged" step fetches the community OpenUA Standalone Package (from
# the Metropolis Dawn community's own server) and the Reforge pack in one go, or uses the player's own folder. Every
# later start shows a small launcher: the campaign, Play, Install on my Quest, and Get the Reforge pack while none is
# installed. All of it lives in downloads.ps1; every address is in downloads.txt (+ the site's links.json).
# Dialogs show no file paths: a player's path holds their account name. launcher.log keeps the paths.
#
# Environment (for testing): UA_GAME_DIR=<folder> skips the first-launch setup, UA_CAMPAIGN=base|ghor|taer skips the
# launcher, UA_DATA_DIR=<folder> uses that folder instead of %LOCALAPPDATA%\UA Reforged.
# Arguments go to the engine. "-UaPurge" (used by the uninstaller) removes %LOCALAPPDATA%\UA Reforged safely.
# "-UaQuest" (the Start menu shortcut "Install UA Reforged VR on my Quest") runs only "Install on my Quest" (quest.ps1);
# the first-launch dialog and the launcher have an "Install on my Quest" button, a Meta Quest plugged in at launch is
# offered it by itself, and holding Shift while starting UA Reforged still works.

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$script:HERE = $PSScriptRoot
$script:INST = Split-Path -Parent $PSScriptRoot
$script:ENGINE = Join-Path $INST 'engine\UA-Reforged-engine.exe'
$script:OWN = Join-Path $INST 'engine'
if ($env:UA_DATA_DIR) { $script:SUPPORT = $env:UA_DATA_DIR } else { $script:SUPPORT = Join-Path $env:LOCALAPPDATA 'UA Reforged' }
$script:RUN = Join-Path $SUPPORT 'game'
$script:PACK = Join-Path $SUPPORT 'reforge-pack'
$script:CONF = Join-Path $SUPPORT 'game-folder.txt'
$script:LOG = Join-Path $SUPPORT 'launcher.log'
$script:LINKED = Join-Path $RUN '.launcher-links.txt'

$WRITABLE = @('ENV', 'SAVE', 'Ghorsave', 'Taersave', 'Ghor_env', 'Taer_env', 'NUCLEUS.INI', 'joyconf.txt')
$OWNED = @('res', 'fonts', 'hd-models', 'hd-textures', 'hd-fmv')   # built by the launcher, never linked straight to the player's folder

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()
$script:ICON = $null
try { $script:ICON = [System.Drawing.Icon]::ExtractAssociatedIcon((Join-Path $INST 'UA Reforged.exe')) } catch { }

# shared helpers (Log, Alert, junction-safe Remove-Tree, hard links) and the downloads
. (Join-Path $HERE 'downloads.ps1')
# "Install on my Quest": UA Reforged VR onto a Meta Quest over USB
. (Join-Path $HERE 'quest.ps1')

# ---------------------------------------------------------------- uninstall: remove our data folder
if ($args.Count -ge 1 -and $args[0] -eq '-UaPurge') {
    if (Test-Path -LiteralPath $SUPPORT) { Remove-Tree $SUPPORT }
    exit 0
}

try { [void][IO.Directory]::CreateDirectory($RUN) } catch { Log "could not create ${RUN}: $_"; Fail "UA Reforged could not create its data folder. Check that your disk is not full." }
Log ("=== {0} launch from {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $INST)
if (-not [IO.File]::Exists($ENGINE)) { Fail "UA Reforged is incomplete (the engine is missing). Please install it again." }


# Shift held while starting, or the Start menu shortcut (-UaQuest): "Install on my Quest" this time
$questOnly = ($args -contains '-UaQuest')
$questAsked = (([Windows.Forms.Control]::ModifierKeys -band [Windows.Forms.Keys]::Shift) -ne 0)

$script:GAME = ''
if ($env:UA_GAME_DIR) { $script:GAME = $env:UA_GAME_DIR }
elseif ([IO.File]::Exists($CONF)) { $script:GAME = ([IO.File]::ReadAllText($CONF)).Trim() }

try {
    # the Start menu shortcut: only "Install on my Quest" (the game folder, if any, is offered for copying)
    if ($questOnly) { Quest-Offer $false $true }
    # no game folder yet: "Set up UA Reforged" (the game + the Reforge pack in one step, or your own folder)
    Setup-First
    $GAME = [IO.Path]::GetFullPath($script:GAME).TrimEnd('\')
    if (-not $env:UA_GAME_DIR) { [IO.File]::WriteAllText($CONF, $GAME + "`r`n") }
    Log "game folder: $GAME"
    # a Meta Quest plugged in (or Shift held): offer to install UA Reforged VR on it
    Quest-Offer $questAsked $false
    # the launcher: campaign, Play, Install on my Quest, Get the Reforge pack (UA_CAMPAIGN or -env skips it)
    $campaign = '' + $env:UA_CAMPAIGN
    if (-not $campaign -and -not ($args -contains '-env')) { $campaign = Play-Menu }

    # what the last launch linked: removed and made again, so a changed game folder or pack is picked up
    if ([IO.File]::Exists($LINKED)) {
        foreach ($n in [IO.File]::ReadAllLines($LINKED)) {
            if (-not $n) { continue }
            $p = Join-Path $RUN $n
            try { if ((Test-Path -LiteralPath $p) -or (Test-Reparse $p)) { Remove-Link $p } } catch { Log "could not remove the old link ${n}: $_" }
        }
        [IO.File]::Delete($LINKED)
    }
    $made = New-Object System.Collections.Generic.List[string]

    # writable state: copied once from the player's folder
    foreach ($name in $WRITABLE) {
        $dst = Join-Path $RUN $name; $src = Join-Path $GAME $name
        if (-not (Test-Path -LiteralPath $dst) -and (Test-Path -LiteralPath $src)) {
            try {
                Copy-Item -LiteralPath $src -Destination $dst -Recurse -Force
                Get-ChildItem -LiteralPath $dst -Recurse -Force -ErrorAction SilentlyContinue | Where-Object { -not $_.PSIsContainer } |
                    ForEach-Object { if ($_.IsReadOnly) { $_.IsReadOnly = $false } }
                if ([IO.File]::Exists($dst)) { [IO.File]::SetAttributes($dst, [IO.FileAttributes]::Normal) }
            } catch { Log "copy ${name}: $_"; Fail "Could not copy $name from your game folder." }
        }
    }

    # read-only game data: junctions (folders) and hard links or copies (loose files) into the player's folder;
    # never the writable or owned names, never the community package's own Windows programs
    foreach ($e in [IO.Directory]::GetFileSystemEntries($GAME)) {
        $name = [IO.Path]::GetFileName($e)
        if ($name.StartsWith('.')) { continue }
        if ($WRITABLE -contains $name -or $OWNED -contains $name) { continue }
        $dst = Join-Path $RUN $name
        if ((Test-Path -LiteralPath $dst) -or (Test-Reparse $dst)) { continue }   # something the player put there
        if ([IO.Directory]::Exists($e)) {
            New-Junction $dst $e; $made.Add($name)
        } else {
            $ext = [IO.Path]::GetExtension($name).ToLowerInvariant()
            if ($ext -eq '.exe' -or $ext -eq '.dll') { continue }
            [void](Link-OrCopy $dst $e); $made.Add($name)
        }
    }
    # the engine's own shaders (the Reforged renderer needs its versions)
    $res = Join-Path $RUN 'res'
    if ((Test-Path -LiteralPath $res) -and -not (Test-Reparse $res)) { Remove-Tree $res }
    if (Test-Reparse $res) { Remove-Link $res }
    New-Junction $res (Join-Path $OWN 'res'); $made.Add('res')
    # fonts: the engine's free fonts, plus any extra fonts your game folder brings (same names win)
    $fonts = Join-Path $RUN 'fonts'
    if (Test-Reparse $fonts) { Remove-Link $fonts }
    if ([IO.Directory]::Exists($fonts)) { Remove-Tree $fonts }
    [void][IO.Directory]::CreateDirectory($fonts)
    foreach ($fdir in @((Join-Path $OWN 'fonts'), (Join-Path $GAME 'fonts'))) {
        if (-not [IO.Directory]::Exists($fdir)) { continue }
        foreach ($f in [IO.Directory]::GetFiles($fdir)) {
            $d = Join-Path $fonts ([IO.Path]::GetFileName($f))
            if ([IO.File]::Exists($d)) { [IO.File]::Delete($d) }
            [void](Link-OrCopy $d $f)
        }
    }

    # the optional Reforge pack: offer the one-click download when none is installed
    Pack-Offer
    $lt = Join-Path $PACK 'LINKS.tsv'; $ld = Join-Path $PACK '.links-done'
    if ([IO.File]::Exists($lt) -and (-not [IO.File]::Exists($ld) -or [IO.File]::GetLastWriteTimeUtc($lt) -gt [IO.File]::GetLastWriteTimeUtc($ld))) {
        Log "reforge pack: rebuilding shared files (LINKS.tsv)"
        if (-not (Install-PackLinks $PACK $false)) { Alert "The Reforge pack is incomplete (see launcher.log). The game starts with what is there." }
    }
    $fwd = { param($p) $p.Replace('\', '/') }
    $hdm = Join-Path $RUN 'hd-models'
    if (Test-Reparse $hdm) { Remove-Link $hdm }
    if ([IO.Directory]::Exists((Join-Path $PACK 'hd-models'))) {
        if (-not (Test-Path -LiteralPath $hdm)) { New-Junction $hdm (Join-Path $PACK 'hd-models'); $made.Add('hd-models') }
        $env:UA_HDMODELS_DIR = & $fwd $hdm
        if ([IO.Directory]::Exists((Join-Path $PACK 'hd-models\sky'))) { $env:UA_SKY_DIR = & $fwd (Join-Path $PACK 'hd-models\sky') }
        Log "reforge pack: $PACK"
    } else {
        Log "reforge pack: none (1998 models)"
    }
    # pack folders the engine reads from its working directory: the pack's copy wins over the game folder's
    foreach ($name in @('hd-textures', 'hd-fmv')) {
        $dst = Join-Path $RUN $name
        if (Test-Reparse $dst) { Remove-Link $dst }
        if ((Test-Path -LiteralPath $dst)) { continue }
        if ([IO.Directory]::Exists((Join-Path $PACK $name))) { New-Junction $dst (Join-Path $PACK $name); $made.Add($name) }
        elseif ([IO.Directory]::Exists((Join-Path $GAME $name))) { New-Junction $dst (Join-Path $GAME $name); $made.Add($name) }
    }
    if ([IO.Directory]::Exists((Join-Path $PACK 'ui'))) { $env:UA_UI_DIR = (& $fwd (Join-Path $PACK 'ui')) + '/' }
    if ([IO.File]::Exists((Join-Path $PACK 'ground\pack.txt'))) { $env:UA_R2_GROUND_DIR = & $fwd (Join-Path $PACK 'ground') }
    [IO.File]::WriteAllLines($LINKED, $made.ToArray())
} catch {
    Log ("launcher error: {0} at {1}" -f $_, $_.InvocationInfo.PositionMessage)
    $msg = "UA Reforged could not set up its game folder: $(No-Paths $_.Exception.Message)`r`n`r`nThe details are in its log file."
    Log ('notice: ' + ($msg -replace '\s+', ' '))
    if ((-not $env:UA_NOTICE_AUTO -and -not $env:UA_DIALOG_SNAP) -and (Ask $msg 'OK' @('Show log in Explorer', 'OK')) -eq 'Show log in Explorer') { Show-InExplorer $LOG }
    exit 1
}

$engineArgs = @($args | Where-Object { $_ -ne '-UaQuest' })
if ($campaign -eq 'ghor') { $engineArgs = @('-env', 'Ghor_env') + $engineArgs }
elseif ($campaign -eq 'taer') { $engineArgs = @('-env', 'Taer_env') + $engineArgs }
if (-not $campaign) { $campaign = 'from arguments' }

# no network side features in this build
Remove-Item Env:UA_RADIO, Env:UA_RADIO_LLM -ErrorAction SilentlyContinue

Log "campaign=$campaign"
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $ENGINE
$psi.Arguments = (($engineArgs | ForEach-Object { Quote-Arg ([string]$_) }) -join ' ')
$psi.WorkingDirectory = $RUN
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$p = [System.Diagnostics.Process]::Start($psi)
# the engine's console output goes to engine.log (the Mac app writes it into launcher.log)
# (shared for reading, so the logs can be opened while the game runs)
$elog = New-Object IO.FileStream((Join-Path $SUPPORT 'engine.log'), [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
$eerr = New-Object IO.FileStream((Join-Path $SUPPORT 'engine-err.log'), [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
$t1 = $p.StandardOutput.BaseStream.CopyToAsync($elog)
$t2 = $p.StandardError.BaseStream.CopyToAsync($eerr)
$p.WaitForExit()
try { [void]$t1.Wait(5000); [void]$t2.Wait(5000) } catch { }
$elog.Dispose(); $eerr.Dispose()
$rc = $p.ExitCode
Log "engine exit $rc"
if ($rc -ne 0) {
    $msg = "The game stopped unexpectedly (code $rc). Its log file has the details."
    Log "notice: $msg"
    $ylog = Join-Path $RUN 'ENV\ypa_log.txt'
    if ($env:UA_NOTICE_AUTO -or $env:UA_DIALOG_SNAP) { if ($env:UA_NOTICE_AUTO -eq 'help') { Show-InExplorer $ylog } }
    elseif ((Ask $msg 'OK' @('Show log in Explorer', 'OK')) -eq 'Show log in Explorer') { Show-InExplorer $ylog }
    exit 1
}
exit 0

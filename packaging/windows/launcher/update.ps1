# UA Reforged - finishes a one-click update (Windows PowerShell 5.1, started hidden by Update-Apply in downloads.ps1).
#
# The launcher has already downloaded the installer and checked its signature, manifest, size and SHA-256. This script
# runs from the download folder: it waits for the launcher (and "UA Reforged.exe") to close, runs the installer silently
# into the same program folder (per-user, no administrator prompt), checks that the new version is in place, removes the
# download and starts UA Reforged again. If the installer fails: a short notice, the download page, and UA Reforged
# starts again (the installer only replaces files once it runs, so the current version is normally still there).
# Test runs: UA_UPDATE_AUTO set = no notice and no browser.
param([string]$Setup, [string]$Inst, [string]$Version, [string]$Page, [string]$Support, [string]$WaitPids = '')
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
try { Set-Location -LiteralPath $Support } catch { }   # (the download folder is removed at the end)
$LOG = Join-Path $Support 'launcher.log'
function Log([string]$m) { try { [IO.File]::AppendAllText($LOG, "$m`r`n") } catch { } }
Log ("=== {0} update to {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Version)

# 1. the launcher and its starter close first (the starter holds the "one copy at a time" lock)
$t0 = [DateTime]::UtcNow
foreach ($w in ($WaitPids -split ',')) {
    $id = 0
    if (-not [int]::TryParse($w, [ref]$id) -or $id -le 0) { continue }
    try {
        $p = [Diagnostics.Process]::GetProcessById($id)
        $left = 60000 - [int]([DateTime]::UtcNow - $t0).TotalMilliseconds
        if ($left -gt 0) { [void]$p.WaitForExit($left) }
    } catch { }
}

# a small window while the installer runs (none in test runs)
$ui = $null
if (-not $env:UA_PACK_NOUI) {
    try {
        Add-Type -AssemblyName System.Windows.Forms, System.Drawing
        [System.Windows.Forms.Application]::EnableVisualStyles()
        $ui = New-Object Windows.Forms.Form
        $ui.Text = 'UA Reforged'; $ui.FormBorderStyle = 'FixedDialog'; $ui.ControlBox = $false; $ui.StartPosition = 'CenterScreen'
        $ui.ClientSize = New-Object Drawing.Size(420, 96); $ui.Font = New-Object Drawing.Font('Segoe UI', 9.5); $ui.TopMost = $true
        $l = New-Object Windows.Forms.Label; $l.Text = "Installing UA Reforged $Version..."; $l.SetBounds(16, 16, 388, 22)
        $bar = New-Object Windows.Forms.ProgressBar; $bar.Style = 'Marquee'; $bar.SetBounds(16, 50, 388, 20)
        $ui.Controls.AddRange(@($l, $bar)); $ui.Show(); [Windows.Forms.Application]::DoEvents()
    } catch { $ui = $null }
}

# 2. the installer, silently, into the same folder (/D= must come last and is never quoted)
$why = ''
$rc = -1
try {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $Setup; $psi.Arguments = "/S /D=$Inst"
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.WorkingDirectory = $Support
    $p = [Diagnostics.Process]::Start($psi)
    $t1 = [DateTime]::UtcNow
    while (-not $p.HasExited -and ([DateTime]::UtcNow - $t1).TotalMinutes -lt 15) {
        if ($ui) { [Windows.Forms.Application]::DoEvents() }
        Start-Sleep -Milliseconds 200
    }
    if (-not $p.HasExited) { try { $p.Kill() } catch { }; $why = 'the installer did not finish' }
    else { $p.WaitForExit(); $rc = $p.ExitCode }
} catch { $why = 'the installer could not be started' }
Log "update: installer exit $rc"
if (-not $why -and $rc -ne 0) { $why = "the installer stopped (code $rc)" }
if (-not $why) {
    $have = ''
    try {
        foreach ($line in [IO.File]::ReadAllLines((Join-Path $Inst 'launcher\downloads.txt'))) {
            if ($line.StartsWith("app_version`t")) { $have = $line.Substring(12).Trim() }
        }
    } catch { }
    if ($have -ne $Version) { $why = "the installed version is $have, not $Version" }
}
if ($ui) { try { $ui.Close(); $ui.Dispose() } catch { } }

# 3. done: remove the download (this script is already loaded), note the new version for the launcher's log
$dl = Split-Path -Parent $Setup
function Remove-Download {
    for ($i = 0; $i -lt 10; $i++) {
        try { Remove-Item -LiteralPath $dl -Recurse -Force -ErrorAction Stop; return } catch { Start-Sleep -Milliseconds 500 }
    }
    Log "update: could not remove the download folder"
}
if (-not $why) {
    Log "update: installed $Version in $Inst"
    try { [IO.File]::WriteAllText((Join-Path $Support 'update-installed.txt'), $Version) } catch { }
    Remove-Download
} else {
    Log "update: not installed - $why"
    Remove-Download
    # the launcher that starts next does not offer this version again at once (no update loop)
    try { [IO.File]::WriteAllText((Join-Path $Support 'update-failed.txt'), $Version) } catch { }
    if (-not $env:UA_UPDATE_AUTO) {
        try {
            Add-Type -AssemblyName System.Windows.Forms
            [void][Windows.Forms.MessageBox]::Show("UA Reforged could not update itself: $why.`r`n`r`nThe download page opens now, so you can get the new version there. Your current version starts as usual.", 'UA Reforged')
            Start-Process $Page
        } catch { }
    } else { Log "update page: $Page (test run: not opened)" }
}

# 4. start UA Reforged again (the new version, or the current one after a failure)
$exe = Join-Path $Inst 'UA Reforged.exe'
try {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $exe; $psi.WorkingDirectory = $Inst; $psi.UseShellExecute = $false
    [void][Diagnostics.Process]::Start($psi)
    Log "update: started $exe"
} catch { Log "update: could not start UA Reforged: $_" }
exit 0

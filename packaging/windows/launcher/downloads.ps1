# UA Reforged - downloads the app can make for the player (dot-sourced by UA-Reforged.ps1, Windows PowerShell 5.1):
#   * "Get the game": the community OpenUA Standalone Package, fetched straight from the Metropolis Dawn
#     community's own server on the player's PC (UA Reforged never hosts, mirrors or ships those files);
#   * the Reforge pack (the new look), checked against SHA-256 sums shipped in the app.
# The Windows twin of packaging/macos/downloads.sh: same addresses, same checks, same notices.
#
# Where from: downloads.txt next to this file (built-in values; the same file as the Mac app's). Before a download
# the app reads a small links.json from the UA Reforged site (links_url) so a broken link can be fixed without a new
# app. That file is only parsed (Test-LinksJson: strict types, https only, an allowlist of hosts); it can never run
# anything. If it is missing or invalid the built-in values are used.
#
# Downloads use curl.exe, which is part of Windows 10 (1803 and newer) and 11.
#
# Failsafe: every link is checked first (a 1 KB ranged GET: must answer 2xx, must not be a web page, must start
# like a zip, must have a sane size). During a download a stall of 30 s or a dropped connection is retried twice
# (resuming where the server allows it). Any break cleans up the partial files and shows a plain notice with
# "Open help page" (the site's help) or another way on; the app is never left half set up.
#
# Testing only: UA_LINKS_URL, UA_GAME_URL, UA_PACK_URL override addresses (UA_LINKS_ALLOW_LOCAL=1 also accepts
# http://127.0.0.1 in links.json); UA_GAME_AUTO=get answers the first-launch choice; UA_PACK_AUTO=yes|no answers
# the pack offer; UA_NOTICE_AUTO=help|folder|later answers the notices (and opens no browser); UA_PACK_NOUI=1 shows
# no progress window. Windows only: UA_DIALOG_SNAP=<folder> saves every dialog and progress window as a PNG there
# and answers each dialog with its default button (opens no browser).

# ---------------------------------------------------------------- shared helpers (also used by UA-Reforged.ps1)
function Log([string]$msg) {
    try { [IO.File]::AppendAllText($script:LOG, $msg + "`r`n", [Text.Encoding]::UTF8) } catch { }
}

function Alert([string]$msg) {
    if ($env:UA_NOTICE_AUTO -or $env:UA_DIALOG_SNAP) { Log "alert (test run: not shown): $msg"; return }
    [void][System.Windows.Forms.MessageBox]::Show($msg, 'UA Reforged', 'OK', 'Error')
}

function Fail([string]$msg) { Log "stopped: $msg"; Alert $msg; exit 1 }

# ---------------------------------------------------------------- safe file helpers
# A directory junction is removed as a link, never by deleting what it points to (the player's game folder).
function Test-Reparse([string]$p) {
    try { return (([IO.File]::GetAttributes($p) -band [IO.FileAttributes]::ReparsePoint) -ne 0) } catch { return $false }
}

function Remove-Link([string]$p) {
    if (-not (Test-Path -LiteralPath $p)) {
        # a junction whose target is gone still exists as a link
        if (Test-Reparse $p) { [IO.Directory]::Delete($p, $false) }
        return
    }
    if (Test-Reparse $p) {
        if ([IO.Directory]::Exists($p)) { [IO.Directory]::Delete($p, $false) } else { [IO.File]::Delete($p) }
    } elseif ([IO.File]::Exists($p)) {
        [IO.File]::SetAttributes($p, [IO.FileAttributes]::Normal); [IO.File]::Delete($p)
    } else {
        throw "not a link: $p"
    }
}

# Remove-Tree <dir>: delete a folder of ours. Junctions inside it are unlinked, never followed.
function Remove-Tree([string]$p) {
    if (-not $p -or -not ((Test-Path -LiteralPath $p) -or (Test-Reparse $p))) { return }
    if (Test-Reparse $p) { Remove-Link $p; return }
    if ([IO.File]::Exists($p)) { [IO.File]::SetAttributes($p, [IO.FileAttributes]::Normal); [IO.File]::Delete($p); return }
    foreach ($e in [IO.Directory]::GetFileSystemEntries($p)) {
        if (Test-Reparse $e) { Remove-Link $e }
        elseif ([IO.Directory]::Exists($e)) { Remove-Tree $e }
        else { [IO.File]::SetAttributes($e, [IO.FileAttributes]::Normal); [IO.File]::Delete($e) }
    }
    [IO.File]::SetAttributes($p, [IO.FileAttributes]::Directory)
    [IO.Directory]::Delete($p, $false)
}

# hard links (same drive) with a copy as the fallback
$script:HL = $false
try {
    Add-Type -Namespace UaReforged -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool CreateHardLink(string lpFileName, string lpExistingFileName, IntPtr lpSecurityAttributes);
'@
    $script:HL = $true
} catch { $script:HL = $false }

function New-HardLink([string]$link, [string]$target) {
    if ($script:HL) { return [UaReforged.Native]::CreateHardLink($link, $target, [IntPtr]::Zero) }
    try { [void](New-Item -ItemType HardLink -Path $link -Value $target -ErrorAction Stop); return $true } catch { return $false }
}

# Link-OrCopy <link> <target>: 'link' or 'copy'
function Link-OrCopy([string]$link, [string]$target) {
    if (New-HardLink $link $target) { return 'link' }
    [IO.File]::Copy($target, $link, $true); return 'copy'
}

function New-Junction([string]$link, [string]$target) {
    [void](New-Item -ItemType Junction -Path $link -Value $target -ErrorAction Stop)
}

# Move-Dir <from> <to>: rename a folder on the same drive. Right after unpacking, an antivirus scan can hold new files
# open for a few seconds, so it retries for up to 30 s before giving up.
function Move-Dir([string]$from, [string]$to) {
    for ($i = 0; ; $i++) {
        try { [IO.Directory]::Move($from, $to); return }
        catch {
            if ($i -ge 60) { Log "move: $from -> $to failed: $($_.Exception.Message)"; throw }
            if ($i -eq 0) { Log "move: $from -> $to waiting ($($_.Exception.Message))" }
            for ($k = 0; $k -lt 2; $k++) { if (Get-Command Pump -ErrorAction SilentlyContinue) { Pump }; Start-Sleep -Milliseconds 250 }
        }
    }
}

function Is-GameDir([string]$d) {
    if (-not $d) { return $false }
    return ([IO.Directory]::Exists((Join-Path $d 'DATA')) -and [IO.Directory]::Exists((Join-Path $d 'LEVELS')))
}

# ---------------------------------------------------------------- downloads
$script:DL_CONF = Join-Path $HERE 'downloads.txt'
$script:PACK_DL = Join-Path $SUPPORT 'reforge-pack-download'
$script:GAME_DL = Join-Path $SUPPORT 'game-download'
$script:GAME_ORIG = Join-Path $SUPPORT 'game-original'
$script:PACK_OFF = Join-Path $SUPPORT 'reforge-pack-offer-off'
$script:DL_UA = 'UA-Reforged (Windows)'
$script:INV = [Globalization.CultureInfo]::InvariantCulture
$script:LINKS_DONE = $false
$script:UI = $null
$script:DL_CANCEL = $false
$script:DL_PROGRESS = $null
$script:DL_DIR = ''
$script:DL_MAXSIZE = $null
$script:CURL_ERR = ''
$script:L_GAME_URL = ''; $script:L_GAME_MIN = ''; $script:L_GAME_MAX = ''; $script:L_PACK_URL = ''; $script:L_PACK_PARTS = @()
$script:L_HELP = ''; $script:L_PACK_HELP = ''; $script:L_MESSAGE = ''
$script:PC_SIZE = [long]0; $script:PC_RANGES = 0; $script:PC_WHY = ''; $script:DL_WHY = ''

function Conf([string]$k) {
    if (-not [IO.File]::Exists($script:DL_CONF)) { return }
    foreach ($line in [IO.File]::ReadAllLines($script:DL_CONF)) {
        $i = $line.IndexOf("`t")
        if ($i -gt 0 -and $line.Substring(0, $i) -eq $k) { $line.Substring($i + 1) }
    }
}
function Conf1([string]$k) { $v = @(Conf $k); if ($v.Count -gt 0) { return [string]$v[0] } return '' }

function Human([double]$b) {
    if ($b -ge 1e9) { return [string]::Format($script:INV, '{0:0.0} GB', $b / 1e9) }
    if ($b -ge 1e6) { return [string]::Format($script:INV, '{0} MB', [math]::Floor($b / 1e6)) }
    return [string]::Format($script:INV, '{0} KB', [math]::Floor(($b + 999) / 1000))
}

# Windows command-line quoting for one argument (CommandLineToArgvW rules)
function Quote-Arg([string]$a) {
    if ($a.Length -gt 0 -and $a -notmatch '[\s"]') { return $a }
    $sb = New-Object Text.StringBuilder; [void]$sb.Append('"'); $bs = 0
    foreach ($c in $a.ToCharArray()) {
        if ($c -eq '\') { $bs++; continue }
        if ($c -eq '"') { [void]$sb.Append('\', 2 * $bs + 1); $bs = 0; [void]$sb.Append('"'); continue }
        if ($bs) { [void]$sb.Append('\', $bs); $bs = 0 }
        [void]$sb.Append($c)
    }
    if ($bs) { [void]$sb.Append('\', 2 * $bs) }
    [void]$sb.Append('"'); return $sb.ToString()
}

# ---------------------------------------------------------------- dialogs
$script:SNAP_N = 0
function Snap-Form($form, [string]$what) {
    try {
        $script:SNAP_N++
        $bmp = New-Object Drawing.Bitmap($form.Width, $form.Height)
        $form.DrawToBitmap($bmp, (New-Object Drawing.Rectangle(0, 0, $form.Width, $form.Height)))
        $bmp.Save((Join-Path $env:UA_DIALOG_SNAP ('{0:D2}-{1}.png' -f $script:SNAP_N, $what)), [Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
    } catch { Log "snap: $_" }
}

# Ask <text> <default button> <buttons>: a dialog; returns the button (empty when closed).
function Ask([string]$text, [string]$default, [string[]]$buttons) {
    $f = New-Object Windows.Forms.Form
    $f.Text = 'UA Reforged'; $f.FormBorderStyle = 'FixedDialog'; $f.MaximizeBox = $false; $f.MinimizeBox = $false
    $f.StartPosition = 'CenterScreen'; $f.TopMost = $true; $f.AutoSize = $true; $f.AutoSizeMode = 'GrowAndShrink'
    $f.Font = New-Object Drawing.Font('Segoe UI', 9.5); $f.KeyPreview = $true
    if ($script:ICON) { $f.Icon = $script:ICON }
    $tbl = New-Object Windows.Forms.TableLayoutPanel
    $tbl.AutoSize = $true; $tbl.AutoSizeMode = 'GrowAndShrink'; $tbl.ColumnCount = 1; $tbl.RowCount = 2
    $tbl.Padding = New-Object Windows.Forms.Padding(16, 16, 16, 12)
    $lbl = New-Object Windows.Forms.Label
    $lbl.Text = $text; $lbl.AutoSize = $true; $lbl.MaximumSize = New-Object Drawing.Size(480, 0)
    $lbl.Margin = New-Object Windows.Forms.Padding(0, 0, 0, 16)
    $flow = New-Object Windows.Forms.FlowLayoutPanel
    $flow.AutoSize = $true; $flow.AutoSizeMode = 'GrowAndShrink'; $flow.FlowDirection = 'LeftToRight'; $flow.WrapContents = $false
    $flow.Anchor = 'Right'; $flow.Margin = New-Object Windows.Forms.Padding(0)
    $script:AskResult = ''
    $script:AskDefault = $null
    foreach ($t in $buttons) {
        $b = New-Object Windows.Forms.Button
        $b.Text = $t; $b.Tag = $t; $b.AutoSize = $true; $b.MinimumSize = New-Object Drawing.Size(96, 30)
        $b.Margin = New-Object Windows.Forms.Padding(8, 0, 0, 0)
        $b.Add_Click({ $script:AskResult = [string]$this.Tag; $this.FindForm().Close() })
        [void]$flow.Controls.Add($b)
        if ($t -eq $default) { $f.AcceptButton = $b; $script:AskDefault = $b }
    }
    $f.Add_KeyDown({ if ($_.KeyCode -eq 'Escape') { $script:AskResult = ''; $this.Close() } })
    $f.Add_Shown({ $this.Activate(); if ($script:AskDefault) { [void]$script:AskDefault.Focus() } })
    if ($env:UA_DIALOG_SNAP) {
        $script:AskDefaultText = $default
        $f.Add_Shown({ Pump; Snap-Form $this 'dialog'; $script:AskResult = $script:AskDefaultText; $this.Close() })
    }
    [void]$tbl.Controls.Add($lbl, 0, 0); [void]$tbl.Controls.Add($flow, 0, 1)
    [void]$f.Controls.Add($tbl)
    [void]$f.ShowDialog()
    $f.Dispose()
    return $script:AskResult
}

function Open-Help([string]$url) {
    if ($env:UA_NOTICE_AUTO -or $env:UA_DIALOG_SNAP) { Log "help page: $url (test run: not opened)"; return }
    Log "help page: $url"
    try { Start-Process $url } catch { Log "could not open the browser: $_" }
}

# ---------------------------------------------------------------- links: built-in values, then the site's links.json
# Test-LinksJson <file> <allowed hosts> <allow local http>: the Windows twin of packaging/macos/links.js.
# Returns "key<TAB>value" lines; throws (the reason) on any problem, and then the WHOLE file is ignored.
function Test-LinksJson([string]$path, [string[]]$hosts, [bool]$local) {
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes.Length -gt 16384) { throw 'missing or too big' }
    try { $text = (New-Object Text.UTF8Encoding($false, $true)).GetString($bytes) } catch { throw 'not UTF-8 text' }
    $text = $text.TrimStart([char]0xFEFF)
    try { $j = ConvertFrom-Json -InputObject $text -ErrorAction Stop } catch { throw 'not valid JSON' }
    if ($null -eq $j -or $j -isnot [Management.Automation.PSCustomObject]) { throw 'not a JSON object' }
    $props = @{}
    foreach ($p in $j.PSObject.Properties) { $props[$p.Name] = $p.Value }
    function IsInt($n) { return ($n -is [int] -or $n -is [long] -or $n -is [decimal] -or $n -is [double]) -and ([double]$n % 1 -eq 0) }
    $checkUrl = {
        param($k)
        $u = $props[$k]
        if ($u -isnot [string] -or $u.Length -gt 512 -or $u -match '[\s\x00-\x1f\x7f"''`\\]') { throw "${k}: not a plain URL" }
        $m = [regex]::Match($u, '^(https|http)://([A-Za-z0-9.-]+)(:[0-9]{1,5})?(/[^#?]*)?(\?[^#]*)?(#[A-Za-z0-9_-]*)?$')
        if (-not $m.Success) { throw "${k}: not an http(s) URL" }
        $h = $m.Groups[2].Value.ToLowerInvariant()
        if ($m.Groups[1].Value -ceq 'http') {
            if (-not ($local -and ($h -eq '127.0.0.1' -or $h -eq 'localhost'))) { throw "${k}: https only" }
            return $u
        }
        if ($m.Groups[3].Success) { throw "${k}: no port numbers" }
        $ok = $false
        foreach ($a in $hosts) { $a = $a.ToLowerInvariant(); if ($h -eq $a -or $h.EndsWith('.' + $a)) { $ok = $true } }
        if (-not $ok) { throw "${k}: host $h is not on the allowlist" }
        return $u
    }
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($k in @('game_url', 'pack_url', 'help_url', 'pack_help_url', 'quest_apk_url', 'quest_help_url')) {
        if ($props.ContainsKey($k)) { $out.Add("$k`t" + (& $checkUrl $k)) }
    }
    if ($props.ContainsKey('game_min_bytes')) {
        $n = $props['game_min_bytes']
        if (-not (IsInt $n) -or [double]$n -lt 1e6 -or [double]$n -gt 1e10) { throw 'game_min_bytes: not a whole number of bytes (1 MB..10 GB)' }
        $out.Add("game_min_bytes`t" + ([long]$n).ToString($script:INV))
    }
    if ($props.ContainsKey('message')) {
        $msg = $props['message']
        if ($msg -isnot [string] -or $msg.Length -gt 300 -or $msg -match '[\x00-\x1f\x7f]') { throw 'message: plain text of at most 300 characters' }
        if ($msg) { $out.Add("message`t$msg") }
    }
    if ($props.ContainsKey('pack_parts')) {
        $pp = $props['pack_parts']
        if ($pp -isnot [array] -or $pp.Count -lt 1 -or $pp.Count -gt 8) { throw 'pack_parts: a list of 1 to 8 parts' }
        for ($i = 0; $i -lt $pp.Count; $i++) {
            $x = $pp[$i]
            if ($null -eq $x -or $x -isnot [Management.Automation.PSCustomObject]) { throw "pack_parts[$i]: not an object" }
            $nm = $x.PSObject.Properties['name']; $by = $x.PSObject.Properties['bytes']; $sh = $x.PSObject.Properties['sha256']
            if (-not $nm -or $nm.Value -isnot [string] -or $nm.Value -cnotmatch '^[A-Za-z0-9._-]{1,80}\.zip$' -or $nm.Value.Contains('..')) { throw "pack_parts[$i].name" }
            if (-not $by -or -not (IsInt $by.Value) -or [double]$by.Value -lt 1 -or [double]$by.Value -gt 1e10) { throw "pack_parts[$i].bytes" }
            if (-not $sh -or $sh.Value -isnot [string] -or $sh.Value -cnotmatch '^[0-9a-f]{64}$') { throw "pack_parts[$i].sha256" }
            $out.Add("part`t" + $nm.Value + "`t" + ([long]$by.Value).ToString($script:INV) + "`t" + $sh.Value)
        }
    }
    $isSha = { param($v) ($v -is [string]) -and ($v -cmatch '^[0-9a-f]{64}$') }
    $isBytes = { param($v) (IsInt $v) -and [double]$v -ge 1 -and [double]$v -le 1e10 }
    if ($props.ContainsKey('quest_apk')) {
        $x = $props['quest_apk']
        if ($null -eq $x -or $x -isnot [Management.Automation.PSCustomObject]) { throw 'quest_apk: not an object' }
        $g = @{}; foreach ($q in $x.PSObject.Properties) { $g[$q.Name] = $q.Value }
        if ($g['name'] -isnot [string] -or $g['name'] -cnotmatch '^[A-Za-z0-9._-]{1,80}\.apk$' -or $g['name'].Contains('..')) { throw 'quest_apk.name' }
        if (-not (& $isBytes $g['bytes'])) { throw 'quest_apk.bytes' }
        if (-not (& $isSha $g['sha256'])) { throw 'quest_apk.sha256' }
        if (-not (IsInt $g['version_code']) -or [double]$g['version_code'] -lt 1 -or [double]$g['version_code'] -gt 2100000000) { throw 'quest_apk.version_code' }
        if ($g['version'] -isnot [string] -or $g['version'] -cnotmatch '^[0-9A-Za-z.+-]{1,32}$') { throw 'quest_apk.version' }
        $out.Add("quest_apk`t" + $g['name'] + "`t" + ([long]$g['bytes']).ToString($script:INV) + "`t" + $g['sha256'] + "`t" + ([long]$g['version_code']).ToString($script:INV) + "`t" + $g['version'])
    }
    foreach ($k in @('adb_mac', 'adb_win')) {
        if (-not $props.ContainsKey($k)) { continue }
        $x = $props[$k]
        if ($null -eq $x -or $x -isnot [Management.Automation.PSCustomObject]) { throw "${k}: not an object" }
        $g = @{}; foreach ($q in $x.PSObject.Properties) { $g[$q.Name] = $q.Value }
        $props["$k.url"] = $g['url']
        $u = & $checkUrl "$k.url"
        if ($u -notmatch '\.zip$') { throw "$k.url: not a zip" }
        if (-not (& $isBytes $g['bytes'])) { throw "$k.bytes" }
        if (-not (& $isSha $g['sha256'])) { throw "$k.sha256" }
        $out.Add("$k`t$u`t" + ([long]$g['bytes']).ToString($script:INV) + "`t" + $g['sha256'])
    }
    return $out.ToArray()
}

function Links-Load {
    $script:L_GAME_URL = Conf1 'game_url'; $script:L_GAME_MIN = Conf1 'game_min_bytes'; $script:L_GAME_MAX = Conf1 'game_max_bytes'
    $script:L_PACK_URL = Conf1 'url'; $script:L_PACK_PARTS = @(Conf 'part')
    $script:L_HELP = Conf1 'help_url'; $script:L_PACK_HELP = Conf1 'pack_help_url'; $script:L_MESSAGE = ''
    $script:L_QUEST_APK_URL = ''; $script:L_QUEST_APK = ''; $script:L_QUEST_HELP = ''; $script:L_ADB_WIN = ''
    $src = if ($env:UA_LINKS_URL) { $env:UA_LINKS_URL } else { Conf1 'links_url' }
    $tmp = Join-Path $SUPPORT 'links.json.tmp'
    $allow = @(Conf 'allow_host')
    $loc = [bool]$env:UA_LINKS_ALLOW_LOCAL
    $proto = if ($loc) { '=https,http' } else { '=https' }
    if ([IO.File]::Exists($tmp)) { [IO.File]::Delete($tmp) }
    $rc = 1
    if ($src) {
        $rc = Run-Curl @('-fsSL', '--proto', $proto, '--proto-redir', $proto, '--max-time', '8', '--connect-timeout', '5',
                         '--max-filesize', '16384', '-A', $script:DL_UA, '-o', $tmp, $src) $null
    }
    if ($rc -eq 0 -and [IO.File]::Exists($tmp)) {
        try {
            $lines = Test-LinksJson $tmp $allow $loc
            $parts = New-Object System.Collections.Generic.List[string]
            foreach ($l in $lines) {
                $i = $l.IndexOf("`t"); $k = $l.Substring(0, $i); $v = $l.Substring($i + 1)
                switch ($k) {
                    'game_url' { $script:L_GAME_URL = $v } 'game_min_bytes' { $script:L_GAME_MIN = $v } 'pack_url' { $script:L_PACK_URL = $v }
                    'help_url' { $script:L_HELP = $v } 'pack_help_url' { $script:L_PACK_HELP = $v } 'message' { $script:L_MESSAGE = $v }
                    'part' { $parts.Add($v) }
                    'quest_apk_url' { $script:L_QUEST_APK_URL = $v } 'quest_apk' { $script:L_QUEST_APK = $v }
                    'quest_help_url' { $script:L_QUEST_HELP = $v } 'adb_win' { $script:L_ADB_WIN = $v }
                }
            }
            if ($parts.Count -gt 0) { $script:L_PACK_PARTS = $parts.ToArray() }
            Log "links: $src accepted"
        } catch {
            Log "links: $src rejected ($($_.Exception.Message)); built-in links used"
        }
    } elseif ($src) {
        Log "links: $src not reachable; built-in links used"
    }
    if ([IO.File]::Exists($tmp)) { [IO.File]::Delete($tmp) }
    if ($script:CURL_ERR -and [IO.File]::Exists($script:CURL_ERR)) { [IO.File]::Delete($script:CURL_ERR) }
    if ($env:UA_GAME_URL) { $script:L_GAME_URL = $env:UA_GAME_URL }
    if ($env:UA_PACK_URL) { $script:L_PACK_URL = $env:UA_PACK_URL }
    if ($script:L_PACK_URL -and -not $script:L_PACK_URL.EndsWith('/')) { $script:L_PACK_URL += '/' }
    $script:LINKS_DONE = $true
}

# ---------------------------------------------------------------- curl.exe (Windows 10 1803+ / 11)
$script:CURL = Join-Path $env:SystemRoot 'System32\curl.exe'
$script:CURL_EXTRA = $null
function Curl-Extra {
    if ($null -ne $script:CURL_EXTRA) { return $script:CURL_EXTRA }
    $script:CURL_EXTRA = @()
    try {
        # Schannel: an unreachable certificate revocation server must not fail the download (curl 7.70+)
        $v = & $script:CURL -V 2>$null | Select-Object -First 1
        if ($v -match '^curl (\d+)\.(\d+)') {
            if ([int]$Matches[1] -gt 7 -or ([int]$Matches[1] -eq 7 -and [int]$Matches[2] -ge 70)) { $script:CURL_EXTRA = @('--ssl-revoke-best-effort') }
        }
    } catch { }
    return $script:CURL_EXTRA
}

# Start-Curl <args>: starts curl.exe hidden; its error text goes to $DL_DIR\curl.last (or the support folder)
function Start-Curl([string[]]$a, [bool]$capture) {
    $errDir = if ($script:DL_DIR) { $script:DL_DIR } else { $SUPPORT }
    $script:CURL_ERR = Join-Path $errDir 'curl.last'
    $all = @(Curl-Extra) + @('--stderr', $script:CURL_ERR) + $a
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $script:CURL
    $psi.Arguments = (($all | ForEach-Object { Quote-Arg ([string]$_) }) -join ' ')
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $capture
    return [System.Diagnostics.Process]::Start($psi)
}

# Run-Curl <args> <[ref] stdout or $null>: runs to the end (the window keeps answering); returns the exit code
function Run-Curl([string[]]$a, $outRef) {
    if (-not [IO.File]::Exists($script:CURL)) { return 2 }
    $p = Start-Curl $a ($null -ne $outRef)
    $task = $null
    if ($null -ne $outRef) { $task = $p.StandardOutput.ReadToEndAsync() }
    while (-not $p.HasExited) { Pump; Start-Sleep -Milliseconds 100 }
    $p.WaitForExit()
    if ($null -ne $outRef) { $outRef.Value = $task.Result }
    return $p.ExitCode
}

function Curl-LastError {
    try { $l = @([IO.File]::ReadAllLines($script:CURL_ERR) | Where-Object { $_ }); if ($l.Count) { return ($l[-1] -replace '^curl: \(\d+\) ', '') } } catch { }
    return ''
}

# ---------------------------------------------------------------- progress window + cancel
function Pump { if ($script:UI) { [Windows.Forms.Application]::DoEvents() } }

function Dl-Status([int]$pct, [string]$text) {
    $script:DL_TEXT = $text
    if ($script:UI) {
        $script:UI_BAR.Value = [math]::Max(0, [math]::Min(100, $pct))
        $script:UI_TEXT.Text = $text
        Pump
        if ($env:UA_DIALOG_SNAP -and $text -like 'Downloading*' -and -not $script:UI_SNAPPED) { $script:UI_SNAPPED = $true; Snap-Form $script:UI 'progress' }
    }
}

function Dl-UiStart([string]$title) {
    $script:DL_CANCEL = $false
    $script:UI = $null; $script:UI_SNAPPED = $false
    if (-not $env:UA_PACK_NOUI) {
        $f = New-Object Windows.Forms.Form
        $f.Text = 'UA Reforged'; $f.FormBorderStyle = 'FixedDialog'; $f.MaximizeBox = $false; $f.MinimizeBox = $true
        $f.StartPosition = 'CenterScreen'; $f.ClientSize = New-Object Drawing.Size(460, 150)
        $f.Font = New-Object Drawing.Font('Segoe UI', 9.5)
        if ($script:ICON) { $f.Icon = $script:ICON }
        $t = New-Object Windows.Forms.Label; $t.Text = $title; $t.AutoSize = $false
        $t.SetBounds(16, 14, 428, 22); $t.Font = New-Object Drawing.Font('Segoe UI', 10, [Drawing.FontStyle]::Bold)
        $s = New-Object Windows.Forms.Label; $s.AutoSize = $false; $s.SetBounds(16, 42, 428, 22)
        $bar = New-Object Windows.Forms.ProgressBar; $bar.SetBounds(16, 70, 428, 20); $bar.Minimum = 0; $bar.Maximum = 100
        $c = New-Object Windows.Forms.Button; $c.Text = 'Cancel'; $c.SetBounds(348, 104, 96, 30)
        $c.Add_Click({ $script:DL_CANCEL = $true; $script:UI_TEXT.Text = 'Stopping...' })
        $f.Add_FormClosing({ param($s2, $e2) if (-not $script:UI_CLOSING) { $e2.Cancel = $true; $script:DL_CANCEL = $true } })
        $f.Controls.AddRange(@($t, $s, $bar, $c))
        $f.CancelButton = $c
        $script:UI_TEXT = $s; $script:UI_BAR = $bar; $script:UI_CLOSING = $false
        $script:UI = $f
        $f.Show(); $f.Activate()
    }
    Dl-Status 0 'Checking the download link...'
}

function Dl-UiEnd {
    if ($script:UI) { $script:UI_CLOSING = $true; $script:UI.Close(); $script:UI.Dispose(); $script:UI = $null }
}

# Dl-Wait <process>: wait for a background job; 130 = the player pressed Cancel
function Dl-Wait($p) {
    while (-not $p.HasExited) {
        if ($script:DL_CANCEL) { try { $p.Kill() } catch { }; $p.WaitForExit(); return 130 }
        if ($script:DL_PROGRESS) { & $script:DL_PROGRESS }
        Pump; Start-Sleep -Milliseconds 250
    }
    $p.WaitForExit()
    return $p.ExitCode
}

function File-Size([string]$f) { if ([IO.File]::Exists($f)) { return (New-Object IO.FileInfo($f)).Length } return [long]0 }

function Dl-Progress {
    $now = $script:DL_BASE + (File-Size $script:DL_FILE)
    if ($script:DL_TOTAL -gt 0) { Dl-Status ([int]($now * 90 / $script:DL_TOTAL)) ("{0}: {1} of {2}" -f $script:DL_WHAT, (Human $now), (Human $script:DL_TOTAL)) }
    else { Dl-Status 5 ("{0}: {1}" -f $script:DL_WHAT, (Human $now)) }
}

# ---------------------------------------------------------------- link check: Dl-Precheck <url> <min> <max> <exact>
# sets PC_SIZE (0 = unknown), PC_RANGES (1/0); on failure returns $false with PC_WHY in plain words
function Dl-Precheck([string]$url, [long]$min = 0, [long]$max = 0, [long]$exact = 0, [bool]$quiet = $false) {
    $h = Join-Path $script:DL_DIR 'check.h'; $b = Join-Path $script:DL_DIR 'check.b'
    $script:PC_SIZE = [long]0; $script:PC_RANGES = 0; $script:PC_WHY = ''
    foreach ($x in @($h, $b)) { if ([IO.File]::Exists($x)) { [IO.File]::Delete($x) } }
    if (-not [IO.File]::Exists($script:CURL)) {
        $script:PC_WHY = 'this version of Windows has no curl.exe (Windows 10 version 1803 or newer is needed)'
        Log "link check: $url -> $($script:PC_WHY)"; return $false
    }
    $w = ''
    $rc = Run-Curl @('-sS', '-L', '--max-time', '20', '--connect-timeout', '10', '--max-redirs', '8', '-r', '0-1023', '-A', $script:DL_UA,
                     '-D', $h, '-o', $b, '-w', '%{http_code}\t%{content_type}', $url) ([ref]$w)
    $i = $w.IndexOf("`t")
    if ($i -ge 0) { $code = $w.Substring(0, $i); $type = $w.Substring($i + 1).ToLowerInvariant() } else { $code = $w; $type = '' }
    if ($rc -ne 0) {
        switch ($rc) {
            6 { $script:PC_WHY = 'the server name could not be found' }
            7 { $script:PC_WHY = 'the server could not be reached' }
            28 { $script:PC_WHY = 'the server did not answer in time' }
            { $_ -in 35, 51, 60 } { $script:PC_WHY = 'the secure connection failed' }
            default { $script:PC_WHY = "the connection failed (curl $rc)" }
        }
        Log "link check: $url -> $($script:PC_WHY)"; return $false
    }
    if ($code -ne '200' -and $code -ne '206') { $script:PC_WHY = "the server answered HTTP $code instead of the file"; Log "link check: $url -> $($script:PC_WHY)"; return $false }
    if ($type -like 'text/html*' -or $type -like 'application/xhtml*') { $script:PC_WHY = 'the link now leads to a web page, not to the file'; Log "link check: $url -> $($script:PC_WHY) ($type)"; return $false }
    $magic = ''
    try { $fs = [IO.File]::OpenRead($b); $buf = New-Object byte[] 4; $n = $fs.Read($buf, 0, 4); $fs.Dispose(); if ($n -eq 4) { $magic = [BitConverter]::ToString($buf) } } catch { }
    if ($magic -ne '50-4B-03-04') { $script:PC_WHY = 'the link does not lead to a zip file'; Log "link check: $url -> $($script:PC_WHY)"; return $false }
    # the last response's headers (redirects come first)
    $cr = ''; $cl = ''
    if ([IO.File]::Exists($h)) {
        foreach ($line in [IO.File]::ReadAllLines($h)) {
            if ($line -match '^HTTP/') { $cr = ''; $cl = '' }
            elseif ($line -match '^(?i)content-range:\s*(.*)$') { $cr = $Matches[1].Trim() }
            elseif ($line -match '^(?i)content-length:\s*(.*)$') { $cl = $Matches[1].Trim() }
        }
    }
    $sz = if ($code -eq '206') { $script:PC_RANGES = 1; ($cr -split '/')[-1] } else { $cl }
    $parsed = [long]0
    if ([long]::TryParse($sz, [ref]$parsed)) { $script:PC_SIZE = $parsed } else { $script:PC_SIZE = [long]0 }
    if ($script:PC_SIZE -gt 0) {
        if ($min -gt 0 -and $script:PC_SIZE -lt $min) { $script:PC_WHY = "the file on the server is far too small ($(Human $script:PC_SIZE))" }
        if ($max -gt 0 -and $script:PC_SIZE -gt $max) { $script:PC_WHY = "the file on the server is far too big ($(Human $script:PC_SIZE))" }
        if ($exact -gt 0 -and $script:PC_SIZE -ne $exact) { $script:PC_WHY = "the file on the server is not the expected one (size $($script:PC_SIZE), expected $exact)" }
        if ($script:PC_WHY) { Log "link check: $url -> $($script:PC_WHY)"; return $false }
    }
    if (-not $quiet) { Log "link check: $url ok (HTTP $code, $(if ($type) { $type } else { 'no type' }), size $($script:PC_SIZE), ranges $($script:PC_RANGES))" }
    return $true
}

# ---------------------------------------------------------------- download: Dl-Fetch <url> <file>
# 2 automatic retries after a stall (no bytes for 30 s) or a dropped connection; resumes when the server allows it.
# returns 0, 130 (cancelled) or 1 with DL_WHY
function Dl-Fetch([string]$url, [string]$f) {
    $try = 0; $script:DL_WHY = ''; $script:DL_FILE = $f; $script:DL_PROGRESS = { Dl-Progress }
    $errAll = Join-Path $script:DL_DIR 'curl.err'
    while ($true) {
        $resume = @()
        if ($script:PC_RANGES -eq 1 -and (File-Size $f) -gt 0) { $resume = @('-C', '-') }
        if ($script:PC_RANGES -ne 1 -and [IO.File]::Exists($f)) { [IO.File]::Delete($f) }
        $a = @('-fL', '-sS', '--connect-timeout', '15', '--speed-limit', '1', '--speed-time', '30', '--max-redirs', '8')
        if ($script:DL_MAXSIZE) { $a += @('--max-filesize', [string]$script:DL_MAXSIZE) }
        $a += @('-A', $script:DL_UA) + $resume + @('-o', $f, $url)
        $p = Start-Curl $a $false
        $rc = Dl-Wait $p
        try { if ([IO.File]::Exists($script:CURL_ERR)) { [IO.File]::AppendAllText($errAll, [IO.File]::ReadAllText($script:CURL_ERR)) } } catch { }
        if ($rc -eq 0) { break }
        if ($rc -eq 130) { $script:DL_PROGRESS = $null; return 130 }
        if ($rc -eq 33) { $script:PC_RANGES = 0; if ([IO.File]::Exists($f)) { [IO.File]::Delete($f) } }
        switch ($rc) {
            28 { $script:DL_WHY = 'the download stalled (no data for 30 seconds)' }
            { $_ -in 18, 52, 55, 56, 92 } { $script:DL_WHY = 'the connection dropped during the download' }
            22 { $script:DL_WHY = "the server stopped serving the file ($(Curl-LastError))" }
            63 { $script:DL_WHY = 'the file on the server is far too big' }
            default { $script:DL_WHY = "the download failed (curl $rc)" }
        }
        $try++
        Log "download: $url -> $($script:DL_WHY) (attempt $try)"
        if ($try -gt 2) { $script:DL_PROGRESS = $null; return 1 }
        if ($rc -eq 22 -and (File-Size $f) -eq 0) { $script:DL_PROGRESS = $null; return 1 }
        for ($k = 0; $k -lt 8; $k++) { Pump; Start-Sleep -Milliseconds 250 }
    }
    $script:DL_PROGRESS = $null; return 0
}

# ---------------------------------------------------------------- SHA-256 + unzip (the window keeps answering)
function Get-Sha256([string]$f) {
    $sha = [Security.Cryptography.SHA256]::Create(); $fs = [IO.File]::OpenRead($f)
    try {
        $buf = New-Object byte[] (4MB)
        while (($n = $fs.Read($buf, 0, $buf.Length)) -gt 0) {
            [void]$sha.TransformBlock($buf, 0, $n, $null, 0)
            if ($script:DL_CANCEL) { return '' }
            Pump
        }
        [void]$sha.TransformFinalBlock($buf, 0, 0)
        return (([BitConverter]::ToString($sha.Hash)) -replace '-', '').ToLowerInvariant()
    } finally { $fs.Dispose(); $sha.Dispose() }
}

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
# Expand-Zip <zip> <dest>: 0 ok, 130 cancelled, 1 damaged (or a name that would land outside <dest>)
function Expand-Zip([string]$zip, [string]$dest) {
    try {
        [void][IO.Directory]::CreateDirectory($dest)
        $root = [IO.Path]::GetFullPath($dest).TrimEnd('\') + '\'
        $z = [IO.Compression.ZipFile]::OpenRead($zip)
        try {
            $buf = New-Object byte[] (1MB); $count = 0
            foreach ($e in $z.Entries) {
                $name = $e.FullName.Replace('/', '\')
                if (-not $name) { continue }
                $target = [IO.Path]::GetFullPath([IO.Path]::Combine($root, $name))
                if (-not $target.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { throw "unsafe name in the zip: $($e.FullName)" }
                if ($name.EndsWith('\')) { [void][IO.Directory]::CreateDirectory($target); continue }
                [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
                $in = $e.Open(); $out = [IO.File]::Create($target)
                try {
                    $chunk = 0
                    while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
                        $out.Write($buf, 0, $n)
                        if ((++$chunk % 32) -eq 0) { Pump; if ($script:DL_CANCEL) { return 130 } }
                    }
                } finally { $out.Dispose(); $in.Dispose() }
                if ((++$count % 20) -eq 0) { Pump; if ($script:DL_CANCEL) { return 130 } }
            }
        } finally { $z.Dispose() }
        return 0
    } catch {
        Log "unzip: $zip -> $($_.Exception.Message)"
        return 1
    }
}

# ---------------------------------------------------------------- Reforge pack: shared files (the Windows install-links.sh)
# Install-PackLinks <pack> <verify>: makes every LINKS.tsv path a hard link of its stored file (a copy where hard links
# are not possible, e.g. some external drives). With verify, also checks the SHA-256 of every linked file.
function Install-PackLinks([string]$pack, [bool]$verify) {
    $list = Join-Path $pack 'LINKS.tsv'
    if (-not [IO.File]::Exists($list)) { Log 'links: LINKS.tsv not found'; return $false }
    $made = 0; $had = 0; $cp = 0; $bad = 0; $sums = @{}; $i = 0
    foreach ($line in [IO.File]::ReadAllLines($list)) {
        if (-not $line -or $line.StartsWith('#')) { continue }
        $c = $line.Split("`t")
        if ($c.Count -lt 3 -or $c[0].Contains('..') -or $c[1].Contains('..') -or $c[0].StartsWith('/') -or $c[1].StartsWith('/') -or $c[0].Contains(':') -or $c[1].Contains(':')) {
            Log "links: bad line: $line"; $bad++; continue
        }
        $link = [IO.Path]::Combine($pack, $c[0].Replace('/', '\')); $target = [IO.Path]::Combine($pack, $c[1].Replace('/', '\')); $sum = $c[2].ToLowerInvariant()
        if (-not [IO.File]::Exists($target)) { Log "links: missing in the pack: $($c[1])"; $bad++; continue }
        $how = 'had'
        if ([IO.File]::Exists($link)) { $had++ }
        else {
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($link))
            try { $how = Link-OrCopy $link $target } catch { Log "links: could not create $($c[0])"; $bad++; continue }
            if ($how -eq 'link') { $made++ } else { $cp++ }
        }
        if ($verify) {
            # a hard link IS the stored file: hash each stored file once; a copy (or a file that was already there) is hashed itself
            $key = if ($how -eq 'link') { $target } else { $link }
            if (-not $sums.ContainsKey($key)) { $sums[$key] = Get-Sha256 $key }
            if ($script:DL_CANCEL) { return $false }
            if ($sums[$key] -ne $sum) { Log "links: checksum mismatch: $($c[0])"; $bad++ }
        }
        if ((++$i % 200) -eq 0) { Pump }
    }
    Log "links: $made made, $had already there, 0 as symbolic links, $cp copied, $bad problems"
    if ($bad -eq 0) { [IO.File]::WriteAllText((Join-Path $pack '.links-done'), (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + "`r`n") }
    return ($bad -eq 0)
}

# ======================================================================== Get the game
# Game-Valid <dir>: the folder holds the original game's DATA and LEVELS with the key files
function Game-Valid([string]$d) {
    $data = Join-Path $d 'DATA'; $levels = Join-Path $d 'LEVELS'
    if (-not [IO.Directory]::Exists($data) -or -not [IO.Directory]::Exists($levels)) { return $false }
    if (-not [IO.File]::Exists((Join-Path $data 'world.ini'))) { return $false }
    foreach ($s in @('SET1', 'SCRIPTS', 'SOUNDS')) { if (-not [IO.Directory]::Exists((Join-Path $data $s))) { return $false } }
    $single = Join-Path $levels 'SINGLE'
    if (-not [IO.Directory]::Exists($single)) { return $false }
    $n = 0
    foreach ($x in [IO.Directory]::EnumerateFiles($single, '*', 'AllDirectories')) { if ((++$n) -ge 10) { break } }
    return ($n -ge 10)
}

function Free-Bytes([string]$p) {
    try { return (New-Object IO.DriveInfo([IO.Path]::GetPathRoot([IO.Path]::GetFullPath($p)))).AvailableFreeSpace } catch { return [long]::MaxValue }
}

# Game-Broken <why>: clean up, explain, offer the help page. returns 2 (help page opened: the launcher stops)
# or 3 (choose a folder instead)
function Game-Broken([string]$why) {
    try { Remove-Tree $script:GAME_DL } catch { Log "cleanup: $_" }
    Dl-UiEnd
    Log "get the game: broken - $why"
    $b = '' + $env:UA_NOTICE_AUTO
    if (-not $b) {
        $b = Ask ("The game download link isn't working right now (it's hosted by the Metropolis Dawn community). We'll open the UA Reforged help page with other ways to get it.`r`n`r`nWhat went wrong: $why.") 'Open help page' @('Choose a folder instead', 'Open help page')
    }
    if ($b -eq 'folder' -or $b -eq 'Choose a folder instead') { return 3 }
    $u = if ($script:L_HELP) { $script:L_HELP } else { 'https://uareforged.pages.dev/downloads/#nogame-h' }
    Open-Help $u; return 2
}

# Game-Get: on success sets GAME. returns 0 ok, 1 cancelled (back to the choices), 2 help page opened (stop),
# 3 choose a folder instead
function Game-Get {
    if (-not $script:LINKS_DONE) { Links-Load }
    $url = $script:L_GAME_URL
    $min = [long]100000000; $max = [long]5000000000
    if ($script:L_GAME_MIN) { $min = [long]$script:L_GAME_MIN }
    if ($script:L_GAME_MAX) { $max = [long]$script:L_GAME_MAX }
    if (-not $env:UA_GAME_AUTO) {
        $note = "UA Reforged will download the OpenUA Standalone Package (the full original game plus the Metropolis Dawn add-on, about 445 MB) straight from the Metropolis Dawn community's server. It is hosted by the Metropolis Dawn community, not by UA Reforged, and the game files belong to Microsoft.`r`n`r`nIt goes to $($script:GAME_ORIG)."
        if ($script:L_MESSAGE) { $note += "`r`n`r`n" + $script:L_MESSAGE }
        if ((Ask $note 'Download' @('Cancel', 'Download')) -ne 'Download') { return 1 }
    }
    $script:DL_DIR = $script:GAME_DL
    try { [void][IO.Directory]::CreateDirectory($script:DL_DIR) } catch { return 1 }
    if ((Free-Bytes $SUPPORT) -lt 1500000000) { return (Game-Broken 'there is not enough free disk space (it needs about 1.5 GB)') }
    Dl-UiStart 'Urban Assault (from the Metropolis Dawn community)'
    if (-not (Dl-Precheck $url $min $max 0)) { return (Game-Broken $script:PC_WHY) }
    $f = Join-Path $script:DL_DIR 'openua-package.zip'
    $script:DL_WHAT = 'Downloading the game'; $script:DL_BASE = [long]0; $script:DL_TOTAL = $script:PC_SIZE; $script:DL_MAXSIZE = $max
    $rc = Dl-Fetch $url $f; $script:DL_MAXSIZE = $null
    if ($rc -eq 130) { Dl-UiEnd; Log 'get the game: cancelled (the part stays for a resume)'; return 1 }
    if ($rc -ne 0) { return (Game-Broken $script:DL_WHY) }
    $got = File-Size $f
    if ($got -lt $min) { return (Game-Broken "the downloaded file is far too small ($(Human $got))") }
    Dl-Status 92 'Unpacking the game...'
    $unpack = Join-Path $script:DL_DIR 'unpack'
    Remove-Tree $unpack
    $rc = Expand-Zip $f $unpack
    if ($rc -eq 130) { Remove-Tree $unpack; Dl-UiEnd; return 1 }
    if ($rc -ne 0) { return (Game-Broken 'the downloaded file is damaged (it does not unpack)') }
    $root = ''
    $cands = @($unpack) + @(Get-ChildItem -LiteralPath $unpack -Directory -Recurse -Depth 3 -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
    foreach ($d in $cands) { if (Game-Valid $d) { $root = $d; break } }
    if (-not $root) { return (Game-Broken 'the download has no complete DATA and LEVELS folders') }
    Dl-Status 98 'Setting up...'
    $rel = $root.Substring($unpack.Length)
    try {
        Remove-Tree $script:GAME_ORIG
        Move-Dir $unpack $script:GAME_ORIG
    } catch { Log "get the game: $($_.Exception.Message)"; return (Game-Broken 'the game folder could not be created') }
    $script:GAME = $script:GAME_ORIG + $rel
    Dl-Status 100 'done'; Start-Sleep -Milliseconds 500; Dl-UiEnd; Remove-Tree $script:GAME_DL
    Log "get the game: installed in $($script:GAME)"
    return 0
}

# ======================================================================== Reforge pack
function Pack-Broken([string]$why) { # explain, offer the help page; the game always starts (1998 look)
    try { Remove-Tree $script:PACK_DL } catch { Log "cleanup: $_" }
    Dl-UiEnd
    Log "reforge pack: broken - $why"
    $b = '' + $env:UA_NOTICE_AUTO
    if (-not $b) {
        $b = Ask ("The Reforge pack download isn't working right now. The game starts with its 1998 look; UA Reforged offers the pack again on a later start.`r`n`r`nWhat went wrong: $why.") 'Open help page' @('Try again later', 'Open help page')
    }
    if ($b -eq 'help' -or $b -eq 'Open help page') {
        $u = if ($script:L_PACK_HELP) { $script:L_PACK_HELP } else { 'https://uareforged.pages.dev/downloads/#install-h' }
        Open-Help $u
    }
    return 1
}

function Pack-Parts {
    foreach ($l in $script:L_PACK_PARTS) {
        $c = $l.Split("`t")
        if ($c.Count -ge 3) { [pscustomobject]@{ Name = $c[0]; Bytes = [long]$c[1]; Sum = $c[2].ToLowerInvariant() } }
    }
}
function Pack-Total { $t = [long]0; foreach ($p in @(Pack-Parts)) { $t += $p.Bytes }; return $t }

function Pack-Download {
    $total = Pack-Total; $done = [long]0
    $script:DL_DIR = $script:PACK_DL
    try { [void][IO.Directory]::CreateDirectory($script:DL_DIR) } catch { return 1 }
    if ((Free-Bytes $SUPPORT) -lt [long]($total * 2.8)) { return (Pack-Broken "there is not enough free disk space (it needs about $(Human ([long]($total * 2.8))))") }
    Dl-UiStart 'Reforge pack'
    $parts = @(Pack-Parts)
    # 1. check every link first, then download + check every part (a checked part is not fetched again)
    foreach ($p in $parts) {
        if ([IO.File]::Exists((Join-Path $script:DL_DIR ($p.Name + '.ok')))) { continue }
        if (-not (Dl-Precheck ($script:L_PACK_URL + $p.Name) 0 0 $p.Bytes)) { return (Pack-Broken $script:PC_WHY) }
        if ($script:DL_CANCEL) { Dl-UiEnd; return 130 }
    }
    foreach ($p in $parts) {
        $f = Join-Path $script:DL_DIR $p.Name
        if ([IO.File]::Exists($f + '.ok') -and [IO.File]::Exists($f)) { $done += $p.Bytes; continue }
        if ((File-Size $f) -gt $p.Bytes) { [IO.File]::Delete($f) }
        Log "reforge pack: downloading $($script:L_PACK_URL)$($p.Name)"
        if (-not (Dl-Precheck ($script:L_PACK_URL + $p.Name) 0 0 $p.Bytes $true)) { return (Pack-Broken $script:PC_WHY) }
        $script:DL_WHAT = 'Downloading the Reforge pack'; $script:DL_BASE = $done; $script:DL_TOTAL = $total
        if ((File-Size $f) -lt $p.Bytes) {
            $rc = Dl-Fetch ($script:L_PACK_URL + $p.Name) $f
            if ($rc -eq 130) { Dl-UiEnd; return 130 }
            if ($rc -ne 0) { return (Pack-Broken $script:DL_WHY) }
        }
        Dl-Status ([int](($done + $p.Bytes) * 90 / $total)) "Checking $($p.Name)..."
        $got = Get-Sha256 $f
        if ($script:DL_CANCEL) { Dl-UiEnd; return 130 }
        if ($got -ne $p.Sum) { return (Pack-Broken "a downloaded part is damaged or not the expected file ($($p.Name))") }
        [IO.File]::WriteAllText($f + '.ok', ''); $done += $p.Bytes
    }
    # 2. unpack next to the download, rebuild the shared files and check them, then move into place
    Dl-Status 91 'Unpacking...'
    $unpack = Join-Path $script:DL_DIR 'unpack'
    Remove-Tree $unpack
    foreach ($p in $parts) {
        $rc = Expand-Zip (Join-Path $script:DL_DIR $p.Name) $unpack
        if ($rc -eq 130) { Remove-Tree $unpack; Dl-UiEnd; return 130 }
        if ($rc -ne 0) { return (Pack-Broken "a downloaded part does not unpack ($($p.Name))") }
    }
    $new = Join-Path $unpack 'reforge-pack'
    if (-not [IO.File]::Exists((Join-Path $new 'install-links.sh'))) { return (Pack-Broken 'the download is not a Reforge pack') }
    Dl-Status 95 'Setting up the pack (about a minute)...'
    $ok = Install-PackLinks $new $true
    if ($script:DL_CANCEL) { Remove-Tree $unpack; Dl-UiEnd; return 130 }
    if (-not $ok) { return (Pack-Broken "the pack's files did not check out") }
    [IO.File]::WriteAllText((Join-Path $new 'PACK-VERSION'), (Conf1 'version') + "`n")
    try {
        if ((Test-Path -LiteralPath $PACK)) { Remove-Tree "$PACK.old"; Move-Dir $PACK "$PACK.old" }
    } catch { return (Pack-Broken 'the old pack folder could not be replaced') }
    try { Move-Dir $new $PACK } catch { return (Pack-Broken 'the pack folder could not be created') }
    Remove-Tree "$PACK.old"
    Dl-Status 100 'done'; Start-Sleep -Milliseconds 500; Dl-UiEnd; Remove-Tree $script:PACK_DL
    Log ("reforge pack: installed " + ([IO.File]::ReadAllText((Join-Path $PACK 'PACK-VERSION'))).Trim())
    return 0
}

# Offer the pack when none is installed. Never fatal: every failure starts the game with the 1998 look.
function Pack-Offer {
    if ([IO.Directory]::Exists((Join-Path $PACK 'hd-models'))) { return }
    if (-not [IO.File]::Exists($script:DL_CONF)) { return }
    if ([IO.File]::Exists($script:PACK_OFF) -and -not $env:UA_PACK_AUTO) { return }
    if (-not $script:LINKS_DONE) { Links-Load }
    if (-not $script:L_PACK_URL -or $script:L_PACK_URL -like '*PACK-URL-NOT-SET*') { Log 'reforge pack: none installed; this build has no download address'; return }
    $total = Pack-Total; $have = [long]0
    foreach ($p in @(Pack-Parts)) { $have += File-Size (Join-Path $script:PACK_DL $p.Name) }
    $answer = '' + $env:UA_PACK_AUTO
    if (-not $answer) {
        $verb = 'Download'
        if ($have -gt 0) {
            $msg = "The Reforge pack download stopped at $(Human $have) of $(Human $total). Resume it now?"; $verb = 'Resume download'
        } else {
            $msg = "Download the Reforge pack ($(Human $total))? It adds the new look: remastered units, buildings and skies, HD textures, restored videos and sharp menus. Without it the game keeps its 1998 look."
        }
        $msg += "`r`n`r`nIt goes to $SUPPORT. Your Urban Assault folder is not changed."
        $r = Ask $msg $verb @("Don't ask again", 'Not now', $verb)
        if ($r -eq "Don't ask again") { [IO.File]::WriteAllText($script:PACK_OFF, ''); Log 'reforge pack: offer turned off'; return }
        if ($r -eq $verb) { $answer = 'yes' } else { $answer = 'no' }
    }
    if ($answer -ne 'yes') { Log 'reforge pack: not now'; return }
    if ([IO.File]::Exists($script:PACK_OFF)) { [IO.File]::Delete($script:PACK_OFF) }
    $rc = Pack-Download
    if ($rc -eq 130) {
        Log 'reforge pack: download cancelled (the parts stay for a resume)'
        if (-not $env:UA_PACK_AUTO) { [void](Ask 'Download stopped. The game starts with the 1998 look; next time UA Reforged offers to resume the download.' 'OK' @('OK')) }
    } elseif ($rc -ne 0) {
        Log 'reforge pack: not installed (1998 look)'
    }
}

# First launch with no game folder: "I have the game" (the folder picker) or "Get the game".
# Sets GAME, or exits the launcher cleanly (nothing half set up: the next start asks again).
function Game-Choose {
    while (-not (Is-GameDir $script:GAME)) {
        $pick = '' + $env:UA_GAME_AUTO
        if (-not $pick) {
            $pick = Ask ("UA Reforged needs the original Urban Assault game files.`r`n`r`nDo you have the game on this PC? If not, UA Reforged can fetch the community OpenUA Standalone Package (about 445 MB) from the Metropolis Dawn community for you.") 'Get the game' @('Quit', 'I have the game - choose folder', 'Get the game')
        }
        switch -exact ($pick) {
            { $_ -eq 'get' -or $_ -eq 'Get the game' } {
                $r = Game-Get
                if ($r -eq 1 -and $env:UA_GAME_AUTO) { exit 0 }
                if ($r -eq 2) { Log 'stopped: help page opened, nothing set up'; exit 0 }
                if ($r -eq 3) { if (-not (Game-Pick) -and $env:UA_GAME_AUTO) { exit 0 } }
                break
            }
            'I have the game - choose folder' { [void](Game-Pick); break }
            default { Log 'stopped at the first-launch choice'; exit 0 }
        }
    }
}

function Game-Pick {
    if ($env:UA_NOTICE_AUTO) { Log 'folder picker (test run: skipped)'; exit 0 }
    $dlg = New-Object Windows.Forms.FolderBrowserDialog
    $dlg.Description = 'Choose your Urban Assault folder (it contains DATA and LEVELS). UA Reforged only reads it.'
    $dlg.ShowNewFolderButton = $false
    $owner = New-Object Windows.Forms.Form; $owner.TopMost = $true; $owner.ShowInTaskbar = $false
    $r = $dlg.ShowDialog($owner); $owner.Dispose()
    if ($r -ne [Windows.Forms.DialogResult]::OK) { return $false }
    $g = $dlg.SelectedPath.TrimEnd('\')
    if (Is-GameDir $g) { $script:GAME = $g; return $true }
    [void](Ask 'That folder has no DATA and LEVELS folders. Pick the folder of your Urban Assault install.' 'OK' @('OK'))
    return $false
}

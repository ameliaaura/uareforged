# Tests for the Windows launcher's new-version notice (packaging/windows/launcher/downloads.ps1: Test-LinksJson
# "latest", Update-Check, Compare-Version, Links-Prefetch). Windows PowerShell 5.1. No network: links.json comes from a
# file (UA_LINKS_FILE) or 127.0.0.1; no dialog is ever shown (Ask is replaced by a stub), no browser is opened.
#   powershell -NoProfile -ExecutionPolicy Bypass -File update_notice_test.ps1 -Launcher <folder with downloads.ps1>
#       [-Old <downloads.ps1 of an earlier release>,...]
# -Old: also runs that release's Test-LinksJson on the new links.json (it must accept the file and give the same
# lines as for the file without "latest"). Exit 0 = all passed.
param([string]$Launcher, [string[]]$Old = @())
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

$T = Join-Path ([IO.Path]::GetTempPath()) ('ua-update-test-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
[void][IO.Directory]::CreateDirectory("$T\fx")
$script:PASS = 0; $script:FAIL = 0
function Ok([string]$m) { $script:PASS++; Write-Output "ok   $m" }
function Bad([string]$m, [string]$logText = '') { $script:FAIL++; Write-Output "FAIL $m"; if ($logText) { $logText -split "`r?`n" | ForEach-Object { Write-Output "     | $_" } } }

# the launcher's script scope, as UA-Reforged.ps1 sets it up
$script:HERE = "$T\app"; [void][IO.Directory]::CreateDirectory($HERE)
Copy-Item (Join-Path $Launcher 'downloads.ps1') $HERE
$BASE = [IO.File]::ReadAllText((Join-Path $Launcher 'downloads.txt'))
$script:SUPPORT = "$T\sup"; $script:LOG = "$T\sup\launcher.log"; $script:INST = $T; $script:ICON = $null
[void][IO.Directory]::CreateDirectory($SUPPORT)
. (Join-Path $HERE 'downloads.ps1')
function Ask([string]$text, [string]$default, [string[]]$buttons) {
    Log ("DIALOG [{0}] buttons [{1}] default [{2}]" -f ($text -replace "`r?`n", ' '), ($buttons -join '|'), $default)
    if ($env:ASK_ANSWER) { return $env:ASK_ANSWER } return 'Later'
}
function Start-Process { Log "BROWSER $args" }   # never opens anything in a test

$REL = 'https://github.com/ameliaaura/uareforged/releases/tag'
$LB = '"game_url": "https://metropolisdawn.de/filebase/download/5/", "game_min_bytes": 100000000, "help_url": "https://uareforged.pages.dev/downloads/#nogame-h", "pack_url": "https://pub-9574664f67b04217aa2ca93f509638ad.r2.dev/pack/", "pack_parts": [{"name": "UA-Reforged-pack-0.8.0-1of2.zip", "bytes": 699598021, "sha256": "afb815b7de26a078097d9ef0fd2190f2434655232f4e903d3e822019b3eca98f"}], "quest_parts": [{"name": "UA-Reforged-Quest-pack-0.9.0.zip", "bytes": 712134306, "sha256": "e2b5447e831dfd75afa0ba1c0b193546e4fa9bce330860fb130faaa52f1dfb78"}], "message": "Test notice"'
function Fx([string]$name, [string]$extra) {
    $j = if ($extra) { "{ $LB, $extra }" } else { "{ $LB }" }
    [IO.File]::WriteAllText("$T\fx\$name.json", $j, (New-Object Text.UTF8Encoding($false)))
}
function Lat([string]$v, [string]$u = '') {
    if (-not $u) { $u = "$REL/v$v" }
    return ('"latest": {{"windows": {{"version": "{0}", "url": "{1}"}}, "mac": {{"version": "{0}", "url": "{2}/v{0}"}}}}' -f $v, $u, $REL)
}
Fx 'none' ''
foreach ($v in @('0.8.6', '0.8.5', '0.8.4', '0.8.4.1', '0.8.5.0', '0.8.7', '0.8.10')) { Fx ('v' + $v.Replace('.', '')) (Lat $v) }
Fx 'site' (Lat '0.8.6' 'https://uareforged.pages.dev/downloads/#windows-h')
Fx 'maconly' '"latest": {"mac": {"version": "0.9.0", "url": "https://uareforged.pages.dev/downloads/"}}'
Fx 'future' '"latest": {"windows": {"version": "0.8.6", "url": "https://uareforged.pages.dev/downloads/", "notes": "x"}, "linux": {"version": "1.0", "url": "https://uareforged.pages.dev/"}, "ios": 5}'
$mal = [ordered]@{
    m_vprefix = (Lat 'v0.8.6' "$REL/v0.8.6"); m_vbeta = (Lat '0.8.6-beta' "$REL/v0.8.6"); m_vone = (Lat '1' "$REL/v1")
    m_vfive = (Lat '0.8.6.1.2' "$REL/v0"); m_vlong = (Lat '0.8.1234567' "$REL/v0")
    m_vnum = '"latest": {"windows": {"version": 0.86, "url": "https://uareforged.pages.dev/"}}'
    m_vnl = '"latest": {"windows": {"version": "0.8.6\n", "url": "https://uareforged.pages.dev/"}}'
    m_urlnl = '"latest": {"windows": {"version": "0.8.6", "url": "https://uareforged.pages.dev/\n"}}'
    m_http = (Lat '0.8.6' 'http://uareforged.pages.dev/downloads/'); m_host = (Lat '0.8.6' 'https://evil.example/uareforged')
    m_repo = (Lat '0.8.6' 'https://github.com/ameliaaura/uareforged-evil/releases'); m_owner = (Lat '0.8.6' 'https://github.com/someone/uareforged')
    m_case = (Lat '0.8.6' 'https://GitHub.com/ameliaaura/uareforged')
    m_dots = (Lat '0.8.6' 'https://github.com/ameliaaura/uareforged/../../someone/x'); m_query = (Lat '0.8.6' 'https://uareforged.pages.dev/?x=1')
    m_port = (Lat '0.8.6' 'https://uareforged.pages.dev:8443/'); m_sub = (Lat '0.8.6' 'https://x.uareforged.pages.dev/')
    m_userinfo = (Lat '0.8.6' 'https://uareforged.pages.dev@evil.example/'); m_pct = (Lat '0.8.6' 'https://github.com/ameliaaura/uareforged/%2e%2e/x')
    m_backslash = '"latest": {"windows": {"version": "0.8.6", "url": "https://uareforged.pages.dev\\..\\x"}}'
    m_nourl = '"latest": {"windows": {"version": "0.8.6"}}'
    m_array = '"latest": [{"windows": "0.8.6"}]'; m_str = '"latest": "0.8.6"'; m_null = '"latest": null'
    m_winstr = '"latest": {"windows": "0.8.6"}'; m_winarr = '"latest": {"windows": ["0.8.6"]}'
}
foreach ($k in $mal.Keys) { Fx $k $mal[$k] }
[IO.File]::WriteAllText("$T\fx\badjson.json", '{ "latest": {"windows": {"version": "0.8.6"} }')

# Run-Start <name> <app version or ''> <env hashtable>: one start up to where the game would start; returns the log
function Run-Start([string]$name, [string]$ver, [hashtable]$envs, [string]$skip = '') {
    $txt = $BASE; if ($ver) { $txt += "`r`napp_version`t$ver`r`n" }
    [IO.File]::WriteAllText($script:DL_CONF, $txt)
    $script:SUPPORT = "$T\sup-$name"; [void][IO.Directory]::CreateDirectory($SUPPORT); $script:LOG = "$SUPPORT\launcher.log"
    if ($skip) { [IO.File]::WriteAllText("$SUPPORT\update-skip.txt", $skip) }
    $script:LINKS_DONE = $false; $script:LINKS_PRE = $null; $script:LINKS_PRE_SRC = ''; $script:L_LATEST = ''; $script:L_LATEST_URL = ''
    foreach ($n in @('UA_LINKS_FILE', 'UA_LINKS_URL', 'UA_LINKS_ALLOW_LOCAL', 'UA_UPDATE_AUTO', 'ASK_ANSWER')) { Remove-Item "Env:$n" -ErrorAction SilentlyContinue }
    foreach ($k in $envs.Keys) { Set-Item "Env:$k" $envs[$k] }
    if ($envs.ContainsKey('PREFETCH')) { Links-Prefetch ($envs['PREFETCH'] -eq 'start') }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Update-Check
    $sw.Stop()
    Log ('CHECK SECONDS ' + $sw.Elapsed.TotalSeconds.ToString('0.000', $script:INV))
    Log 'GAME STARTS'
    return [IO.File]::ReadAllText($LOG)
}
function Expect([string]$name, [string]$what, [string]$log, [string]$must, [string]$mustNot = '') {
    if ($log -cmatch $must -and $log -cmatch 'GAME STARTS' -and (-not $mustNot -or $log -cnotmatch $mustNot)) { Ok "${name}: $what" } else { Bad "${name}: $what" $log }
}
function F([string]$n) { return @{ UA_LINKS_FILE = "$T\fx\$n.json" } }
function FA([string]$n, [string]$k, [string]$v) { $h = F $n; $h[$k] = $v; return $h }

# --- versions
$l = Run-Start 'newer' '0.8.5' (F 'v086')
Expect 'newer' '0.8.6 > 0.8.5: one dialog, Later' $l 'DIALOG \[A new version of UA Reforged \(0\.8\.6\) is out\.' 'update page|BROWSER'
if ($l -match "buttons \[Don't remind me for this version\|Later\|Download\] default \[Download\]") { Ok 'newer: buttons + default' } else { Bad 'newer: buttons' $l }
if ($l -match 'You have version 0\.8\.5') { Ok 'newer: names this version' } else { Bad 'newer: names this version' $l }
if (-not [IO.File]::Exists("$T\sup-newer\update-skip.txt")) { Ok 'newer: Later stores nothing' } else { Bad 'newer: Later stored a skip' }
if ($l -match 'links: .* accepted') { Ok 'newer: links.json accepted' } else { Bad 'newer: links accepted' $l }
Expect 'same' '0.8.5 = 0.8.5: no dialog' (Run-Start 'same' '0.8.5' (F 'v085')) 'up to date \(latest 0\.8\.5\)' 'DIALOG'
Expect 'older' '0.8.4 < 0.8.5: no dialog' (Run-Start 'older' '0.8.5' (F 'v084')) 'up to date \(latest 0\.8\.4\)' 'DIALOG'
Expect 'p0850' '0.8.5.0 = 0.8.5: no dialog' (Run-Start 'p0850' '0.8.5' (F 'v0850')) 'up to date' 'DIALOG'
Expect 'p4a' '0.8.5 > 0.8.4.1: dialog' (Run-Start 'p4a' '0.8.4.1' (F 'v085')) 'DIALOG \[A new version of UA Reforged \(0\.8\.5\)'
Expect 'p4b' '0.8.4 < 0.8.4.1: no dialog' (Run-Start 'p4b' '0.8.4.1' (F 'v084')) 'up to date' 'DIALOG'
Expect 'p4c' '0.8.4.1 > 0.8.4: dialog' (Run-Start 'p4c' '0.8.4' (F 'v0841')) 'DIALOG \[A new version of UA Reforged \(0\.8\.4\.1\)'
Expect 'p4d' '0.8.4.1 = 0.8.4.1: no dialog' (Run-Start 'p4d' '0.8.4.1' (F 'v0841')) 'up to date' 'DIALOG'
Expect 'p10' '0.8.10 > 0.8.9: dialog' (Run-Start 'p10' '0.8.9' (F 'v0810')) 'DIALOG \[A new version of UA Reforged \(0\.8\.10\)'
Expect 'site' 'uareforged.pages.dev URL; test Download logs it' (Run-Start 'site' '0.8.5' (FA 'site' 'UA_UPDATE_AUTO' 'download')) 'update page: https://uareforged\.pages\.dev/downloads/#windows-h \(test run' 'BROWSER'
Expect 'future' 'unknown platforms + extra keys: tolerated' (Run-Start 'future' '0.8.5' (F 'future')) 'DIALOG \[A new version of UA Reforged \(0\.8\.6\)'
Expect 'maconly' 'no windows entry: no dialog' (Run-Start 'maconly' '0.8.5' (F 'maconly')) 'no version information' 'DIALOG|ignored'
Expect 'none' "no latest field (today's site): no dialog" (Run-Start 'none' '0.8.5' (F 'none')) 'no version information' 'DIALOG'
Expect 'nover' 'build without app_version: no dialog' (Run-Start 'nover' '' (F 'v086')) 'this build has no version' 'DIALOG'

# --- answers
Expect 'dl' 'test answer download: logged, not opened' (Run-Start 'dl' '0.8.5' (FA 'v086' 'UA_UPDATE_AUTO' 'download')) 'update page: https://github\.com/ameliaaura/uareforged/releases/tag/v0\.8\.6 \(test run: not opened\)' 'DIALOG|BROWSER'
Expect 'dl2' 'dialog Download: opens the page (stubbed)' (Run-Start 'dl2' '0.8.5' (FA 'v086' 'ASK_ANSWER' 'Download')) 'BROWSER https://github\.com/ameliaaura/uareforged/releases/tag/v0\.8\.6'
Expect 'later' 'test answer later: nothing kept' (Run-Start 'later' '0.8.5' (FA 'v086' 'UA_UPDATE_AUTO' 'later')) 'test answer later' 'update page|no reminder|BROWSER'
$null = Run-Start 'skip1' '0.8.5' (FA 'v086' 'UA_UPDATE_AUTO' 'skip')
if (([IO.File]::ReadAllText("$T\sup-skip1\update-skip.txt")).Trim() -eq '0.8.6') { Ok 'skip: update-skip.txt = 0.8.6' } else { Bad 'skip: update-skip.txt' }
Expect 'skip2' 'skipped 0.8.6, site 0.8.6: no dialog' (Run-Start 'skip2' '0.8.5' (F 'v086') "0.8.6`r`n") 'not reminding \(skipped\)' 'DIALOG'
Expect 'skip3' 'skipped 0.8.6, site 0.8.7: asks again' (Run-Start 'skip3' '0.8.5' (F 'v087') "0.8.6`r`n") 'DIALOG \[A new version of UA Reforged \(0\.8\.7\)'
Expect 'skip4' 'junk skip file: ignored, asks' (Run-Start 'skip4' '0.8.5' (F 'v086') "junk`r`n") 'DIALOG'
$null = Run-Start 'skip5' '0.8.5' (FA 'v086' 'ASK_ANSWER' "Don't remind me for this version")
if ([IO.File]::Exists("$T\sup-skip5\update-skip.txt") -and ([IO.File]::ReadAllText("$T\sup-skip5\update-skip.txt")).Trim() -eq '0.8.6') { Ok 'skip via the dialog button' } else { Bad 'skip via the dialog button' }

# --- malformed: only "latest" is dropped, the rest of links.json still counts, never a dialog
foreach ($k in $mal.Keys) {
    $l = Run-Start $k '0.8.5' (F $k)
    Expect $k 'malformed latest ignored, links still accepted' $l '\(ignored; no new-version notice\)' 'DIALOG|rejected'
    if ($l -notmatch 'links: .* accepted') { Bad "${k}: links.json no longer accepted" $l }
}
Expect 'badjson' 'bad JSON: rejected as before, no dialog' (Run-Start 'badjson' '0.8.5' (F 'badjson')) 'rejected \(not valid JSON\)' 'DIALOG'
Expect 'nofile' 'no file: built-in links, no dialog' (Run-Start 'nofile' '0.8.5' @{ UA_LINKS_FILE = "$T\fx\missing.json" }) 'not reachable' 'DIALOG'

# --- the real background read (Links-Prefetch -> curl.exe), against 127.0.0.1
$port = Get-Random -Minimum 20000 -Maximum 40000
Add-Type -TypeDefinition @'
using System; using System.Collections.Generic; using System.Net; using System.Net.Sockets; using System.Text; using System.Threading;
public class UaTinyHttp {
    TcpListener l; byte[] body; bool answer; List<TcpClient> held = new List<TcpClient>();
    public List<string> Reqs = new List<string>();
    public UaTinyHttp(int port, byte[] b, bool a) { l = new TcpListener(IPAddress.Loopback, port); body = b; answer = a; }
    public void Start() { l.Start(); Thread t = new Thread(Loop); t.IsBackground = true; t.Start(); }
    void Loop() {
        for (;;) {
            TcpClient c; try { c = l.AcceptTcpClient(); } catch { return; }
            if (!answer) { lock (held) held.Add(c); continue; }
            try {
                NetworkStream s = c.GetStream(); StringBuilder sb = new StringBuilder(); byte[] buf = new byte[1];
                while (!sb.ToString().EndsWith("\r\n\r\n") && s.Read(buf, 0, 1) == 1) sb.Append((char)buf[0]);
                string[] first = sb.ToString().Split(' ');
                lock (Reqs) Reqs.Add(first.Length > 1 ? first[1] : "?");
                byte[] h = Encoding.ASCII.GetBytes("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + body.Length + "\r\nConnection: close\r\n\r\n");
                s.Write(h, 0, h.Length); s.Write(body, 0, body.Length); s.Flush(); c.Close();
            } catch { }
        }
    }
    public void Stop() { l.Stop(); lock (held) foreach (TcpClient c in held) c.Close(); }
}
'@
$srv = New-Object UaTinyHttp($port, [IO.File]::ReadAllBytes("$T\fx\v086.json"), $true); $srv.Start()
Expect 'net' 'background read from a server: notice' (Run-Start 'net' '0.8.5' @{ UA_LINKS_URL = "http://127.0.0.1:$port/links.json"; UA_LINKS_ALLOW_LOCAL = '1'; UA_UPDATE_AUTO = 'later'; PREFETCH = 'setup' }) 'is out \(this app 0\.8\.5\); test answer later'
Expect 'net2' 'game + pack in place: read with ?start, notice' (Run-Start 'net2' '0.8.5' @{ UA_LINKS_URL = "http://127.0.0.1:$port/links.json"; UA_LINKS_ALLOW_LOCAL = '1'; UA_UPDATE_AUTO = 'later'; PREFETCH = 'start' }) 'links\.json\?start accepted'
$srv.Stop()
if (($srv.Reqs -join ',') -eq '/links.json,/links.json?start') { Ok "requests: $($srv.Reqs -join ', ') (one per start)" } else { Bad "requests: $($srv.Reqs -join ', ')" }
$l = Run-Start 'offline' '0.8.5' @{ UA_LINKS_URL = 'http://127.0.0.1:9/links.json'; UA_LINKS_ALLOW_LOCAL = '1'; PREFETCH = 'setup' }
# (Windows retries a refused connection for about 2 s, so the read may still be going when the check gives up)
Expect 'offline' 'offline (refused): no dialog' $l 'not reachable; built-in links used|slow to answer; skipped' 'DIALOG'
$s = [double]::Parse(([regex]::Match($l, 'CHECK SECONDS ([0-9.]+)')).Groups[1].Value, $script:INV)
if ($s -lt 2.5) { Ok "offline: check took ${s}s" } else { Bad "offline: check took ${s}s" $l }
# a server that accepts and never answers
$slow = New-Object UaTinyHttp(($port + 1), [byte[]]@(), $false); $slow.Start()
$l = Run-Start 'slow' '0.8.5' @{ UA_LINKS_URL = "http://127.0.0.1:$($port + 1)/links.json"; UA_LINKS_ALLOW_LOCAL = '1'; PREFETCH = 'setup' }
Expect 'slow' 'server never answers: skipped, no dialog' $l 'slow to answer; skipped' 'DIALOG'
$s = [double]::Parse(([regex]::Match($l, 'CHECK SECONDS ([0-9.]+)')).Groups[1].Value, $script:INV)
if ($s -lt 2.6) { Ok "slow: gave up after ${s}s" } else { Bad "slow: took ${s}s" $l }
Start-Sleep -Milliseconds 300
$left = @(Get-CimInstance Win32_Process -Filter "Name = 'curl.exe'" | Where-Object { $_.CommandLine -like "*127.0.0.1:$($port + 1)*" })
if ($left.Count -eq 0) { Ok 'slow: the read was stopped (no curl left)' } else { Bad 'slow: curl still running'; $left | ForEach-Object { Stop-Process -Id $_.ProcessId -Force } }
$slow.Stop()

# --- Compare-Version
foreach ($c in @('0.8.5 0.8.4.1 1', '0.8.4.1 0.8.4 1', '0.8.4 0.8.4.1 -1', '0.8.4 0.8.4.0 0', '0.8.10 0.8.9 1', '1.0 0.9.9.9 1', '0.8.05 0.8.5 0', '0.9 0.8.99 1', '0.8.4.2 0.8.4.10 -1')) {
    $p = $c.Split(' '); $r = Compare-Version $p[0] $p[1]
    if ([string]$r -eq $p[2]) { Ok "Compare-Version $($p[0]) $($p[1]) = $($p[2])" } else { Bad "Compare-Version $($p[0]) $($p[1]) = $r (want $($p[2]))" }
}

# --- released validators on the new links.json
$allow = @(Conf 'allow_host')
$Old = @($Old | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().TrimEnd(';') } | Where-Object { $_ })   # (-File passes "a,b" as one string)
foreach ($o in $Old) {
    $src = [IO.File]::ReadAllText($o)
    $m = [regex]::Match($src, '(?s)function Test-LinksJson.*?\r?\n}\r?\n')
    if (-not $m.Success) { Bad "old validator ${o}: Test-LinksJson not found"; continue }
    Invoke-Expression ($m.Value -replace 'function Test-LinksJson', 'function Test-LinksJsonOld')
    $a = (Test-LinksJsonOld "$T\fx\none.json" $allow $false) -join "`n"
    foreach ($f in @('v086', 'future', 'm_array', 'm_vnum', 'm_winstr')) {
        try {
            $b = (Test-LinksJsonOld "$T\fx\$f.json" $allow $false) -join "`n"
            if ($a -eq $b -and $a) { Ok "old validator $o accepts $f.json, same lines as without latest" } else { Bad "old validator $o on ${f}.json: different lines" ($b) }
        } catch { Bad "old validator $o REJECTS ${f}.json: $_" }
    }
}

Write-Output "update notice tests: $($script:PASS) passed, $($script:FAIL) failed"
try { Remove-Item -Recurse -Force $T } catch { }
if ($script:FAIL -eq 0) { exit 0 } else { exit 1 }

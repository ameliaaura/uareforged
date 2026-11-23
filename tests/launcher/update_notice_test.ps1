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
function Ask-Ex([string]$heading, [string]$text, [string]$default, [string[]]$buttons, [string]$checkText, [bool]$checkOn, [string]$menuLabel, [string[]]$menuItems, [int]$menuSel) {
    Log ("DIALOG [{0} {1}] buttons [{2}] default [{3}] link [{4}]" -f $heading, ($text -replace "`r?`n", ' '), ($buttons -join '|'), $default, $script:AskLinkText)
    $b = if ($env:ASK_ANSWER) { $env:ASK_ANSWER } else { 'Later' }
    return @{ Button = $b; Check = $false; Index = 0 }
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
$script:KEYLINES = ''   # update_key lines for this run (set by the one-click update tests)
function Run-Start([string]$name, [string]$ver, [hashtable]$envs, [string]$skip = '') {
    $txt = (($BASE -split "`r?`n") | Where-Object { $_ -notmatch '^update_key' }) -join "`r`n"
    if ($ver) { $txt += "`r`napp_version`t$ver`r`n" }
    if ($script:KEYLINES) { $txt += $script:KEYLINES }
    [IO.File]::WriteAllText($script:DL_CONF, $txt)
    $script:SUPPORT = "$T\sup-$name"; [void][IO.Directory]::CreateDirectory($SUPPORT); $script:LOG = "$SUPPORT\launcher.log"
    if ($skip) { [IO.File]::WriteAllText("$SUPPORT\update-skip.txt", $skip) }
    $script:LINKS_DONE = $false; $script:LINKS_PRE = $null; $script:LINKS_PRE_SRC = ''; $script:L_LATEST = ''; $script:L_LATEST_URL = ''
    $script:UPDATE_HANDED = $false; $script:DL_DIR = ''
    foreach ($n in @('UA_LINKS_FILE', 'UA_LINKS_URL', 'UA_LINKS_ALLOW_LOCAL', 'UA_UPDATE_AUTO', 'ASK_ANSWER')) { Remove-Item "Env:$n" -ErrorAction SilentlyContinue }
    foreach ($k in $envs.Keys) { Set-Item "Env:$k" $envs[$k] }
    if ($envs.ContainsKey('PREFETCH')) { Links-Prefetch ($envs['PREFETCH'] -eq 'start') }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Update-Check
    $sw.Stop()
    Log ('CHECK SECONDS ' + $sw.Elapsed.TotalSeconds.ToString('0.000', $script:INV))
    if ($script:UPDATE_HANDED) { Log 'LAUNCHER STOPS' } else { Log 'GAME STARTS' }
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

# --- one-click update (signed manifest + installer), against 127.0.0.1; keys made here (never the release key)
function New-TestKey {
    $k = [Security.Cryptography.CngKey]::Create([Security.Cryptography.CngAlgorithm]::ECDsaP256)   # ephemeral
    $blob = $k.Export([Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
    $spki = [byte[]](0x30,0x59,0x30,0x13,0x06,0x07,0x2a,0x86,0x48,0xce,0x3d,0x02,0x01,0x06,0x08,0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07,0x03,0x42,0x00,0x04) + $blob[8..71]
    return @{ Key = $k; Pub = [Convert]::ToBase64String([byte[]]$spki) }
}
function DerInt([byte[]]$b) { $i = 0; while ($i -lt 31 -and $b[$i] -eq 0) { $i++ }; $v = [byte[]]$b[$i..31]; if ($v[0] -ge 0x80) { $v = [byte[]](@(0) + $v) }; return [byte[]](@(2, $v.Length) + $v) }
function Sign-Bytes($key, [byte[]]$data) {
    $ec = New-Object Security.Cryptography.ECDsaCng($key.Key); $ec.HashAlgorithm = [Security.Cryptography.CngAlgorithm]::Sha256
    $raw = $ec.SignData($data); $ec.Dispose()
    $body = [byte[]]((DerInt ([byte[]]$raw[0..31])) + (DerInt ([byte[]]$raw[32..63])))
    return [byte[]](@(0x30, $body.Length) + $body)
}
$KREL = New-TestKey; $KTEST = New-TestKey; $KOTHER = New-TestKey
$S = "$T\srv"; [void][IO.Directory]::CreateDirectory($S)
$EXE = 'UA-Reforged-0.8.6-Windows-Setup.exe'
Copy-Item "$env:SystemRoot\System32\whoami.exe" "$S\$EXE"
Copy-Item "$env:SystemRoot\System32\whoami.exe" "$S\good-copy.exe"
$tb = [IO.File]::ReadAllBytes("$S\$EXE"); $tb[$tb.Length - 1] = $tb[$tb.Length - 1] -bxor 0xFF; [IO.File]::WriteAllBytes("$S\tampered.exe", $tb)
Add-Type -TypeDefinition @'
using System; using System.IO; using System.Net; using System.Net.Sockets; using System.Text; using System.Threading; using System.Collections.Generic;
public class UaFileHttp {
    TcpListener l; string dir; public List<string> Reqs = new List<string>();
    public UaFileHttp(int port, string d) { l = new TcpListener(IPAddress.Loopback, port); dir = d; }
    public void Start() { l.Start(); Thread t = new Thread(Loop); t.IsBackground = true; t.Start(); }
    void Loop() { for (;;) { TcpClient c; try { c = l.AcceptTcpClient(); } catch { return; } ThreadPool.QueueUserWorkItem(delegate { Serve(c); }); } }
    void Serve(TcpClient c) {
        try {
            NetworkStream s = c.GetStream(); StringBuilder sb = new StringBuilder(); byte[] one = new byte[1];
            while (!sb.ToString().EndsWith("\r\n\r\n") && s.Read(one, 0, 1) == 1) sb.Append((char)one[0]);
            string req = sb.ToString(); string path = req.Split(' ')[1]; bool range = req.IndexOf("\r\nRange:", StringComparison.OrdinalIgnoreCase) >= 0;
            lock (Reqs) Reqs.Add(path);
            bool trunc = path.StartsWith("/trunc/"); string f = Path.Combine(dir, trunc ? path.Substring(7) : path.Substring(1));
            if (!File.Exists(f)) { byte[] nf = Encoding.ASCII.GetBytes("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); s.Write(nf, 0, nf.Length); c.Close(); return; }
            byte[] d = File.ReadAllBytes(f); string h; int n = d.Length;
            if (range) { n = Math.Min(1024, d.Length); h = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-" + (n - 1) + "/" + d.Length + "\r\nContent-Length: " + n; }
            else { h = "HTTP/1.1 200 OK\r\nContent-Length: " + d.Length; if (trunc) n = d.Length / 2; }
            byte[] hb = Encoding.ASCII.GetBytes(h + "\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\n");
            s.Write(hb, 0, hb.Length); s.Write(d, 0, n); s.Flush(); c.Close();
        } catch { try { c.Close(); } catch { } }
    }
    public void Stop() { l.Stop(); }
}
'@
$uport = Get-Random -Minimum 40001 -Maximum 50000
$fsrv = New-Object UaFileHttp($uport, $S); $fsrv.Start()
function USign($key, [string]$name, [string]$ver, [string]$plat, [string]$file, [string]$urlName = '') {
    $b = [IO.File]::ReadAllBytes("$S\$file")
    $sha = (([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($b))) -replace '-', '').ToLowerInvariant()
    $man = [Text.Encoding]::UTF8.GetBytes("UA Reforged update 1`nname`t$name`nversion`t$ver`nplatform`t$plat`nbytes`t$($b.Length)`nsha256`t$sha`n")
    if (-not $urlName) { $urlName = $name }
    return ('{{"url": "http://127.0.0.1:{0}/{1}", "manifest": "{2}", "sig": "{3}"}}' -f $uport, $urlName, [Convert]::ToBase64String($man), [Convert]::ToBase64String((Sign-Bytes $key $man)))
}
function UFx([string]$name, [string]$ver, [string]$upd) { Fx $name ('"latest": {"windows": {"version": "' + $ver + '", "url": "https://uareforged.pages.dev/downloads/", "update": ' + $upd + '}}') }
UFx 'u_good' '0.8.6' (USign $KREL $EXE '0.8.6' 'windows' $EXE)
UFx 'u_badkey' '0.8.6' (USign $KOTHER $EXE '0.8.6' 'windows' $EXE)
UFx 'u_testkey' '0.8.6' (USign $KTEST $EXE '0.8.6' 'windows' $EXE)
UFx 'u_mac' '0.8.6' (USign $KREL $EXE '0.8.6' 'mac' $EXE)
UFx 'u_replay' '0.8.6' (USign $KREL $EXE '0.8.5' 'windows' $EXE)
UFx 'u_mismatch' '0.8.7' (USign $KREL $EXE '0.8.6' 'windows' $EXE)
UFx 'u_tamper' '0.8.6' (USign $KREL 'tampered.exe' '0.8.6' 'windows' $EXE)
UFx 'u_tamper2' '0.8.6' (USign $KOTHER 'tampered.exe' '0.8.6' 'windows' 'tampered.exe')
UFx 'u_urlname' '0.8.6' (USign $KREL $EXE '0.8.6' 'windows' $EXE 'good-copy.exe')
UFx 'u_trunc' '0.8.6' (USign $KREL $EXE '0.8.6' 'windows' $EXE "trunc/$EXE")
$good = USign $KREL $EXE '0.8.6' 'windows' $EXE
UFx 'u_junksig' '0.8.6' ($good -replace '"sig": "[^"]*"', '"sig": "MEUCIQDjunkjunkjunkjunkjunkjunkjunkjunkjunkjunkjunkAAAA"')
UFx 'u_nosig' '0.8.6' ($good -replace ', "sig": "[^"]*"', '')
UFx 'u_hostbad' '0.8.6' ($good -replace 'http://127\.0\.0\.1:[0-9]+/', 'https://evil.example/')
UFx 'u_notexe' '0.8.6' ($good -replace '\.exe"', '.msi"')
UFx 'u_manjunk' '0.8.6' ($good -replace '"manifest": "[^"]*"', '"manifest": "not base64!"')
# the hand-over: a stand-in update.ps1 notes how it was started (the real one is tested end to end on the PC)
[IO.File]::WriteAllText("$HERE\update.ps1", 'param([string]$Setup, [string]$Inst, [string]$Version, [string]$Page, [string]$Support, [string]$WaitPids) [IO.File]::WriteAllText("$Support\runner.txt", "$Setup|$Inst|$Version|$Page|$WaitPids")')
$script:INST = "$T\inst"; [void][IO.Directory]::CreateDirectory($INST); [IO.File]::WriteAllText("$INST\Uninstall.exe", 'x')
$REL1 = "`r`nupdate_key`t$($KREL.Pub)`r`n"
$script:KEYLINES = $REL1
function URun([string]$name, [string]$fx, [hashtable]$more = @{}) {
    $h = @{ UA_LINKS_FILE = "$T\fx\$fx.json"; UA_LINKS_ALLOW_LOCAL = '1'; UA_UPDATE_AUTO = 'update'; UA_PACK_NOUI = '1' }
    foreach ($k in $more.Keys) { $h[$k] = $more[$k] }
    return (Run-Start $name '0.8.5' $h)
}
function Refused([string]$name, [string]$fx, [string]$why, [string]$what) {
    $l = URun $name $fx
    $clean = -not [IO.Directory]::Exists("$T\sup-$name\update-download") -and -not [IO.File]::Exists("$T\sup-$name\runner.txt")
    if ($l -cmatch "update: not installed - .*($why)" -and $l -cmatch 'notice: UA Reforged could not update itself' -and $l -cmatch 'update page: https://uareforged\.pages\.dev/downloads/ \(test run' -and $l -cmatch 'GAME STARTS' -and $clean) { Ok "${name}: $what" } else { Bad "${name}: $what" $l }
}
$l = Run-Start 'udlg' '0.8.5' @{ UA_LINKS_FILE = "$T\fx\u_good.json"; UA_LINKS_ALLOW_LOCAL = '1' }
if ($l -match "buttons \[Don't remind me for this version\|Later\|Update now\] default \[Update now\] link \[What's new in 0\.8\.6\]" -and $l -match 'Update now downloads it, checks it and starts the new version' -and $l -match 'GAME STARTS') { Ok "dialog: Don't remind / Later / Update now + What's new link (Later: the game starts)" } else { Bad 'dialog with update' $l }
$l = URun 'ugood' 'u_good'
$rf = "$T\sup-ugood\runner.txt"; for ($i = 0; $i -lt 100 -and -not [IO.File]::Exists($rf); $i++) { Start-Sleep -Milliseconds 100 }
$ra = if ([IO.File]::Exists($rf)) { [IO.File]::ReadAllText($rf) } else { '' }
if ($l -match 'update: signature and manifest ok' -and $l -match 'the installer runs once this launcher has closed' -and $l -match 'LAUNCHER STOPS' -and $l -notmatch 'GAME STARTS') { Ok 'good: downloaded, checked, handed over, the launcher stops' } else { Bad 'good: flow' $l }
$p = $ra.Split('|')
if ($p.Count -ge 5 -and $p[0] -eq "$T\sup-ugood\update-download\$EXE" -and $p[1] -eq $INST -and $p[2] -eq '0.8.6' -and $p[4] -match "^$PID,") { Ok "good: update.ps1 started with the checked installer, this folder, 0.8.6, waits for this launcher" } else { Bad "good: runner args [$ra]" }
if ([IO.File]::Exists("$T\sup-ugood\update-download\$EXE") -and (Get-Sha256 "$T\sup-ugood\update-download\$EXE") -eq (Get-Sha256 "$S\$EXE")) { Ok 'good: the installer is in the download folder, same bytes' } else { Bad 'good: downloaded file' }
if ($fsrv.Reqs -contains "/$EXE") { Ok 'good: fetched from the server' } else { Bad "good: requests $($fsrv.Reqs -join ',')" }
Refused 'ubadkey' 'u_badkey' 'signature is not valid' 'signed with an unknown key: refused'
Refused 'utestkey' 'u_testkey' 'signature is not valid' 'signed with the TEST key, release build (no update_key_test): refused'
$script:KEYLINES = $REL1 + "update_key_test`t$($KTEST.Pub)`r`n"
$l = URun 'utestok' 'u_testkey'
if ($l -match 'LAUNCHER STOPS') { Ok 'testok: a test-hook build (update_key_test) takes the test-signed update' } else { Bad 'testok' $l }
$script:KEYLINES = $REL1
Refused 'ujunksig' 'u_junksig' 'signature is not valid' 'garbage signature: refused'
Refused 'umac' 'u_mac' 'for another system \(mac\)' 'manifest for the Mac: refused'
Refused 'ureplay' 'u_replay' 'not newer than this version' 'replay of a 0.8.5 manifest (downgrade): refused'
Refused 'umismatch' 'u_mismatch' 'not the version the site names' 'manifest 0.8.6, site says 0.8.7: refused'
Refused 'utamper' 'u_tamper' 'does not match its signature|not the expected' 'tampered file, original signed manifest: refused'
Refused 'utamper2' 'u_tamper2' 'signature is not valid' 'tampered file with a manifest that matches it, unknown key: refused'
Refused 'uurlname' 'u_urlname' 'address does not match' 'url names another file than the manifest: refused'
Refused 'utrunc' 'u_trunc' 'connection dropped|download failed|not the expected' 'interrupted download (half the file, connection dropped): refused'
foreach ($m in @('u_nosig', 'u_hostbad', 'u_notexe', 'u_manjunk')) {
    $l = Run-Start $m '0.8.5' @{ UA_LINKS_FILE = "$T\fx\$m.json"; UA_LINKS_ALLOW_LOCAL = '1' }
    if ($l -match '\(ignored; no one-click update\)' -and $l -match 'buttons \[[^\]]*\|Download\] default \[Download\]' -and $l -match 'GAME STARTS') { Ok "${m}: update dropped, notice offers Download" } else { Bad $m $l }
}
# a copy unpacked from the zip (no Uninstall.exe) and a program folder it may not write to
[IO.File]::Delete("$INST\Uninstall.exe")
$l = URun 'uzip' 'u_good'
if ($l -match 'unpacked from the zip' -and $l -match 'update page: .*\(test run' -and $l -match 'GAME STARTS') { Ok 'zip copy: explained, download page, nothing downloaded' } else { Bad 'zip copy' $l }
[IO.File]::WriteAllText("$INST\Uninstall.exe", 'x')
$me = [Security.Principal.WindowsIdentity]::GetCurrent().Name
& icacls.exe $INST /deny "${me}:(W)" | Out-Null
$l = URun 'uro' 'u_good'
& icacls.exe $INST /remove:d $me | Out-Null
if ($l -match 'may not change its own program folder' -and $l -match 'GAME STARTS') { Ok 'read-only program folder: explained, download page' } else { Bad 'read-only folder' $l }
[void][IO.Directory]::CreateDirectory("$T\sup-ufailed"); [IO.File]::WriteAllText("$T\sup-ufailed\update-failed.txt", '0.8.6')
$l = URun 'ufailed' 'u_good'
if ($l -match 'did not install just now; not asking again this start' -and $l -match 'GAME STARTS' -and -not [IO.File]::Exists("$T\sup-ufailed\update-failed.txt")) { Ok 'just failed (update-failed.txt): not offered again this start, note removed' } else { Bad 'ufailed' $l }
$l = Run-Start 'unoupd' '0.8.5' @{ UA_LINKS_FILE = "$T\fx\v086.json"; UA_UPDATE_AUTO = 'update' }
if ($l -match 'update page: .*\(test run' -and $l -match 'GAME STARTS') { Ok 'no update block + update answer: download page' } else { Bad 'unoupd' $l }
# Test-UpdateSig on its own: a signature of other bytes, a truncated DER, the wrong key
$m1 = [Text.Encoding]::UTF8.GetBytes('abc'); $s1 = Sign-Bytes $KREL $m1
if ((Test-UpdateSig $m1 $s1 $KREL.Pub) -and -not (Test-UpdateSig ([Text.Encoding]::UTF8.GetBytes('abd')) $s1 $KREL.Pub) -and -not (Test-UpdateSig $m1 ([byte[]]$s1[0..($s1.Length - 2)]) $KREL.Pub) -and -not (Test-UpdateSig $m1 $s1 $KOTHER.Pub)) { Ok 'Test-UpdateSig: right key ok; other bytes / cut signature / other key refused' } else { Bad 'Test-UpdateSig' }
$fsrv.Stop()
$script:KEYLINES = ''
Fx 'v086u' ('"latest": {"windows": {"version": "0.8.6", "url": "https://uareforged.pages.dev/downloads/", "update": ' + $good + '}}')

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
    foreach ($f in @('v086', 'future', 'm_array', 'm_vnum', 'm_winstr', 'v086u')) {
        try {
            $b = (Test-LinksJsonOld "$T\fx\$f.json" $allow $false) -join "`n"
            if ($a -eq $b -and $a) { Ok "old validator $o accepts $f.json, same lines as without latest" } else { Bad "old validator $o on ${f}.json: different lines" ($b) }
        } catch { Bad "old validator $o REJECTS ${f}.json: $_" }
    }
}

Write-Output "update notice tests: $($script:PASS) passed, $($script:FAIL) failed"
try { Remove-Item -Recurse -Force $T } catch { }
if ($script:FAIL -eq 0) { exit 0 } else { exit 1 }

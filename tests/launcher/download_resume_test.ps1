# Tests for the Windows launcher downloads carrying on where they stopped (packaging/windows/launcher/downloads.ps1:
# Dl-Precheck, Dl-Fetch, Dl-Again, Game-Get, Pack-Get). The Windows twin of download_resume_test.sh: a local server on
# 127.0.0.1 drops connections half way, ends transfers early, stalls, ignores or refuses Range requests, sends a damaged
# file and serves a new version of a file; downloads are stopped and the launcher "restarted". Windows PowerShell 5.1,
# curl.exe (Windows 10 1803+). No dialog is ever shown (Ask is a stub), no browser is opened.
#   powershell -NoProfile -ExecutionPolicy Bypass -File download_resume_test.ps1 -Launcher <folder with downloads.ps1>
# Exit 0 = all passed.
param([string]$Launcher)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

$T = Join-Path ([IO.Path]::GetTempPath()) ('ua-dl-test-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
[void][IO.Directory]::CreateDirectory("$T\srv")
$script:PASS = 0; $script:FAIL = 0
function Ok([string]$m) { $script:PASS++; Write-Output "ok   $m" }
function Bad([string]$m, [string]$logFile = '') {
    $script:FAIL++; Write-Output "FAIL $m"
    if ($logFile -and [IO.File]::Exists($logFile)) { Get-Content $logFile | Select-Object -Last 30 | ForEach-Object { Write-Output "     | $_" } }
}
function Sha([string]$f) { $s = [Security.Cryptography.SHA256]::Create(); try { return (([BitConverter]::ToString($s.ComputeHash([IO.File]::ReadAllBytes($f)))) -replace '-', '').ToLowerInvariant() } finally { $s.Dispose() } }
function Size([string]$f) { if ([IO.File]::Exists($f)) { return (New-Object IO.FileInfo($f)).Length } return [long]0 }
$rnd = New-Object Random 7
function Mk([string]$f, [int]$n) { $b = New-Object byte[] $n; $rnd.NextBytes($b); $b[0] = 0x50; $b[1] = 0x4B; $b[2] = 3; $b[3] = 4; [IO.File]::WriteAllBytes($f, $b) }
$S = "$T\srv"
Mk "$S\game.zip" 3000000; Mk "$S\game-v2.zip" 3000000; Mk "$S\part1.zip" 1500000; Mk "$S\part2.zip" 1200000
$GSUM = Sha "$S\game.zip"

Add-Type -TypeDefinition @'
using System; using System.IO; using System.Net; using System.Net.Sockets; using System.Text; using System.Threading; using System.Collections.Generic;
public class UaDlServer {
    TcpListener l; string dir; public List<string> Log = new List<string>(); Dictionary<string, int> count = new Dictionary<string, int>();
    public UaDlServer(int port, string d) { l = new TcpListener(IPAddress.Loopback, port); dir = d; }
    public void Start() { l.Start(); Thread t = new Thread(Loop); t.IsBackground = true; t.Start(); }
    public void Stop() { l.Stop(); }
    void Loop() { for (;;) { TcpClient c; try { c = l.AcceptTcpClient(); } catch { return; } ThreadPool.QueueUserWorkItem(delegate { Serve(c); }); } }
    void Note(string s) { lock (Log) Log.Add(s); }
    void Head(NetworkStream s, string h) { byte[] b = Encoding.ASCII.GetBytes(h + "\r\n"); s.Write(b, 0, b.Length); }
    void Serve(TcpClient c) {
        try {
            NetworkStream s = c.GetStream(); StringBuilder sb = new StringBuilder(); byte[] one = new byte[1];
            while (!sb.ToString().EndsWith("\r\n\r\n") && s.Read(one, 0, 1) == 1) sb.Append((char)one[0]);
            string req = sb.ToString(); string path = req.Split(' ')[1].Split('?')[0];
            string rng = "";
            foreach (string line in req.Split(new string[] { "\r\n" }, StringSplitOptions.None))
                if (line.StartsWith("Range:", StringComparison.OrdinalIgnoreCase)) rng = line.Substring(6).Trim();
            string[] parts = path.Trim('/').Split('/');
            if (parts.Length != 2 || !File.Exists(Path.Combine(dir, parts[1]))) { Head(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n"); c.Close(); return; }
            string mode = parts[0], name = parts[1], f = Path.Combine(dir, name);
            byte[] data = File.ReadAllBytes(f); long size = data.Length;
            string tag = "\"" + size.ToString("x") + "-" + File.GetLastWriteTimeUtc(f).Ticks.ToString("x") + "\"";
            bool check = rng == "bytes=0-1023"; int n = 0;
            if (!check) lock (count) { int v; count.TryGetValue(mode + "/" + name, out v); n = v + 1; count[mode + "/" + name] = n; }
            long start = 0, end = size - 1;
            if (rng.StartsWith("bytes=")) { string[] ab = rng.Substring(6).Split('-'); if (ab[0] != "") start = long.Parse(ab[0]); if (ab.Length > 1 && ab[1] != "") end = Math.Min(long.Parse(ab[1]), size - 1); }
            if ((mode == "bad" || (mode == "badonce" && n == 1)) && !check) { data = (byte[])data.Clone(); data[size / 2] ^= 0xFF; }
            long cut = -1;
            if (!check) {
                if (mode == "drop" || mode == "close") cut = size * 3 / 10;
                else if ((mode == "stall" || mode == "norange" || mode == "r416") && n == 1) cut = size * 3 / 10;
                else if (mode == "dead") cut = 0;
                else if (mode.StartsWith("flaky")) cut = n <= int.Parse(mode.Substring(5)) ? 0 : size * 3 / 10;
            }
            string rs = rng != "" ? start.ToString() : "-";
            if (rng != "" && !check && start > 0 && mode == "r416" || rng != "" && start >= size) {
                Head(s, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" + size + "\r\nContent-Length: 0\r\nConnection: close\r\n");
                c.Close(); Note(mode + " " + name + " range=" + rs + " -> 416 sent=0"); return;
            }
            bool partial = rng != "" && !(mode == "norange" && !check && start > 0);
            if (!partial) { start = 0; end = size - 1; }
            bool close10 = mode == "close" && !check;
            string h = (close10 ? "HTTP/1.0 " : "HTTP/1.1 ") + (partial ? "206 Partial Content\r\nContent-Range: bytes " + start + "-" + end + "/" + size : "200 OK");
            h += "\r\nAccept-Ranges: bytes\r\nETag: " + tag + "\r\nContent-Type: application/zip\r\n";
            if (!close10) h += "Content-Length: " + (end - start + 1) + "\r\n";
            h += "Connection: close\r\n";
            Head(s, h);
            long len = end - start + 1; if (cut >= 0 && cut < len) len = cut;
            long sent = 0;
            try {
                if (mode == "slow" && !check) { while (sent < len) { int k = (int)Math.Min(20000, len - sent); s.Write(data, (int)(start + sent), k); s.Flush(); sent += k; Thread.Sleep(100); } }
                else { s.Write(data, (int)start, (int)len); s.Flush(); sent = len; }
                if (mode == "stall" && n == 1 && !check) { Note(mode + " " + name + " range=" + rs + " -> stalling sent=" + sent); Thread.Sleep(600000); }
            } catch { }
            Note(mode + " " + name + " range=" + rs + " -> " + (partial ? 206 : 200) + " sent=" + sent);
            c.Close();
        } catch { try { c.Close(); } catch { } }
    }
}
'@
$port = Get-Random -Minimum 41000 -Maximum 52000
$srv = New-Object UaDlServer($port, $S); $srv.Start()
$U = "http://127.0.0.1:$port"

# the launcher's script scope, as UA-Reforged.ps1 sets it up
$script:HERE = "$T\app"; [void][IO.Directory]::CreateDirectory($HERE)
Copy-Item (Join-Path $Launcher 'downloads.ps1') $HERE
$DLTXT = [IO.File]::ReadAllText((Join-Path $Launcher 'downloads.txt'))
[IO.File]::WriteAllText("$HERE\downloads.txt", $DLTXT)
$script:INST = $T; $script:ICON = $null
$env:UA_PACK_NOUI = '1'; $env:UA_DL_STALL_SECS = '3'; $env:UA_DL_WAITS = '0 0 0 0'
$script:SUPPORT = "$T\sup"; $script:LOG = "$T\sup\launcher.log"; [void][IO.Directory]::CreateDirectory($SUPPORT)
. (Join-Path $HERE 'downloads.ps1')
function Ask([string]$text, [string]$default, [string[]]$buttons) { Log ("ASK [{0}] buttons [{1}]" -f ($text -replace "`r?`n", ' '), ($buttons -join '|')); return '' }
function Start-Process { Log "BROWSER $args" }
$script:STATUS = New-Object Collections.Generic.List[string]
function Dl-Status([int]$pct, [string]$text) { $script:STATUS.Add("$pct`t$text") }

# Use <case>: a support folder of its own (kept between runs of the same case = a restart of the launcher)
function Use([string]$c) {
    $script:SUPPORT = "$T\sup-$c"; [void][IO.Directory]::CreateDirectory($script:SUPPORT)
    $script:LOG = "$T\$c.log"; $script:DL_PARTS = Join-Path $script:SUPPORT 'partial-downloads'
    $script:PACK_DL = Join-Path $script:SUPPORT 'reforge-pack-download'; $script:GAME_DL = Join-Path $script:SUPPORT 'game-download'
    $script:GAME_ORIG = Join-Path $script:SUPPORT 'game-original'; $script:PACK_OFF = Join-Path $script:SUPPORT 'reforge-pack-offer-off'
    $script:DL_DIR = Join-Path $script:SUPPORT 'dl'; [void][IO.Directory]::CreateDirectory($script:DL_DIR)
    $script:DL_CANCEL = $false; $script:STATUS.Clear(); $script:LINKS_DONE = $false; $script:PACK_ASKED = $false
}
function Fetch([string]$c, [string]$url, [string]$name, [long]$bytes, [string]$sum) {
    Use $c; $script:DL_WHAT = 'Downloading'; $script:DL_BASE = [long]0; $script:DL_TOTAL = $bytes
    if (-not (Dl-Precheck $url 0 0 $bytes)) { Log "RESULT precheck $($script:PC_WHY)"; return 3 }
    $rc = Dl-Fetch $url (Join-Path $script:DL_DIR $name) $bytes $sum
    Log "RESULT rc=$rc net=$($script:DL_NET) badsum=$($script:DL_BADSUM) why=$($script:DL_WHY)"
    return $rc
}
function Part([string]$c, [string]$n) { return "$T\sup-$c\partial-downloads\$n.part" }
function Got([string]$c, [string]$n) { return "$T\sup-$c\dl\$n" }
function LogHas([string]$c, [string]$re) { return ([IO.File]::ReadAllText("$T\$c.log") -match $re) }
function Count([string]$prefix) { return @($srv.Log.ToArray() | Where-Object { $_.StartsWith($prefix) }).Count }

try {
# 1. plain
[void](Fetch 'ok' "$U/ok/game.zip" 'game.zip' 3000000 $GSUM)
if ((Sha (Got 'ok' 'game.zip')) -eq $GSUM -and -not (Test-Path (Part 'ok' 'game.zip')) -and -not (Test-Path ((Part 'ok' 'game.zip') + '.id'))) { Ok 'plain download: right file, partial and its id removed' } else { Bad 'plain download' "$T\ok.log" }
# 2. drops every 30 %
[void](Fetch 'drop' "$U/drop/game.zip" 'game.zip' 3000000 $GSUM)
$n = Count 'drop game.zip range=9'; $n += Count 'drop game.zip range=18'; $n += Count 'drop game.zip range=27'
if ([IO.File]::Exists((Got 'drop' 'game.zip')) -and (Sha (Got 'drop' 'game.zip')) -eq $GSUM -and $n -ge 3) { Ok "drops every 30 %: carried on $n times, file right" } else { Bad "drops every 30 % ($n)" "$T\drop.log" }
# 3. ends early without an error
[void](Fetch 'close' "$U/close/game.zip" 'game.zip' 3000000 '')
if ([IO.File]::Exists((Got 'close' 'game.zip')) -and (Sha (Got 'close' 'game.zip')) -eq $GSUM -and (LogHas 'close' 'connection dropped')) { Ok 'transfer ends early without an error: noticed by the size, carried on, file right' } else { Bad 'transfer ends early' "$T\close.log" }
# 4. stall
$t0 = Get-Date; [void](Fetch 'stall' "$U/stall/game.zip" 'game.zip' 3000000 $GSUM); $secs = [int]((Get-Date) - $t0).TotalSeconds
if ([IO.File]::Exists((Got 'stall' 'game.zip')) -and (Sha (Got 'stall' 'game.zip')) -eq $GSUM -and (LogHas 'stall' 'stalled') -and (Count 'stall game.zip range=900000') -ge 1) { Ok "stall: given up after the stall time ($secs s), carried on from 30 %, file right" } else { Bad 'stall' "$T\stall.log" }
# 5. 200 to a Range request
[void](Fetch 'norange' "$U/norange/game.zip" 'game.zip' 3000000 $GSUM)
if ([IO.File]::Exists((Got 'norange' 'game.zip')) -and (Sha (Got 'norange' 'game.zip')) -eq $GSUM -and (LogHas 'norange' 'does not carry on downloads')) { Ok 'server ignores Range (200): started over, file right' } else { Bad 'server ignores Range' "$T\norange.log" }
# 6. 416
[void](Fetch 'r416' "$U/r416/game.zip" 'game.zip' 3000000 $GSUM)
if ([IO.File]::Exists((Got 'r416' 'game.zip')) -and (Sha (Got 'r416' 'game.zip')) -eq $GSUM -and (LogHas 'r416' 'HTTP 416')) { Ok 'server refuses Range (416): started over, file right' } else { Bad 'server refuses Range (416)' "$T\r416.log" }
# 7. wrong SHA-256 once
[void](Fetch 'badonce' "$U/badonce/game.zip" 'game.zip' 3000000 $GSUM)
if ([IO.File]::Exists((Got 'badonce' 'game.zip')) -and (Sha (Got 'badonce' 'game.zip')) -eq $GSUM -and (LogHas 'badonce' 'once more from the start') -and (Count 'badonce game.zip range=- ') -eq 2) { Ok 'wrong SHA-256 once: downloaded once more, file right' } else { Bad 'wrong SHA-256 once' "$T\badonce.log" }
# 8. always wrong
[void](Fetch 'bad' "$U/bad/game.zip" 'game.zip' 3000000 $GSUM)
if ((LogHas 'bad' 'RESULT rc=1 net=0 badsum=1') -and -not (Test-Path (Got 'bad' 'game.zip')) -and -not (Test-Path (Part 'bad' 'game.zip')) -and (Count 'bad game.zip range=- ') -eq 2) { Ok 'always the wrong SHA-256: 2 downloads, then refused, nothing kept' } else { Bad 'always the wrong SHA-256' "$T\bad.log" }
# 9. no data at all: 5 tries with waits shown
$env:UA_DL_WAITS = '1 1 1 1'; [void](Fetch 'dead' "$U/dead/game.zip" 'game.zip' 3000000 $GSUM); $env:UA_DL_WAITS = '0 0 0 0'
$n = Count 'dead game.zip range=- '
if ((LogHas 'dead' 'RESULT rc=1 net=1') -and $n -eq 5 -and (@($script:STATUS | Where-Object { $_ -like '*it stopped, carrying on in 1 s*' }).Count -ge 4)) { Ok 'no data at all: 5 tries (waits between them shown), then given up as a connection problem' } else { Bad "no data at all (tries $n)" "$T\dead.log" }
# 10. Cancel half way (UA_TEST_CANCEL_MB), then a new run carries on; the progress starts where it was
$env:UA_TEST_CANCEL_MB = '1'; $rc = Fetch 'cancel' "$U/slow/game.zip" 'game.zip' 3000000 $GSUM; Remove-Item Env:\UA_TEST_CANCEL_MB
$h = Size (Part 'cancel' 'game.zip')
$rc2 = Fetch 'cancel' "$U/ok/game.zip" 'game.zip' 3000000 $GSUM
$p0 = -1; foreach ($l in $script:STATUS) { if ($l -match '^(\d+)\tDownloading') { $p0 = [int]$Matches[1]; break } }
$st = @($srv.Log.ToArray() | Where-Object { $_ -match '^ok game\.zip range=([1-9]\d*) ' } | ForEach-Object { [long]$Matches[1] })
if ($rc -eq 130 -and $h -gt 0 -and (LogHas 'cancel' 'carrying on from') -and $st -contains $h -and (Sha (Got 'cancel' 'game.zip')) -eq $GSUM) { Ok "Cancel: partial download kept ($h bytes), the next run carried on from there, file right" } else { Bad "Cancel ($rc, $h)" "$T\cancel.log" }
if ($p0 -gt 0 -and $p0 -ge [int]($h * 90 / 3000000)) { Ok "progress after the restart starts at $p0 % (not 0 %)" } else { Bad "progress after the restart starts at $p0 %" }
# 11. the launcher process killed half way, started again
$kid = "$T\kid.ps1"
[IO.File]::WriteAllText($kid, @"
`$script:HERE = '$HERE'; `$script:SUPPORT = '$T\sup-kill'; `$script:LOG = '$T\kill-kid.log'; `$script:INST = '$T'; `$script:ICON = `$null
. '$HERE\downloads.ps1'
`$script:DL_DIR = '$T\sup-kill\dl'; [void][IO.Directory]::CreateDirectory(`$script:DL_DIR); `$script:DL_WHAT = 'x'; `$script:DL_BASE = 0; `$script:DL_TOTAL = 3000000
if (Dl-Precheck '$U/slow/game.zip' 0 0 3000000) { [void](Dl-Fetch '$U/slow/game.zip' '$T\sup-kill\dl\game.zip' 3000000 '$GSUM') }
"@)
[void][IO.Directory]::CreateDirectory("$T\sup-kill")
$kp = Microsoft.PowerShell.Management\Start-Process powershell.exe -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $kid) -WindowStyle Hidden -PassThru
for ($i = 0; $i -lt 150 -and (Size (Part 'kill' 'game.zip')) -lt 600000; $i++) { Start-Sleep -Milliseconds 100 }
Microsoft.PowerShell.Management\Stop-Process -Id $kp.Id -Force
Get-CimInstance Win32_Process -Filter "Name = 'curl.exe'" | Where-Object { $_.CommandLine -like "*$U/slow/game.zip*" } | ForEach-Object { Microsoft.PowerShell.Management\Stop-Process -Id $_.ProcessId -Force }
Start-Sleep -Milliseconds 500
$h = Size (Part 'kill' 'game.zip')
[void](Fetch 'kill' "$U/ok/game.zip" 'game.zip' 3000000 $GSUM)
$st = @($srv.Log.ToArray() | Where-Object { $_ -match '^ok game\.zip range=([1-9]\d*) ' } | ForEach-Object { [long]$Matches[1] })
if ($h -gt 0 -and $st -contains $h -and (Sha (Got 'kill' 'game.zip')) -eq $GSUM) { Ok "launcher killed half way: the next start carried on ($h bytes), file right" } else { Bad "killed launcher ($h)" "$T\kill.log" }
# 12. new version on the server: same size, new tag -> old partial not used
$env:UA_TEST_CANCEL_MB = '1'; [void](Fetch 'ver' "$U/slow/game.zip" 'game.zip' 3000000 ''); Remove-Item Env:\UA_TEST_CANCEL_MB
Copy-Item "$S\game.zip" "$S\game-v1.zip" -Force; Start-Sleep -Milliseconds 1100; Copy-Item "$S\game-v2.zip" "$S\game.zip" -Force; (Get-Item "$S\game.zip").LastWriteTimeUtc = [DateTime]::UtcNow
[void](Fetch 'ver' "$U/ok/game.zip" 'game.zip' 3000000 '')
if ((LogHas 'ver' 'another version') -and (Sha (Got 'ver' 'game.zip')) -eq (Sha "$S\game-v2.zip")) { Ok 'new version on the server (new tag): old partial discarded, new file downloaded whole' } else { Bad 'new version on the server' "$T\ver.log" }
Copy-Item "$S\game-v1.zip" "$S\game.zip" -Force
# partial for another SHA-256, partial for another size
$env:UA_TEST_CANCEL_MB = '1'; [void](Fetch 'ver2' "$U/slow/game.zip" 'game.zip' 3000000 $GSUM); Remove-Item Env:\UA_TEST_CANCEL_MB
$idf = (Part 'ver2' 'game.zip') + '.id'; [IO.File]::WriteAllText($idf, ([IO.File]::ReadAllText($idf) -replace 'sha256=[0-9a-f]+', ('sha256=' + ('0' * 64))))
[void](Fetch 'ver2' "$U/ok/game.zip" 'game.zip' 3000000 $GSUM)
if ((LogHas 'ver2' 'another version') -and (Sha (Got 'ver2' 'game.zip')) -eq $GSUM) { Ok 'partial for another SHA-256: discarded, file right' } else { Bad 'partial for another SHA-256' "$T\ver2.log" }
$P1 = Sha "$S\part1.zip"; $P2 = Sha "$S\part2.zip"
[void][IO.Directory]::CreateDirectory("$T\sup-ver3\partial-downloads")
[IO.File]::WriteAllBytes((Part 'ver3' 'part1.zip'), [byte[]]([IO.File]::ReadAllBytes("$S\part1.zip")[0..499999]))
[IO.File]::WriteAllText((Part 'ver3' 'part1.zip') + '.id', "bytes=1400000 sha256=$P1 tag=-`n")
[void](Fetch 'ver3' "$U/ok/part1.zip" 'part1.zip' 1500000 $P1)
if ((LogHas 'ver3' 'another version') -and (Sha (Got 'ver3' 'part1.zip')) -eq $P1) { Ok 'partial for another file size: discarded, file right' } else { Bad 'partial for another file size' "$T\ver3.log" }

# 13. Game-Get: Try again carries on; Not now goes back to the choices
Add-Type -AssemblyName System.IO.Compression
$zs = [IO.File]::Open("$S\realgame.zip", 'Create'); $z = New-Object IO.Compression.ZipArchive($zs, [IO.Compression.ZipArchiveMode]::Create)
foreach ($nm in @('UA/DATA/world.ini', 'UA/DATA/SET1/x', 'UA/DATA/SCRIPTS/x', 'UA/DATA/SOUNDS/x') + @(0..11 | ForEach-Object { 'UA/LEVELS/SINGLE/L{0:D2}.LDF' -f $_ })) {
    $e = $z.CreateEntry($nm, [IO.Compression.CompressionLevel]::NoCompression); $w = $e.Open(); $b = New-Object byte[] 120000; $rnd.NextBytes($b); $w.Write($b, 0, $b.Length); $w.Dispose()
}
$z.Dispose(); $zs.Dispose()
[IO.File]::WriteAllText("$HERE\downloads.txt", ($DLTXT -replace "(?m)^game_min_bytes\t.*$", "game_min_bytes`t1000"))
$env:UA_LINKS_URL = "$U/none/links.json"; $env:UA_LINKS_ALLOW_LOCAL = '1'; $env:UA_GAME_AUTO = 'get'
function Load-Conf { $script:DL_CONF = "$HERE\downloads.txt" }
Use 'try'; Load-Conf; $env:UA_GAME_URL = "$U/flaky6/realgame.zip"; $env:UA_RETRY_AUTO = 'again'; $env:UA_NOTICE_AUTO = 'later'
$rc = Game-Get; Log "RESULT game_get rc=$rc"
if ((LogHas 'try' 'The download (keeps stopping\. Check your connection and press Try again; it will carry on where it stopped|could not get going\. Check your connection and press Try again)')) { Ok 'game: connection keeps failing -> the Try again notice' } else { Bad 'game: Try again notice' "$T\try.log" }
if ((LogHas 'try' 'again: again') -and (LogHas 'try' 'get the game: installed') -and $rc -eq 0) { Ok 'game: Try again carried on and the game was set up' } else { Bad 'game: Try again' "$T\try.log" }
Use 'later'; $env:UA_GAME_URL = "$U/flaky99/realgame.zip"; $env:UA_RETRY_AUTO = ''
$rc = Game-Get; Log "RESULT game_get rc=$rc"
if ((LogHas 'later' 'again: later') -and $rc -eq 1 -and (LogHas 'later' 'partial download stays')) { Ok 'game: Not now -> back to the choices (rc 1), nothing broken' } else { Bad 'game: Not now' "$T\later.log" }
Remove-Item Env:\UA_GAME_URL, Env:\UA_GAME_AUTO

# 14. Pack-Get
function Set-Parts([string]$mode) {
    $t = ($DLTXT -split "`n" | Where-Object { $_ -notmatch "^(part|url)`t" }) -join "`n"
    $t += "`nurl`t$U/$mode/`npart`tpart1.zip`t1500000`t$P1`npart`tpart2.zip`t1200000`t$P2`n"
    [IO.File]::WriteAllText("$HERE\downloads.txt", $t); Load-Conf
}
Set-Parts 'flaky6'; Use 'pack'; $env:UA_PACK_AUTO = 'yes'; $env:UA_RETRY_AUTO = '2'
Links-Load; $rc = Pack-Get; Log "RESULT pack_get rc=$rc"
$ag = ([regex]::Matches([IO.File]::ReadAllText("$T\pack.log"), 'again: again')).Count
if ($ag -eq 2 -and (LogHas 'pack' 'does not unpack|not a Reforge pack') -and -not (LogHas 'pack' 'damaged or not the expected')) { Ok 'pack: Try again (twice) carried on, both parts downloaded and SHA-256 checked' } else { Bad "pack: Try again ($ag)" "$T\pack.log" }
Set-Parts 'flaky99'; Use 'packl'; $env:UA_RETRY_AUTO = ''
[void][IO.Directory]::CreateDirectory("$T\sup-packl\partial-downloads")
[IO.File]::WriteAllBytes((Part 'packl' 'part1.zip'), [byte[]]([IO.File]::ReadAllBytes("$S\part1.zip")[0..699999]))
$tag = '"' + (Size "$S\part1.zip").ToString('x') + '-' + (Get-Item "$S\part1.zip").LastWriteTimeUtc.Ticks.ToString('x') + '"'
[IO.File]::WriteAllText((Part 'packl' 'part1.zip') + '.id', "bytes=1500000 sha256=$P1 tag=$tag`n")
Links-Load; $rc = Pack-Get; Log "RESULT pack_get rc=$rc"
if ((LogHas 'packl' 'again: later') -and (Size (Part 'packl' 'part1.zip')) -eq 700000 -and (LogHas 'packl' '700 KB of 2\.7 MB are already downloaded|700 KB of 3 MB are already downloaded|700 KB of 2 MB are already downloaded')) { Ok 'pack: Not now keeps the partial part; the notice says how much is here' } else { Bad 'pack: Not now' "$T\packl.log" }
Set-Parts 'ok'; Use 'packo'
[void][IO.Directory]::CreateDirectory("$T\sup-packo\reforge-pack-download")
[IO.File]::WriteAllBytes("$T\sup-packo\reforge-pack-download\part1.zip", [byte[]]([IO.File]::ReadAllBytes("$S\part1.zip")[0..599999]))
Links-Load; $rc = Pack-Get; Log "RESULT pack_get rc=$rc"
if ((LogHas 'packo' 'carrying on from 600 KB') -and (Count 'ok part1.zip range=600000 ') -ge 1) { Ok 'pack: a part an older version stopped carries on (Range from 600000)' } else { Bad "pack: older version's part" "$T\packo.log" }
} finally {
    $srv.Stop()
    foreach ($v in 'UA_PACK_NOUI', 'UA_DL_STALL_SECS', 'UA_DL_WAITS', 'UA_LINKS_URL', 'UA_LINKS_ALLOW_LOCAL', 'UA_RETRY_AUTO', 'UA_NOTICE_AUTO', 'UA_PACK_AUTO', 'UA_TEST_CANCEL_MB', 'UA_GAME_URL', 'UA_GAME_AUTO') { Remove-Item "Env:\$v" -ErrorAction SilentlyContinue }
    Get-CimInstance Win32_Process -Filter "Name = 'curl.exe'" | Where-Object { $_.CommandLine -like "*127.0.0.1:$port*" } | ForEach-Object { Microsoft.PowerShell.Management\Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 300
    Remove-Item -LiteralPath $T -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Output "$($script:PASS) passed, $($script:FAIL) failed (windows)"
exit [int]($script:FAIL -ne 0)

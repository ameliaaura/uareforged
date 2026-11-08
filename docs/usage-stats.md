# Anonymous usage stats: what is sent, wire format v2

UA Reforged can send **anonymous usage stats**: a crash report if the game crashes, a short performance
summary after each mission, a few error codes, and a count of how many people play. It is **off unless you
say yes**. The first start asks once; Options has the switch **"Send anonymous usage stats"** on the desktop
and in VR. Turning it off stops sending at once and deletes everything the app kept for it (the random
install number, the unsent queue, any crash file).

This file is the exact contract between the apps and the site's endpoint. A field that is not listed here
is never sent; a body that breaks a rule here is refused by the server (`400`).

## What is never sent

No name, no account, no email, no IP address stored, no hardware IDs or serial numbers, no MAC address, no
device name, no user name, no file paths, no file names from your disk, no memory contents, no save games, no
chat or text you typed, no location, no advertising ID. Crash reports carry only program-module **base names**
(`UA_source`, `libSDL2-2.0.0.dylib`, `ntdll.dll`) and numeric offsets inside them, never a folder.

## Transport

| | |
|---|---|
| Endpoint | `POST https://uareforged.pages.dev/api/ping` (fixed in the apps; HTTPS only) |
| Headers | `Content-Type: application/json`, `User-Agent: UA-Reforged` (fixed, no OS or device in it) |
| Body | one JSON object, UTF-8, ASCII only in practice; **≤ 4096 bytes**, `crash` bodies **≤ 8192 bytes** |
| Answers | `204` accepted · `400` bad body (the app drops it, no retry) · anything else / no answer = try later |
| Client rules | background thread only; 3 s timeout per try; at most one retry right away; then the event waits in a local queue of **at most 20 events** (oldest dropped) and is sent on a later start; never blocks the game or a menu; silent on failure |

The server should store the fields below and the receive time, and nothing from the connection (no IP, no
headers beyond what it needs to answer).

## Common fields (every event)

| Field | Type | Rule |
|---|---|---|
| `v` | integer | `2` |
| `install` | string | 32 lowercase hex digits (`^[0-9a-f]{32}$`): 128 random bits from the OS's secure random source, made when stats are turned on, deleted when they are turned off (a new one each time they are turned on again). Never derived from the hardware. |
| `app` | string | `desktop` or `vr` |
| `platform` | string | `mac`, `windows` or `quest` |
| `version` | string | app version, `^[0-9A-Za-z.+-]{1,32}$` (e.g. `0.8.1`, `0.9.1`) |
| `event` | string | `launch`, `level_start`, `perf`, `crash` or `error` |

Key order in the apps' bodies is the order of the tables (common fields first); the server must not depend on
it. Unknown keys: `400`. Missing required keys: `400`.

### Shared value rules

| Field | Type | Rule |
|---|---|---|
| `preset` | string | `original`, `low`, `medium`, `high` or `ultra` (the Graphics preset). VR: New Look = `high`, Original look = `original`. Desktop with no preset chosen yet: `high` when the Reforge pack is installed, else `original`. |
| `level` | integer | `0`–`9999` (the game's level number) |

## `launch`: once per app start

| Field | Type | Req. | Rule |
|---|---|---|---|
| `preset` | string | yes | see above |
| `os` | string | yes | `^[0-9A-Za-z .()_+-]{1,40}$`; name + version only, e.g. `macOS 15.2`, `Windows 11 23H2`, `Windows 10 22H2`, `Quest OS 74` (`Quest OS (Android 14)` when the Horizon OS version can't be read) |
| `gpu` | string | yes | `^[0-9A-Za-z .()_+-]{1,64}$`; the graphics driver's renderer name, coarse: anything after the first `/` cut, `(TM)` / `(R)` removed, spaces collapsed; e.g. `Apple M2`, `NVIDIA GeForce RTX 3070`, `Adreno 740`; `unknown` when none |

```json
{"v":2,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"desktop","platform":"mac","version":"0.8.1","event":"launch","preset":"high","os":"macOS 15.2","gpu":"Apple M2"}
```

## `level_start`: when a mission starts (new, restarted, loaded or network)

| Field | Type | Req. | Rule |
|---|---|---|---|
| `preset` | string | yes | |
| `level` | integer | yes | 0–9999 |

```json
{"v":2,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"vr","platform":"quest","version":"0.9.1","event":"level_start","preset":"high","level":7}
```

## `perf`: when a mission ends or the game is quit during one

Measured over the mission's played frames (a frame = one pass of the game loop while the mission runs).

| Field | Type | Req. | Rule |
|---|---|---|---|
| `preset` | string | yes | at the end of the mission |
| `level` | integer | yes | 0–9999 |
| `res` | string | yes | render resolution `WxH`, `^[1-9][0-9]{1,4}x[1-9][0-9]{1,4}$` (desktop: the game's drawing size; Quest: one eye's image) |
| `fps_avg` | number | yes | frames ÷ seconds, 0–1000, one decimal |
| `fps_p1low` | number | yes | "1 % low": 1000 ÷ the 99th-percentile frame time in ms, 0–1000, one decimal |
| `frame_ms_p95` | number | yes | 95th-percentile frame time in ms, 0–10000, one decimal (0.25 ms resolution up to 250 ms) |
| `duration_s` | integer | yes | played seconds, 0–86400 (capped) |
| `hitches` | integer | yes | frames that took over 100 ms, 0–1000000 |
| `vr_hz` | integer | Quest only | the headset's display rate during the mission, 0–240 |

Missions shorter than 5 seconds or 30 frames send no `perf`.

```json
{"v":2,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"desktop","platform":"windows","version":"0.8.1","event":"perf","preset":"medium","level":7,"res":"1920x1080","fps_avg":118.4,"fps_p1low":61.2,"frame_ms_p95":11.3,"duration_s":842,"hitches":3}
```

```json
{"v":2,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"vr","platform":"quest","version":"0.9.1","event":"perf","preset":"high","level":1,"res":"1680x1760","fps_avg":89.6,"fps_p1low":71.4,"frame_ms_p95":11.5,"duration_s":604,"hitches":0,"vr_hz":90}
```

## `crash`: on the next start after a crash

A crash handler (POSIX signals on macOS / Quest, a structured-exception filter on Windows) writes a small local
file (`usage-stats-crash.txt`) only while stats are on, then hands the crash on to the system (macOS crash
reporter, Windows Error Reporting, Android tombstones keep working). On the next start the file is sent and
deleted once the server accepted it (or moved into the queue); with stats off it is deleted unsent.

| Field | Type | Req. | Rule |
|---|---|---|---|
| `preset` | string | yes | at the time of the crash |
| `level` | integer | no | the mission running at the time (absent: in the menus) |
| `build` | string | yes | build identifier, `^[0-9A-Za-z._-]{1,40}$` (the source commit of the build, `dev` for local builds) |
| `signal` | string | yes | `^[A-Z0-9_]{1,24}$`: `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE`, `SIGABRT`, `SIGTRAP`, or on Windows `EXC_` + the 8-digit hex exception code (`EXC_C0000005`) |
| `module` | string | yes | base name of the module with the faulting address, `^[0-9A-Za-z._+-]{1,64}$`, `unknown` when none |
| `offset` | string | yes | faulting address minus that module's load address, `^0x[0-9a-f]{1,16}$` |
| `frames` | array of strings | yes | 0–32 return addresses, innermost first, each `module+0xoffset` with the module rule above (`^[0-9A-Za-z._+-]{1,64}\+0x[0-9a-f]{1,16}$`) |
| `minutes` | integer | yes | minutes played in that session before the crash, 0–100000 |

```json
{"v":2,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"desktop","platform":"mac","version":"0.8.1","event":"crash","preset":"high","level":12,"build":"0b8d9f6345e4","signal":"SIGSEGV","module":"UA_source","offset":"0x1a2f40","frames":["UA_source+0x1a2f40","UA_source+0x1a3b18","UA_source+0x9c204","libdyld.dylib+0x3f10"],"minutes":23}
```

Offsets are turned back into function names offline with `tools/symbolicate-crash.py` and the build's own
symbol files (kept with the release builds, never shipped).

## `error`: a non-fatal problem that matters

A short code from this fixed list, plus an optional number. No free text.

| Field | Type | Req. | Rule |
|---|---|---|---|
| `code` | string | yes | one of the codes below |
| `detail` | integer | no | 0–1000000000, meaning per code |
| `preset` | string | no | when the game was running |
| `level` | integer | no | when a mission was involved |

| `code` | Where | `detail` |
|---|---|---|
| `pack_download_failed` | Reforge pack download (desktop launcher, Quest start screen) | curl exit code (desktop) or HTTP status when known |
| `pack_checksum_mismatch` | a pack part's SHA-256 didn't match | part number (1-based) |
| `game_download_failed` | "Get the game" download | curl exit code or HTTP status when known |
| `game_folder_invalid` | the chosen / copied game folder has no usable DATA + LEVELS | — |
| `links_json_invalid` | the site's links.json was refused (bad type / host) | — |
| `level_load_failed` | the engine couldn't set up a level | — (`level` set) |
| `material_map_missing` | "material map not loaded" during a level load | how many |
| `shader_load_failed` | a shader failed to compile or link | how many |
| `audio_device_failed` | no audio output device could be opened | — |
| `vr_session_lost` | Quest: the OpenXR session was lost (not a normal exit) | — |

Launcher-side errors (desktop) are kept in the local queue and sent by the game on that start or the next one.
Each code is sent at most once per app start.

```json
{"v":2,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"desktop","platform":"windows","version":"0.8.1","event":"error","code":"pack_checksum_mismatch","detail":2}
```

```json
{"v":2,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"desktop","platform":"mac","version":"0.8.1","event":"error","code":"material_map_missing","detail":3,"preset":"high","level":5}
```

## Files on the player's device

All in the app's own settings folder (macOS `~/Library/Application Support/UA Reforged/`, Windows
`%LOCALAPPDATA%\UA Reforged\`, Quest: the app's private storage):

| File | Holds | Removed |
|---|---|---|
| `usage-stats.txt` | the answer (`yes` / `no`) and, while on, the install number | the install number when stats are turned off |
| `usage-stats-queue.txt` | up to 20 events that could not be sent yet (the fields above, never more) | when sent, and all of it when stats are turned off |
| `usage-stats-crash.txt` | the last crash (fields of `crash`) | after sending, or at start when stats are off |

Nothing about a request is logged to disk. `UA_STATS_DEBUG=1` prints one line per request (event and result,
never the install number) to the game's console output, for troubleshooting.

## What the site keeps

The site's own privacy page is the authority: <https://uareforged.pages.dev/privacy/#usage-stats>. In short: the
day and the country (from Cloudflare) are noted with each message; the IP address and the user agent are never
stored; the install number is kept only as a salted SHA-256 hash; crash reports, performance summaries and error
codes are kept as totals (one example report per crash type); usage counts are deleted after 400 days.

## Libraries

No new dependency. HTTPS goes through what the platform already has: NSURLSession (Foundation, macOS),
WinHTTP (Windows), `java.net.HttpURLConnection` (Quest, through JNI, as the start screens' downloads already do).
Random bytes: `getentropy` (macOS), `BCryptGenRandom` (Windows), `arc4random_buf` (Android).

## Version history

- **v2** (UA Reforged 0.8.1 / 0.9.1): first public version. (`v:1` was a launch / level-start count only and
  never shipped.)

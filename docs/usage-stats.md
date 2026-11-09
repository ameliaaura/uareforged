# Anonymous usage stats: what is sent, wire format v3

UA Reforged can send **anonymous usage stats**: a crash report if the game crashes, a short performance
summary after each mission, how missions end and how long they took to load, how long the app was open, which
kind of controls were used, a coarse description of the computer or headset, a few error codes, and a count of
how many people play. It is **off unless you
say yes**. The first start asks once; Options has the switch **"Send anonymous usage stats"** on the desktop
and in VR. Turning it off stops sending at once and deletes everything the app kept for it (the random
install number, the unsent queue, any crash file).

This file is the exact contract between the apps and the site's endpoint. A field that is not listed here
is never sent; a body that breaks a rule here is refused by the server (`400`).

## What is never sent

No name, no account, no email, no IP address stored, no hardware IDs or serial numbers, no MAC address, no
device name, no user name, no file paths, no file names from your disk, no memory contents, no save games, no
chat or text you typed, no location, no advertising ID. A bug report's note is typed in the web browser on the
site's own form, never in the game, and never travels through this channel. Crash reports carry only program-module **base names**
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
| `v` | integer | `3` (the server still accepts `2` from older builds, with the v2 events and fields only) |
| `install` | string | 32 lowercase hex digits (`^[0-9a-f]{32}$`): 128 random bits from the OS's secure random source, made when stats are turned on, deleted when they are turned off (a new one each time they are turned on again). Never derived from the hardware. |
| `app` | string | `desktop` or `vr` |
| `platform` | string | `mac`, `windows` or `quest` |
| `version` | string | app version, `^[0-9A-Za-z.+-]{1,32}$` (e.g. `0.8.1`, `0.9.1`) |
| `event` | string | `launch`, `level_start`, `level_end`, `perf`, `session_end`, `crash`, `error` or `bug_report` |

Key order in the apps' bodies is the order of the tables (common fields first); the server must not depend on
it. Unknown keys: `400`. Missing required keys: `400`.

### Shared value rules

| Field | Type | Rule |
|---|---|---|
| `preset` | string | `original`, `low`, `medium`, `high` or `ultra` (the Graphics preset). VR: New Look = `high`, Original look = `original`. Desktop with no preset chosen yet: `high` when the Reforge pack is installed, else `original`. |
| `level` | integer | `0`–`9999` (the game's level number) |
| `input` | string | `keyboard_mouse`, `controller`, `mixed` or `vr_controllers`: what was actually pressed during that mission or session. Desktop: key presses, mouse clicks and wheel turns are counted against controller button presses and stick pushes; both kinds with the smaller one at least a tenth of the larger = `mixed`, else the larger one. Quest: always `vr_controllers`. Absent when nothing was pressed. |

## `launch`: once per app start

| Field | Type | Req. | Rule |
|---|---|---|---|
| `preset` | string | yes | see above |
| `os` | string | yes | `^[0-9A-Za-z .()_+-]{1,40}$`; name + version only, e.g. `macOS 15.2`, `Windows 11 23H2`, `Windows 10 22H2`, `Quest OS 74` (`Quest OS (Android 14)` when the Horizon OS version can't be read) |
| `gpu` | string | yes | `^[0-9A-Za-z .()_+-]{1,64}$`; the graphics driver's renderer name, coarse: anything after the first `/` cut, `(TM)` / `(R)` removed, spaces collapsed; e.g. `Apple M2`, `NVIDIA GeForce RTX 3070`, `Adreno 740`; `unknown` when none |
| `ram_gb` | integer | no | memory, rounded down to one of `2`, `4`, `8`, `16`, `32`, `64` (`64` = 64 GB or more; under 4 GB = `2`); absent when the system doesn't say |
| `cpu_cores` | integer | no | logical processor cores, rounded down to one of `1`, `2`, `4`, `6`, `8`, `12`, `16`, `24`, `32` (`32` = 32 or more) |
| `refresh_hz` | integer | no | the display's refresh rate in whole Hz, `1`–`500` (Quest: the headset's display rate at start); absent when unknown |
| `display` | string | no | desktop only: the main display's size as a bucket of its height in the system's display mode (macOS: points, not Retina pixels): `sd` (< 720), `720p` (720–1079), `1080p` (1080–1439), `1440p` (1440–2159), `4k` (2160–2879), `5k+` (2880 and up) |

```json
{"v":3,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"desktop","platform":"mac","version":"0.8.1","event":"launch","preset":"high","os":"macOS 15.2","gpu":"Apple M2","ram_gb":16,"cpu_cores":8,"refresh_hz":60,"display":"1080p"}
```

## `level_start`: when a mission starts (new, restarted, loaded or network)

Sent at the mission's first playable frame (v3; v2 sent it when the level had loaded).

| Field | Type | Req. | Rule |
|---|---|---|---|
| `preset` | string | yes | |
| `level` | integer | yes | 0–9999 |
| `load_ms` | integer | no | loading time in milliseconds, `0`–`600000`: from the moment the mission was asked for (Play, Load, Restart, joining a network game) to its first playable frame |

```json
{"v":3,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"vr","platform":"quest","version":"0.9.1","event":"level_start","preset":"high","level":7,"load_ms":4210}
```

## `level_end`: how a mission ended

Sent when a mission ends; when the app is closed during a mission it is kept for the next start (`quit`).

| Field | Type | Req. | Rule |
|---|---|---|---|
| `preset` | string | yes | at the end of the mission |
| `level` | integer | yes | 0–9999 |
| `result` | string | yes | `won` (the host station went through the gate), `lost` (the host station was destroyed), `quit` (Exit Mission, or the app was closed during the mission), `restart` (Restart, or a saved game was loaded during the mission) |
| `duration_s` | integer | yes | seconds from the first playable frame to the end, pauses included, `0`–`86400` (capped) |
| `input` | string | no | see the shared rules: what was pressed during this mission |
| `vr_table_s` | integer | Quest only | seconds the war table (the miniature command view) was open, `0`–`86400` |
| `vr_unit_s` | integer | Quest only | seconds spent inside a unit other than the host station (first person), `0`–`86400` |
| `vr_jumps` | integer | Quest only | how many times the player jumped into a unit (a different unit from the one before, the host station not counted), `0`–`100000` |

```json
{"v":3,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"desktop","platform":"windows","version":"0.8.1","event":"level_end","preset":"medium","level":7,"result":"won","duration_s":911,"input":"controller"}
```

```json
{"v":3,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"vr","platform":"quest","version":"0.9.1","event":"level_end","preset":"high","level":1,"result":"lost","duration_s":640,"input":"vr_controllers","vr_table_s":402,"vr_unit_s":198,"vr_jumps":6}
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
{"v":3,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"desktop","platform":"windows","version":"0.8.1","event":"perf","preset":"medium","level":7,"res":"1920x1080","fps_avg":118.4,"fps_p1low":61.2,"frame_ms_p95":11.3,"duration_s":842,"hitches":3}
```

```json
{"v":3,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"vr","platform":"quest","version":"0.9.1","event":"perf","preset":"high","level":1,"res":"1680x1760","fps_avg":89.6,"fps_p1low":71.4,"frame_ms_p95":11.5,"duration_s":604,"hitches":0,"vr_hz":90}
```

## `session_end`: how long the app was open

Sent when the app is closed normally (kept in the queue if it can't be sent in time). While the app runs, a small
local file (`usage-stats-session.txt`, the fields below) is updated once a minute; if the app is killed or crashes,
the next start sends it with `ended` = `killed` or `crash`. Day-1 / day-7 return rates are worked out by the site
from the days the same install sent a `launch`; the app sends nothing extra for that.

| Field | Type | Req. | Rule |
|---|---|---|---|
| `preset` | string | yes | at the end |
| `minutes` | integer | yes | minutes the app was open, menus included, `0`–`100000` (rounded down) |
| `levels` | integer | yes | missions started in that session, `0`–`10000` |
| `ended` | string | yes | `quit` (closed normally), `killed` (closed by the system or force-quit; counted to the last minute), `crash` |
| `input` | string | no | see the shared rules: what was pressed during the whole session |
| `look_switches` | integer | yes | how many times the player switched between the New Look and the Original look, `0`–`1000` (desktop: Graphics preset Original ⇄ any other; Quest: Settings > LOOK) |
| `preset_changes` | integer | yes | how many times the Graphics preset changed, `0`–`1000` (look switches included) |

```json
{"v":3,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"desktop","platform":"mac","version":"0.8.1","event":"session_end","preset":"high","minutes":47,"levels":3,"ended":"quit","input":"keyboard_mouse","look_switches":2,"preset_changes":3}
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
{"v":3,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"desktop","platform":"mac","version":"0.8.1","event":"crash","preset":"high","level":12,"build":"0b8d9f6345e4","signal":"SIGSEGV","module":"UA_source","offset":"0x1a2f40","frames":["UA_source+0x1a2f40","UA_source+0x1a3b18","UA_source+0x9c204","libdyld.dylib+0x3f10"],"minutes":23}
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
{"v":3,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"desktop","platform":"windows","version":"0.8.1","event":"error","code":"pack_checksum_mismatch","detail":2}
```

```json
{"v":3,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"desktop","platform":"mac","version":"0.8.1","event":"error","code":"material_map_missing","detail":3,"preset":"high","level":5}
```

## `bug_report`: the player chose "Report a bug"

**Report a bug** (desktop: *Options*, next to the stats switch; Quest: the pause menu) opens the site's feedback
form in the web browser at `https://uareforged.pages.dev/contribute/?ref=<ref>#feedback`, where the player types
the note. `<ref>` is a new short random reference for each report (8 characters from
`abcdefghjkmnpqrstuvwxyz23456789`; not the install number and not derived from it). The form sends the reference
with the note so the two can be matched. **With stats on**, the game also sends this event with the same
reference and what it knows about this session; **with stats off, only the browser opens and nothing is sent.**
At most 5 per app start.

| Field | Type | Req. | Rule |
|---|---|---|---|
| `ref` | string | yes | `^[a-z0-9]{8}$` (the app uses the 31 characters above) |
| `preset` | string | yes | now |
| `level` | integer | no | the mission running now, else the last one played in this session |
| `minutes` | integer | yes | minutes the app has been open, `0`–`100000` |
| `input` | string | no | see the shared rules (this session so far) |
| `errors` | array of strings | no | the error codes sent in this session, 0–10 entries, each one from the fixed list |
| `crash_signal` | string | no | the last crash (the one reported at this start): `signal` rule of `crash` |
| `crash_frames` | array of strings | no | its first 1–5 frames (`frames` rule of `crash`; the site's crash signature is made from these) |
| `fps_avg` | number | no | the last performance summary of this session, rule of `perf` |
| `fps_p1low` | number | no | the same summary's 1 % low, rule of `perf` |

```json
{"v":3,"install":"3f9c2a7d0b5e4c18a6d2f0e9b7c41a55","app":"desktop","platform":"mac","version":"0.8.1","event":"bug_report","ref":"k7m2q9xa","preset":"high","level":7,"minutes":32,"input":"keyboard_mouse","errors":["shader_load_failed"],"crash_signal":"SIGSEGV","crash_frames":["UA_source+0x1a2f40","UA_source+0x1a3b18"],"fps_avg":58.2,"fps_p1low":31.0}
```

## Files on the player's device

All in the app's own settings folder (macOS `~/Library/Application Support/UA Reforged/`, Windows
`%LOCALAPPDATA%\UA Reforged\`, Quest: the app's private storage):

| File | Holds | Removed |
|---|---|---|
| `usage-stats.txt` | the answer (`yes` / `no`) and, while on, the install number | the install number when stats are turned off |
| `usage-stats-queue.txt` | up to 20 events that could not be sent yet (the fields above, never more) | when sent, and all of it when stats are turned off |
| `usage-stats-crash.txt` | the last crash (fields of `crash`) | after sending, or at start when stats are off |
| `usage-stats-session.txt` | this session so far (fields of `session_end`), rewritten once a minute | when the app closes normally, at the next start after it was sent or queued, and when stats are turned off |

Nothing about a request is logged to disk. `UA_STATS_DEBUG=1` prints one line per request (event and result,
never the install number) to the game's console output, for troubleshooting.

## What the site keeps

The site's own privacy page is the authority: <https://uareforged.pages.dev/privacy/#usage-stats>. In short: the
day and the country (from Cloudflare) are noted with each message; the IP address and the user agent are never
stored; the install number is kept only as a salted SHA-256 hash; crash reports, performance summaries, mission
results, loading times, session summaries and error codes are kept as totals (one example report per crash type);
bug reports are kept one per reference without the install number; usage counts (app and mission starts) are
deleted after 400 days, mission results, loading times, session summaries and bug reports after 90 days.

## Libraries

No new dependency. HTTPS goes through what the platform already has: NSURLSession (Foundation, macOS),
WinHTTP (Windows), `java.net.HttpURLConnection` (Quest, through JNI, as the start screens' downloads already do).
Random bytes: `getentropy` (macOS), `BCryptGenRandom` (Windows), `arc4random_buf` (Android).

## Version history

- **v3** (UA Reforged 0.8.1 / 0.9.1): first public version. Adds `level_end`, `session_end`, `bug_report`;
  `load_ms` on `level_start`; `ram_gb`, `cpu_cores`, `refresh_hz`, `display` on `launch`. `level_start` moves to the
  first playable frame.
- **v2**: launch / level_start / perf / crash / error; release candidates only. The server keeps accepting it.
- `v:1` was a launch / level-start count only and never shipped.

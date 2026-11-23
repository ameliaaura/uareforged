# The Reforge visual upgrade

Everything the Reforged renderer adds is drawn by the engine itself (procedural shadows,
glow, trails, battle marks, atmosphere, the sharp UI text layer). The optional Reforge pack adds
remastered unit models, buildings and skies, HD textures, restored videos, sharp UI packs and
the floor pack. Without it the game keeps its 1998 look.

## The Reforge pack in the macOS app

The app looks for the pack in `~/Library/Application Support/UA Reforged/reforge-pack/`. When
none is there it offers a one-click download (`packaging/macos/downloads.sh`): the download
address and the size and SHA-256 of every part are in `packaging/macos/downloads.txt`. Each
link is checked first, each part's checksum is checked, the parts are unpacked next to the download, the pack's `install-links.sh --verify` rebuilds
and checks the shared files, and only then the folder is moved into place. A cancelled or broken
download resumes on the next start; the player's game folder is never touched. The launcher
links `hd-models/`, `hd-textures/` and `hd-fmv/` into the game's working folder and sets
`UA_HDMODELS_DIR`, `UA_SKY_DIR`, `UA_UI_DIR` and `UA_R2_GROUND_DIR`. "Don't ask again" writes
`~/Library/Application Support/UA Reforged/reforge-pack-offer-off`; delete it to get the offer
back. A broken link (no answer, HTTP error, a web page instead of the file, wrong size, a stall
of 30 s after two automatic resumes, a bad checksum) gives a plain notice with *Open help page*
or *Try again later*, removes the partial files, and the game starts with the 1998 look.

Before any download the app reads `links_url` (the site's `release/links.json`): its
`game_url`, `game_min_bytes`, `pack_url`, `pack_parts`, `help_url`, `pack_help_url` and
`message` replace the built-in values, so a broken link can be fixed without a new app. The
file is only parsed and checked (`packaging/macos/links.js`: strict types, https only, hosts on
the `allow_host` list of `downloads.txt`); one bad value rejects the whole file and the built-in
values are used. Testing: `UA_LINKS_URL`, `UA_GAME_URL`, `UA_PACK_URL` (other addresses),
`UA_GAME_AUTO=get`, `UA_PACK_AUTO=yes|no`, `UA_NOTICE_AUTO=help|folder|later`, `UA_PACK_NOUI=1`.

**New-version notice (desktop, from 0.8.5).** The app reads links.json once per start, in the background from
the moment it knows the game folder (the setup, the pack and the Quest install use that same read). Its optional
`latest` field names the newest version per platform:
`"latest": {"mac": {"version": "0.8.5", "url": "https://uareforged.pages.dev/downloads/"}, "windows": {...}, "linux": {...}}`.
`version` = 2 to 4 numbers (`0.8.5`, `0.8.4.1`; compared part by part, missing parts = 0), `url` = https on
`github.com/ameliaaura/uareforged` or `uareforged.pages.dev` only (no port, query, `.`/`..` segments). A bad `latest`
is dropped on its own (logged `latest_ignored`); the rest of the file still counts. Older apps skip the key. When
the version is newer than the app's own (`app_version`, written into `downloads.txt` by the build script), one
dialog before the game starts: Download (opens the url) / Later / Don't remind me for this version
(`update-skip.txt` in the app's support folder; a newer version asks again). Offline, a bad file, a missing field,
the same or an older version, or links.json not read within 2 s of that point: no dialog, the game starts. A start
with the game and the Reforge pack in place asks for `links.json?start` (the site does not count it as a setup).
Testing: `UA_LINKS_FILE=<file>` (a local links.json, no request), `UA_UPDATE_AUTO=download|later|skip` (answers the
dialog, opens no browser); tests `tests/launcher/update_notice_test.sh` (Mac, CI) and `update_notice_test.ps1`
(Windows PowerShell 5.1).

The campaign map always shows the 1998 picture (the pack carries no campaign-map pictures).

## Graphics presets

*Options > Graphics* offers **Original** (the 1998 renderer, unchanged), Low, Medium, High
and Ultra, with a first-launch benchmark that picks one. The choice is saved in
`SAVE/gfx.txt`. On Mac and Windows the benchmark runs once, on the first start with the
Reforge pack installed and no `SAVE/gfx.txt` yet: about 20 seconds of level 75 flown by a
fixed camera, sound off, measured at High; then the game opens with the preset that keeps
60 fps. A saved choice is never replaced. Esc (or closing the game) during the benchmark
saves nothing; it runs once more on the next start and then stops asking. `SAVE/gfx.bench`
tracks the attempts: if a benchmark never finishes (crash, hang), the next start skips it
and uses Medium. Saving any preset removes that file. `UA_GFX_DEFAULT=<preset>` sets the
default, `UA_GFX_BENCH=0` skips the benchmark.

## Model pack layout

The engine reads `hd-models/` in its working directory, or the folder named by
`UA_HDMODELS_DIR`:

```
hd-models/
  VP_<NAME>.obj / .mtl         one per visual prototype the pack replaces (plus VPf/VPw states)
  VP_<NAME>_albedo|normal|orm  PNG + DDS (BC1/BC5), authored far mips (_mip1.._mip5)
  fx/        fx.txt + procedural effect flipbooks and genesis (beam-in) shells
  trails/    trails.txt + trail sprites
  sky/<sky>/ sky.txt + sky_{1024,2048,4096}.dds (+ sky3d_* for 3D clouds), atmos.txt
```

A prototype with no files in the pack keeps its 1998 model. `UA_SKY_DIR` points at the sky
folder (the macOS app sets it for you).

## Optional AI commander tuning

The engine runs the 1998 enemy commander unless an `ai-tuning.txt` sits in the game
folder. `docs/ai-tuning.example.txt` documents every key; with no file present the
behaviour is the original one.

## Frame rate and game speed (desktop, since 0.8.5)

The game speed no longer depends on the frame rate: the world advances 1084 game ticks per real second at any
frame rate, which is exactly the old speed at the 60 fps cap (the engine used to add one tick per frame, so it ran
about 8 % faster at 144 fps and 2x at 1000 fps). The frame cap therefore follows the screen: `gfx.maxfps = 60`
(the value in the game package's `NUCLEUS.INI`) or `-1` caps at the display's refresh rate, with vsync; any
other value is used as given; `0` is uncapped. The cap is paced on the performance counter (144 Hz = 144 fps) and
follows the window to another display. The engine never runs above 1000 fps. The Quest app keeps its own
display-clock timing.

Code from 1998 that did something once per frame does it per 60 fps frame of time instead (`src/frame_rate.h`):
steering keys, sticks, hats and the mouse ramp up and let go in the same real time; brakes and drift (UFOs, host
stations, the hand brake) stop in the same distance; AI host stations scan the map at the same pace; ground
scraping costs the same energy per second; aircraft bank for their turn rate. `tests/frame_rate_test.cpp` checks
each at 20 to 1000 fps against the 1998 code at 60 fps.

## Useful variables

| Variable | Effect |
|---|---|
| `UA_HDMODELS_DIR` | model pack folder (a missing folder turns HD models off) |
| `UA_SKY_DIR` | remastered sky folder |
| `UA_GFX_DEFAULT`, `UA_GFX_BENCH` | default preset, first-launch benchmark on/off |
| `UA_WINDOW_SIZE=WxH` | window size |
| `UA_MAXFPS` | frame cap for this run (0 = uncapped, at most 1000) |
| `UA_GAME_CLOCK=1998` | the old game clock: one extra game tick per frame, so the game runs faster the higher the frame rate (comparison only) |
| `UA_UBO_RING=0/1/2` | Windows/Linux uniform-buffer ring off / mapped / persistent (default 2 with ARB/EXT_buffer_storage, otherwise off; macOS default 0) |
| `UA_AUTOSTART_LEVEL=<n>` | jump straight into level n (testing) |

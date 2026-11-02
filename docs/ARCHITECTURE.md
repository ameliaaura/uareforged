# Architecture: a map for contributors

UA Reforged is **OpenUA** (a re-implementation of the 1998 Urban Assault engine by Marisa-Chan and
contributors) plus the Reforged layer: a modern renderer, graphics presets, a loader for the Reforge pack,
platform launchers and the VR app. This page tells you where things live. The code itself is the reference;
most Reforged files start with a comment that explains what they do and which switch turns them on.

## The big picture

```
 player's game folder (DATA/, LEVELS/ …)   Reforge pack (optional)
            │ read-only                         │ hd-models/ hd-textures/ hd-fmv/ ui/ ground/
            ▼                                   ▼
 ┌──────────────────────── launcher (per platform) ─────────────────────────┐
 │ macOS: packaging/macos/launcher.sh   Windows: packaging/windows/launcher/ │
 │ first-run choices, "Get the game", pack download + checks, run folder     │
 └──────────────────────────────────┬────────────────────────────────────────┘
                                    ▼  working folder + environment
 ┌──────────────────────────────── engine (src/) ───────────────────────────┐
 │ game logic (yw_*, ypa*)  ◄──►  world data (world/)  ◄──►  GUI (gui/)     │
 │        │                                                                  │
 │        ▼                                                                  │
 │ renderer (system/gfx*, Reforged passes)   sound (system/sound, wrapal)     │
 │ input (system/inpt, idev)   network (netlib/)   Lua scripting             │
 └──────────────────────────────────────────────────────────────────────────┘
                                    ▲
                     Quest: platform/quest/ (OpenXR + Android) + src/vr/
```

## Source tree

| Path | What |
|---|---|
| `src/main.cpp` | Entry point, main loop, start-up switches |
| `src/yw*.cpp`, `src/yw_internal.h` | The game world: level load, the game loop (`yw_game.cpp`), HUD and in-game UI (`yw_game_ui.cpp`), briefings, saves, network play |
| `src/ypa*.cpp` | Units: base unit (`ypabact`), tanks, cars, flyers, UFOs, guns, missiles, host stations (`yparobo`) |
| `src/world/` | Level and prototype parsers, save parsers, particles, Lua events |
| `src/gui/`, `src/button.cpp`, `src/lstvw.cpp` | Menus and widgets |
| `src/system/` | Platform services: window and OpenGL (`gfx.cpp`, `glfuncs`), input (`inpt.cpp`), sound (`sound.cpp`), files (`fsmgr.cpp`), movies (`movie.cpp`; `movie_stub.cpp` on Quest), Lua (`luascript.cpp`) |
| `src/bitmap.cpp`, `src/ilbm.cpp`, `src/obj3d.cpp`, `src/sklt.cpp`, `src/ade.cpp` | 1998 asset formats: bitmaps, ILBM, 3D objects and skeletons; `obj3d.cpp` also loads the pack's OBJ/MTL models |
| `src/wrapal.cpp` | OpenAL audio backend |
| `src/netlib/`, `src/network.cpp` | Multiplayer (between friends over a private network) |
| `src/gfxsettings.*` | **Graphics presets** and per-feature levels: `gfxLevel("SHADOWS", 0)` etc. Start here for anything visual |
| `src/reforge*.cpp` | Reforged features: HD model/sky override and dev tools (`reforge.cpp`), effects (`reforge_fx`), battle marks (`reforge_marks`), trails (`reforge_trails`), skies (`reforge_sky`) |
| `src/system/` Reforged passes | `shadows.cpp`, `gfx_glow.cpp`, `atmos*.cpp` (light, haze, night windows), `ground.cpp` (detailed floor), `stdshdr.cpp` (shaders), `perf.cpp` (measurement) |
| `src/aiinfluence.*`, `src/aituning.*` | Optional enemy commander tuning; off unless an `ai-tuning.txt` exists ([REFORGE.md](REFORGE.md)) |
| `src/system/radio.*` | Optional radio-chatter hook; off by default |
| `src/vr/` | The VR module: OpenXR session (`vr_openxr`), rendering (`vr_gl`), controls, comfort, cockpit, the Command Deck (`vr_command`, `vr_field*`), overlays |
| `src/winport.h` | Windows stand-ins (`mkdir`, `realpath`, …), all behind `#ifdef _WIN32` |
| `src/version.h` | Version strings (the Quest app's first, the desktop's last) |
| `res/` | Engine shaders and the engine's own small resources (not game data) |
| `fonts/`, `locale/`, `localexp/` | Free fonts with their licences; locale tables from upstream |
| `platform/quest/` | Quest platform layer: Android entry, EGL, OpenXR input and frame timing, in-headset first-run screens (`setup/`) |
| `packaging/macos/` | Mac build (`build-mac.sh`), launcher, downloads (`downloads.sh`, `downloads.txt`, `links.js`), *Install on my Quest* (`quest.sh`) |
| `packaging/windows/` | Windows cross-build, PowerShell launcher (twins of the Mac scripts), NSIS installer |
| `packaging/quest/` | Quest build, manifest, dependency scripts, licences |
| `tools/reforge/` | `uatexc` (DDS encoder for packs) and the battle-marks atlas tool |
| `tools/vr/` | Quest pack builders (ASTC re-encoding) |

## How a frame is drawn

1. The game loop (`yw_game.cpp`) updates units and the world, then renders.
2. The 1998 renderer path draws sectors, buildings and units. With a preset above Original, the **V2** path
   replaces the lighting and adds the Reforged passes, each gated by its own level from `gfxsettings`:
   shadows, ground, sky and atmosphere, glow, battle marks, trails and effects.
3. Models: for each visual prototype, the HD override (`reforge.cpp`, loaded by `obj3d.cpp`) checks the pack's
   `hd-models/VP_<NAME>.obj`. If it is there it
   is drawn at the original size and anchor points; if not, the 1998 model is used.
4. The HUD and menus are drawn last, with the sharp UI text layer when the UI level is on.

**Rule:** at the **Original** preset every Reforged path is off and the game draws as in 1998. Keep it that way.

## Configuration

- **Presets** are saved in `SAVE/gfx.txt` in the run folder. Order of precedence is described at the top of
  `src/gfxsettings.h`.
- **Environment variables** (`UA_*`) switch development features and tests. Each one is documented next to its
  `getenv` call. Common ones are listed in [BUILD-macOS.md](BUILD-macOS.md#running-the-built-app) and
  [REFORGE.md](REFORGE.md).
- **Downloads:** `packaging/macos/downloads.txt` (shared with Windows) and `packaging/quest/downloads.txt` hold
  every address, size and SHA-256 the apps use. A `links.json` on the site can override addresses, within a
  strict host allowlist, so a moved file does not need a new app.

## Launchers

The engine expects to run inside a folder that looks like an Urban Assault install. The launchers build one per
user: read-only game folders are linked in from the player's folder, writable ones (`SAVE`, `ENV`, settings) are
copied once, and the pack's folders are linked over them. The Mac launcher is a shell script with native dialogs
through `osascript` (plus small JavaScript helpers for the links check and the progress window); the Windows launcher is a PowerShell port of the same logic. Change both together.

## VR

The Quest app is the same engine compiled with `UA_VR=ON` for Android. `platform/quest/` owns the Android
activity, EGL context and OpenXR session; `src/vr/` turns the game into stereo views, handles controllers and
comfort options, and adds the Command Deck. The in-headset first-run flow (`platform/quest/src/setup/`) mirrors
the desktop downloads, with the same checks.

## Where to start

- A visual bug at a given preset: find the feature's level in `gfxsettings.cpp`, then its pass in `src/system/`
  or `src/reforge_*`.
- A gameplay bug: `yw_game.cpp` and the unit class in `ypa*.cpp`; compare with the Original preset first.
- An install or download problem: `packaging/<platform>/` and the launcher log.
- New to the code: build, play a level with `UA_GFX_PRESET=original` and `high`, and read `gfxsettings.h`.

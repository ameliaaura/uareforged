# Installing and playing UA Reforged

UA Reforged is free. It needs **the original Urban Assault game files** (the folders `DATA` and `LEVELS`).
They belong to Microsoft, so we never host or ship them. The app fetches the free community **OpenUA Standalone
Package** (the full game plus the Metropolis Dawn add-on, 445 MB) for you, straight from the Metropolis Dawn
community's own server. Already have the game (your retail CD install)? Choose its folder instead. More in
[GAME-DATA.md](GAME-DATA.md). Every download and disk size: [SIZES.md](SIZES.md).

Downloads and checksums: **[uareforged.pages.dev/downloads](https://uareforged.pages.dev/downloads/)** and the
[GitHub releases](https://github.com/ameliaaura/uareforged/releases).

- [macOS](#macos)
- [Windows](#windows)
- [Meta Quest 3](#meta-quest-3)
- [The Reforge pack](#the-reforge-pack)
- [Graphics presets](#graphics-presets)
- [Where things are saved](#where-things-are-saved)
- [Uninstalling](#uninstalling)
- [Something went wrong](#something-went-wrong)

---

## macOS

**Needs** an Apple Silicon Mac (M1 or later) with macOS 11 or newer. Intel Macs are not supported.

1. **Download** `UA-Reforged-<version>-macOS-arm64.zip` (14 MB), unzip it and drag **UA Reforged.app** into
   *Applications*.
2. **Open it the first time.** The app is not notarised by Apple, so macOS blocks the first start once:
   1. Open *UA Reforged*. macOS says it was blocked: click **Done**.
   2. Open *System Settings > Privacy & Security* and scroll down to *Security*: "UA Reforged was blocked to
      protect your Mac". Click **Open Anyway**.
   3. Click **Open Anyway** again and confirm with Touch ID or your password.

   This needs an administrator: on a standard (non-admin) account macOS asks for an administrator's name and
   password. You only do it once. (On macOS 14 and older, Control-click > **Open** also works; on macOS 15 and
   newer it does not.)
3. **Set up everything.** The first start shows one dialog: click **Set up everything**. The app downloads the
   game (445 MB, from the Metropolis Dawn community) and the [Reforge pack](#the-reforge-pack) (1.6 GB) in one
   progress window. Already have the game? Click **I already have the game** and choose the folder that holds
   `DATA` and `LEVELS`. Untick **Include the Reforge pack** to keep the 1998 look for now.
4. **Play.** Every start shows a small launcher: pick the campaign and click **Play**. Pick a preset in
   *Options > Graphics*.

## Windows

**Needs** Windows 10 (version 1803 or newer) or Windows 11, 64-bit, and a graphics card with OpenGL 4.1 and a
current driver.

1. **Download** `UA-Reforged-<version>-Windows-Setup.exe` (60 MB) and run it. It is not code-signed, so
   SmartScreen may say *Windows protected your PC*: click **More info**, then **Run anyway**.
   It installs for your account only (no administrator rights), into your Programs folder under AppData, with a
   Start menu entry. Want it somewhere else? The `-Windows-x64.zip` has the same files: unzip it anywhere and
   start `UA Reforged.exe`.
2. **Set up everything.** The first start shows one dialog: click **Set up everything** (the game, 445 MB, and
   the Reforge pack, 1.6 GB, in one progress window), or **I already have the game** to choose your folder.
   Untick **Include the Reforge pack** to keep the 1998 look for now.
3. **Play.** Every start shows a small launcher: pick the campaign and click **Play**. Pick a preset in
   *Options > Graphics*.

## Meta Quest 3

UA Reforged VR is a separate app for the headset, with the Command Deck (the battlefield as a table-top
miniature) and a New Look / Original switch. It is not in the Meta Horizon Store, so you install it yourself
("sideloading"). Built and tested on the Quest 3; the Quest 3S has the same chip and should work, but is
untested. Quest 2 and Quest Pro are untested.

**You need** the headset, a USB-C cable that carries data, a Mac or Windows PC, and Developer Mode (free).

1. **Turn on Developer Mode (once).** Sign in at [developers.meta.com/horizon](https://developers.meta.com/horizon/)
   with your headset's account and create an organisation (any name; Meta may ask you to confirm the account
   first). Then in the **Meta Horizon** phone app: *Devices > your headset > Headset settings > Developer mode > On*.
   Restart the headset. Meta's own guide:
   [set up your headset for development](https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/).
2. **Connect it.** Plug the headset into your computer, put it on and choose **Allow** for USB debugging
   (tick *Always allow from this computer*).
3. **Install the app.** Pick one:
   - **The easy way: from UA Reforged on your computer.** Start the desktop app and click **Install on my
     Quest**: it is on the first-start dialog and on the launcher every start, and the app offers it by itself
     when it sees a headset. Your computer does not need the game for this. On Windows the Start menu shortcut
     **Install UA Reforged VR on my Quest** does the same. The app downloads and checks the
     VR app and, the first time, Google's Android platform tools (adb), installs or updates the app, and can
     copy your game files to the headset. Nothing else on the headset changes. On Windows you may need Meta's
     [Oculus ADB Drivers](https://developers.meta.com/horizon/downloads/package/oculus-adb-drivers/).
   - **SideQuest**: install [SideQuest](https://sidequestvr.com/setup-howto) (the Advanced Installer), wait for
     the green dot, drag `UA-Reforged-VR-<version>-Quest3.apk` onto its window.
   - **adb**: with Google's [Platform Tools](https://developer.android.com/tools/releases/platform-tools):
     `adb install -r UA-Reforged-VR-<version>-Quest3.apk`
4. **Start it.** In the headset: *App Library >* the filter at the top right *> Unknown Sources > UA Reforged VR*.
5. **First launch.** Point a controller at a button and pull the trigger. Choose **Get the game** (fetched onto
   the headset from the Metropolis Dawn community's server), or copy your own game folder into
   `Android/data/io.github.ameliaaura.uareforged.vr/files/ua` and choose **Check again**. Then say yes to the
   **Reforge pack for Quest** for the new look. Use Wi-Fi and keep the headset on its charger while it downloads.

In the game: *Pause > Settings > LOOK* switches between New Look and Original (from the next level);
*Pause > Settings > DISPLAY* sets the refresh rate (90 Hz by default). Hold the Meta button on the right
controller to recentre.

Step-by-step with pictures: the site's [Quest guide](https://uareforged.pages.dev/downloads/#quest-computer-h).

## Anonymous usage stats (optional)

On the first start after installing or updating, UA Reforged asks once: **"Help make UA Reforged better?"**
With **Yes, happy to help** the app sends a crash report if the game crashes, a short performance
summary after each mission, a few error codes and a count of how many people play. **No thanks** (or closing the
question) keeps it off; nothing is pre-selected. Change it any time:

- **Mac / Windows:** *Options* > **SEND ANONYMOUS USAGE STATS: ON / OFF** (under the text at the top; it applies
  at once).
- **Quest:** *Pause > Settings > CONTROLS, PRIVACY >* **SEND ANONYMOUS USAGE STATS**.

No name, no IP, no files, no personal data. Turning it off deletes the random install number, anything not yet
sent and any crash file. Everything sent, field by field: [usage-stats.md](usage-stats.md).

## Game controllers (desktop)

Any controller SDL knows works (DualSense, DualShock 4, Xbox and most others). Plug it in before starting
the game.

| Button | Does |
|---|---|
| Left stick | steer; tanks: aim the gun up / down (or drive, see below); aircraft: nose up / down |
| R1/L1 (RB / LB) | drive forward / back; aircraft: faster / slower |
| Right stick, D-pad | aim the gun (tanks) |
| R2/L2 (RT / LT) | fire weapon / fire machine gun |
| Triangle (Y) | missile camera |
| Cross (A) | brake |
| Options (Menu) | the in-mission menu (Esc) |
| Share / Create, touchpad click (View on Xbox) | pause in single player; press again to continue |

*Options > Input* and the in-mission menu's **CONTROLLER SETTINGS** have:
- **Tank stick layout: AIM** (ticked): the left stick aims the gun, R1/L1 drive. **DRIVE** (not ticked): the
  left stick drives, the right stick aims, and R1/L1 also drive while the left stick is centred.
- **Invert vertical aim (tanks)** and **Invert vertical aim (aircraft)**: off = push the stick up and the gun /
  nose goes up; on = the other way round (flight-sim style). The tank setting also covers station guns and
  the host station's view. Keyboard and mouse keep their own keys.

In single player the in-mission menu holds the mission while it is open. Settings changed there are saved
to your profile straight away, without saving the mission.

## The Reforge pack

The pack is the new look: rebuilt units and host stations, buildings, skies, HD textures, restored videos, sharp
menus and the detailed floor. It is optional; without it the game keeps its 1998 look.

- **Desktop:** *Set up everything* on first start includes it: 1.6 GB in two parts, 2.4 GB on disk (about
  4.5 GB free while it installs). It checks every part's SHA-256, sets the pack up (about 30 seconds) and starts
  the game. Cancelling is fine; the next start resumes. Skipped it? The launcher shows **Get the Reforge pack**
  while none is installed.
- **By hand:** download the parts from the Downloads page and unzip them into the app's data folder so that
  `reforge-pack/hd-models/` exists. The next start finishes the setup.
- **Quest:** the headset downloads its own version of the pack, the Reforge pack for Quest (712 MB, textures made
  for the Quest's GPU).

The pack has its own licence (see the [README](../README.md#licences)).

## Graphics presets

*Options > Graphics* (desktop): **Original** draws the game exactly as in 1998. **Low**, **Medium**, **High**
and **Ultra** add the Reforged renderer (shadows, glow, trails, battle marks, skies and atmosphere) and, with the
pack, the new models and textures. A short benchmark on first launch picks one for you.

## Where things are saved

| | Data folder | Your saves and settings |
|---|---|---|
| macOS | `~/Library/Application Support/UA Reforged/` | `…/game/` |
| Windows | `%LOCALAPPDATA%\UA Reforged\` | `…\game\` |
| Quest | `Android/data/io.github.ameliaaura.uareforged.vr/files/` | inside the app's folder |

The app only **reads** your game folder; it never changes it. To choose a different game folder, delete
`game-folder.txt` in the data folder and start the app again.

## Uninstalling

- **macOS:** move *UA Reforged.app* to the Bin. To remove the downloaded game, pack and saves too, delete
  `~/Library/Application Support/UA Reforged/`.
- **Windows:** *Settings > Apps > UA Reforged > Uninstall* (or *Uninstall UA Reforged* in the Start menu). It
  asks whether to delete UA Reforged's data folder (`%LOCALAPPDATA%\UA Reforged`) as well. Your own game folder is never touched.
- **Quest:** *App Library > Unknown Sources > UA Reforged VR > … > Uninstall*. This also removes the downloaded
  game and pack from the headset.

## Something went wrong

- **"Files are missing" after choosing a folder:** you picked the wrong level of folder. Choose the one that
  holds `DATA` and `LEVELS` directly: not the zip, not a folder above it, not `DATA` itself.
- **A download broke:** the app says what went wrong, cleans up and offers the help page. Try again later, or
  choose a folder instead.
- **The game stopped:** the notice has a **Show log in Finder** / **Show log in Explorer** button that opens the
  folder with the log file.
- **Logs** (attach them to a [bug report](https://github.com/ameliaaura/uareforged/issues/new/choose)):
  - macOS: `~/Library/Application Support/UA Reforged/launcher.log` and `…/game/ENV/ypa_log.txt`
  - Windows: `%LOCALAPPDATA%\UA Reforged\launcher.log`, `engine.log` and `game\ENV\ypa_log.txt`
  - Quest: `adb logcat -d -s UAVR`
- **Questions:** [Discussions > Q&A](https://github.com/ameliaaura/uareforged/discussions/categories/q-a).

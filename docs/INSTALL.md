# Installing and playing UA Reforged

UA Reforged is free. It needs **the original Urban Assault game files** (the folders `DATA` and `LEVELS`).
They belong to Microsoft, so we never host or ship them. Use your own retail CD install, or let the app fetch
the community **OpenUA Standalone Package** (the full game plus the Metropolis Dawn add-on, about 445 MB) from
the Metropolis Dawn community's own server. More in [GAME-DATA.md](GAME-DATA.md).

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

1. **Download** `UA-Reforged-<version>-macOS-arm64.zip`, unzip it and drag **UA Reforged.app** into *Applications*.
2. **Open it the first time.** The app is not notarised by Apple, so macOS asks once: Control-click
   (or right-click) *UA Reforged*, choose **Open**, then **Open** again. On macOS 15 and later you may need
   *System Settings > Privacy & Security*, scroll down to "UA Reforged was blocked" and click **Open Anyway**.
3. **Game files.** Choose **I have the game - choose folder** and pick the folder that holds `DATA` and
   `LEVELS`, or **Get the game** to download the community package.
4. **The new look.** Say **Download** when the app offers the Reforge pack (see [below](#the-reforge-pack)).
5. Pick a preset in *Options > Graphics* and play.

## Windows

**Needs** Windows 10 (version 1803 or newer) or Windows 11, 64-bit, and a graphics card with OpenGL 4.1 and a
current driver.

1. **Download** `UA-Reforged-<version>-Windows-Setup.exe` and run it. It is not code-signed, so SmartScreen may
   say *Windows protected your PC*: click **More info**, then **Run anyway**.
   It installs for your account only (no administrator rights), into `%LOCALAPPDATA%\Programs\UA Reforged`,
   with a Start menu entry. Prefer no installer? The `-Windows-x64.zip` has the same files: unzip it anywhere
   and start `UA Reforged.exe`.
2. **Game files.** Choose **I have the game - choose folder** (the folder with `DATA` and `LEVELS`) or
   **Get the game**.
3. **The new look.** Say **Download** when the app offers the Reforge pack.
4. Pick a preset in *Options > Graphics* and play.

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
   - **The easy way: from UA Reforged on your computer.** Start the desktop app with the headset plugged in;
     it offers **Install on my Quest**. Any time later: hold **Option** while opening the app on a Mac, or use
     the Start menu shortcut **Install UA Reforged VR on my Quest** on Windows. The app downloads and checks the
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

## The Reforge pack

The pack is the new look: rebuilt units and host stations, buildings, skies, HD textures, restored videos, sharp
menus and the detailed floor. It is optional; without it the game keeps its 1998 look.

- **Desktop:** on first start (after the game folder) the app offers to download it, about 1.6 GB in two parts
  and 2.4 GB on disk. It checks every part's SHA-256, sets the pack up (about 30 seconds) and starts the game.
  Cancelling is fine; the next start resumes. *Don't ask again* stops the offer; delete the file
  `reforge-pack-offer-off` in the app's data folder to get it back.
- **By hand:** download the parts from the Downloads page and unzip them into the app's data folder so that
  `reforge-pack/hd-models/` exists. The next start finishes the setup.
- **Quest:** the headset downloads its own version of the pack (textures made for the Quest's GPU).

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
  asks whether to delete `%LOCALAPPDATA%\UA Reforged` as well. Your own game folder is never touched.
- **Quest:** *App Library > Unknown Sources > UA Reforged VR > … > Uninstall*. This also removes the downloaded
  game and pack from the headset.

## Something went wrong

- **"Files are missing" after choosing a folder:** you picked the wrong level of folder. Choose the one that
  holds `DATA` and `LEVELS` directly: not the zip, not a folder above it, not `DATA` itself.
- **A download broke:** the app says what went wrong, cleans up and offers the help page. Try again later, or
  choose a folder instead.
- **Logs** (attach them to a [bug report](https://github.com/ameliaaura/uareforged/issues/new/choose)):
  - macOS: `~/Library/Application Support/UA Reforged/launcher.log` and `…/game/ENV/ypa_log.txt`
  - Windows: `%LOCALAPPDATA%\UA Reforged\launcher.log`, `engine.log` and `game\ENV\ypa_log.txt`
  - Quest: `adb logcat -d -s UAVR`
- **Questions:** [Discussions > Q&A](https://github.com/ameliaaura/uareforged/discussions/categories/q-a).

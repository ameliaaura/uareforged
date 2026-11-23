# Changelog

What changed for players in each release. The format follows [Keep a Changelog](https://keepachangelog.com/);
versions follow [Semantic Versioning](https://semver.org/) loosely (0.x: anything may still change).
Desktop apps (macOS, Windows) and the Quest app have their own version numbers.

## [Unreleased]

## [0.8.5] - Desktop - 2026-11-24

### Added
- **Linux edition (new, x86_64)**: an AppImage and a tar.gz folder for desktop Linux with glibc 2.35 or newer
  (Ubuntu 22.04+, Mint 21+, Debian 12+, Fedora 36+, SteamOS 3.5+). The same one-step setup as on Mac and Windows
  (the game from the Metropolis Dawn community, the checked Reforge pack download), the first-start benchmark,
  the usage stats question and *Report a bug*. Its dialogs work with a game controller. No *Install on my Quest*.
- **Desktop: a short notice when a new version is out, with one-click Update now.** At start-up the app checks the
  small links file it already reads from the UA Reforged site; when a newer version is listed there, one dialog says
  so before the game starts: Update now, Later, Don't remind me for this version, and What's new. Update now
  downloads the new version, checks its signature (every update is signed with the UA Reforged update key, so the
  site alone can never push one) and installs it: on a Mac it swaps the app (the old one is kept until the new one has
  started), on Windows it runs the installer silently into the same folder, on Linux the AppImage replaces itself
  (the tar.gz folder gets the download page). Then the new version starts. If anything goes wrong the app says so,
  opens the download page and your current version starts as usual. Offline or anything unexpected: no notice and
  no wait. The check sends nothing about you or your game.

### Changed
- **Desktop: the game speed no longer depends on the frame rate.** The game used to run faster the higher the
  frame rate (about 8 % at 144 fps, 17 % at 240 fps, twice as fast at 1000 fps), which is why it was held at
  60 fps. It now runs at exactly its old 60 fps speed at any frame rate, from 20 fps to 1000. Aircraft bank as
  they did at 60 fps, guns no longer recharge slower and tanks still dodge at high frame rates; steering (keys,
  sticks, mouse), brakes, drifting host stations and the AI's map scans also behave as at 60 fps.
- **Desktop: 120 Hz and 144 Hz screens now get their full frame rate.** The frame cap follows the screen's
  refresh rate (with vsync) instead of 60, exactly (144 Hz = 144 fps), and follows the window to another screen.
- **Desktop: faster.** The Mac and Windows engines are now built with the compiler's optimiser, as the Quest
  app always was.

### Fixed
- **No more crash when holding the mouse on Yes or No in the in-mission confirm box** and dragging off it
  (seen on Windows).
- **No more crash drawing the map's lines** (seen on Windows after a long session).
- **The main menu's buttons no longer lose their labels** after the first-start graphics benchmark (or any level
  started straight from the title screen) until the game is restarted.
- **Downloads carry on where they stopped.** When the game download (from the Metropolis Dawn community), the
  Reforge pack or the VR app is cut short, stalls or the connection drops, the app now carries on from where it
  stopped instead of reporting the download as broken and starting again: automatically a few times, then with a
  Try again button, and also after Cancel or the next time the app starts. A file is only used once its size (and
  checksum, where known) are right; a partial download of an older version of a file is never used.
- **Russian and Japanese menus no longer show empty boxes with the modern menu style** (High, Ultra or
  *STYLE: MODERN*). The modern style's font has Latin letters only; a language it cannot write now keeps its own
  font in the modern menus. English, German and French keep the modern font.
- **No more crash pressing New on the save screen** when a profile is named just "UNNAMED" (no number after it).
- **"." and ".." are no longer accepted as a profile name** (they saved outside the save folder).
  Both fixes come from upstream OpenUA (Marisa-Chan).

## [0.8.4] - Desktop - 2026-11-22

### Fixed
- **Desktop: no more crash when a night mission loads with ATMOSPHERE on** (Medium and up). The game could close
  at the start of a night map, most often on the second mission of a session (seen on Windows). Freed model
  resources were left in a list that the night window lights read at the sky load.
- **Desktop: no more crash when buildings are destroyed** (seen on Windows, e.g. level 2). A building's explosion
  could get itself back as its own debris and then read past the end of its effect list.
- **Desktop: no more crash from a particle effect on some units** (seen on Windows on level 20 against the
  Mykonians). Some 1998 smoke and fire emitters name a point their model does not have; they now emit from the
  unit's centre.
- **Windows: no more crash with ATMOSPHERE High or Ultra on maps with weather** (dust, rain, snow) when GLOW is
  on, seen with NVIDIA graphics (for example level 75).
- **Windows laptops with two graphics chips now run the game on the fast one** (NVIDIA Optimus, AMD switchable
  graphics), not on the built-in chip.
- **Mac and Windows: the first start now runs the short graphics benchmark and picks a preset** for your computer
  (with the Reforge pack installed). It never ran before, so new players started with the expensive defaults. A
  preset you already chose is kept. If you have played before but never saved anything under *Options >
  Graphics*, you get this one-time benchmark of about 20 seconds on your first 0.8.4 start. Press Esc to skip it:
  nothing is saved then, it offers itself once more on the next start and then stops asking. If it ever fails to
  finish, the next start skips it and uses Medium.

## [0.8.3] - Desktop - 2026-11-16

### Added
- **Desktop: change what every game controller button does.** *Options > Input* has a new **CONTROLLER
  BUTTONS** button, and so does the in-mission menu's *CONTROLLER SETTINGS* page. Pick an action, press the pad
  button you want for it. Any button works, triggers, D-pad, stick clicks, Share / View, the touchpad and paddles
  included. Besides firing, driving, braking, the missile camera, pause and the menu, a pad can now also do the
  keyboard hotkeys: map, squadron manager, orders, next unit, jump to the host station, aggressiveness and more.
  A button another action already uses swaps with it. Works with the controller itself (D-pad, A to choose, B to
  go back) as well as the mouse and keys; *Reset to defaults* is on the page. Saved in your profile, applies at
  once. Start / Options always opens the in-mission menu, so you cannot lock yourself out. Nothing changes until
  you change something: the defaults are the old buttons. VR keeps its own controls.

### Fixed
- **Desktop: menus and loading screens no longer come out garbled after changing the screen resolution**
  (seen on Windows). The screen after the change, often the campaign map with its tip, showed smeared
  pieces of old pictures and noise behind the text. The hi-res text layer now starts out clean whenever
  the resolution or the window size changes.

## [0.8.2] - Desktop / [0.9.2] - Quest - 2026-11-11

### Fixed
- **City walls: the 1998 artists' hidden joke graffiti no longer shows on the original textures** (it was already
  gone from the sharp textures). Some buildings showed lettering such as "GURU MEDITATION ERROR" and a signature
  along the bottom of a wall. Now cleaned on the Quest and on desktop with sharp textures off, with the same
  touch-up the sharp textures got; nothing else in the look changes, and there is nothing new to download.
  *Wall Signatures* (off by default) still brings the signature back.

## [0.8.1] - Desktop / [0.9.1] - Quest - 2026-11-09

### Added
- **Anonymous usage stats, optional and off unless you say yes.** The first start asks once ("Help make UA
  Reforged better?"; nothing pre-selected, closing it = no); *Options > Send anonymous usage stats* (desktop) and
  *Pause > Settings > CONTROLS, PRIVACY* (VR) change it any time. When on: a crash report on the next start
  after a crash, a performance summary after each mission, how each mission ended and how long it took to load,
  how long the app was open, which kind of controls were used, a coarse description of the computer or headset
  (memory and cores in steps, refresh rate, screen size step), in VR the time at the war table and inside units, a
  few error codes, and a count of app and mission starts. No name, no IP, no files, no personal data. Exactly what
  is sent: [docs/usage-stats.md](docs/usage-stats.md).
- **Report a bug** (desktop *Options*, VR *Pause* menu) opens the site's feedback form in the browser with a short
  reference; with usage stats on, the game sends that session's diagnostics under the same reference so the note
  and the details can be matched. The note itself is typed on the website.

## [0.9.0] - Quest - 2026-11-08

The first public release of **UA Reforged VR** for the Meta Quest 3.

### Added
- **UA Reforged VR**, a standalone Quest app (sideloaded; not in the Meta Horizon Store). One app with a
  **New Look / Original** switch (*Pause > Settings > LOOK*).
- **Command Deck**: the live battlefield as a table-top miniature, with commander tools, and a dive into any
  of your units.
- 90 Hz by default (*Pause > Settings > DISPLAY*), comfort options and controller support for both hands.
- In-headset first launch: **Get the game** (the community OpenUA Standalone Package, fetched from the Metropolis
  Dawn community's server onto the headset) and the **Reforge pack for Quest**, with the same link, size and
  checksum checks as the desktop apps. Or copy your own game folder over USB.
- Licence texts and the source offer inside the app.

### Known limits
- Tested on the Quest 3. The Quest 3S should work but is untested; Quest 2 and Quest Pro are untested.
- In-game videos are skipped on the Quest.

## [0.8.0] - Desktop - 2026-11-08

The first public release of the desktop apps.

### Added
- **macOS app** for Apple Silicon (macOS 11 or newer): a self-contained app with every library inside.
- **Windows version** (Windows 10 1803+ and 11, 64-bit): an installer that needs no administrator rights, and
  a zip with the same files.
- **Reforged renderer**: sun shadows, glow, unit trails, battle marks, remastered skies with 3D clouds,
  per-level light and haze, lit windows at night, and a detailed floor.
- **Graphics presets**: Original, Low, Medium, High and Ultra in *Options > Graphics*, with a short benchmark on
  first launch. **Original** draws the game exactly as in 1998.
- **Reforge pack** support: rebuilt units and host stations, buildings, skies, HD textures, restored videos,
  sharp menus and the floor. The app offers a one-click download on first start and checks every part.
- **Set up everything**: with no game folder yet, one dialog and one progress window fetch the community OpenUA
  Standalone Package (from the Metropolis Dawn community's server) and the Reforge pack. Your own CD install works
  too (*I already have the game*), and the pack can be skipped.
- **Launcher** at every start: pick the campaign (the last one is remembered), *Play*, *Install on my Quest*, and
  *Get the Reforge pack* while none is installed.
- **Safe downloads**: link check, resume after a dropped connection, size and SHA-256 checks, plain messages
  and a help page when something breaks; nothing is left half set up.
- **Install on my Quest**: the desktop apps can install or update UA Reforged VR on a Quest over USB, and copy
  your game files to the headset. A button on the first-launch dialog and the launcher; the computer does not
  need the game for it.
- Dialogs show no file paths (your account name stays off streams and screenshots); *Show log in Finder* /
  *Show log in Explorer* where a log helps.
- Sharp UI text, DualSense and other SDL game controllers.
- **Controller settings** (*Options > Input* and the in-mission menu's *CONTROLLER SETTINGS*): *Tank stick
  layout* AIM (the stick aims, R1/L1 drive) or DRIVE (the stick drives; R1/L1 drive too), and *Invert
  vertical aim* for tanks and for aircraft. By default pushing the stick up aims up.
- **Pause with the controller** in single player (Share / Create, the touchpad click, or View on Xbox), and the
  in-mission menu holds the mission while it is open.

### Unchanged on purpose
- Gameplay, units, levels and factions are the 1998 game's. The campaign map keeps its 1998 picture.
- Your game folder is only read, never changed.

### Known limits
- The Mac app is not notarised: the first start needs *System Settings > Privacy & Security > Open Anyway*
  (an administrator's password). The Windows installer is not code-signed:
  SmartScreen needs *Run anyway*.
- Intel Macs and Linux are not supported in this release.

[Unreleased]: https://github.com/ameliaaura/uareforged/compare/v0.8.5...HEAD
[0.8.5]: https://github.com/ameliaaura/uareforged/releases/tag/v0.8.5
[0.8.4]: https://github.com/ameliaaura/uareforged/releases/tag/v0.8.4
[0.8.3]: https://github.com/ameliaaura/uareforged/releases/tag/v0.8.3
[0.9.2]: https://github.com/ameliaaura/uareforged/releases/tag/v0.9.2
[0.8.2]: https://github.com/ameliaaura/uareforged/releases/tag/v0.8.2
[0.9.1]: https://github.com/ameliaaura/uareforged/releases/tag/v0.9.1
[0.8.1]: https://github.com/ameliaaura/uareforged/releases/tag/v0.8.1
[0.9.0]: https://github.com/ameliaaura/uareforged/releases/tag/v0.9.0
[0.8.0]: https://github.com/ameliaaura/uareforged/releases/tag/v0.8.0

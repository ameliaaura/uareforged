# Changelog

What changed for players in each release. The format follows [Keep a Changelog](https://keepachangelog.com/);
versions follow [Semantic Versioning](https://semver.org/) loosely (0.x: anything may still change).
Desktop apps (macOS, Windows) and the Quest app have their own version numbers.

## [Unreleased]

Nothing yet.

## [0.9.0] - Quest - not yet released

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

## [0.8.0] - Desktop - not yet released

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

[Unreleased]: https://github.com/ameliaaura/uareforged/compare/v0.9.0...HEAD
[0.9.0]: https://github.com/ameliaaura/uareforged/releases/tag/v0.9.0
[0.8.0]: https://github.com/ameliaaura/uareforged/releases/tag/v0.8.0

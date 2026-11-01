# UA Reforged engine

A native macOS (Apple Silicon) build of **OpenUA**, the open-source re-implementation of the
*Urban Assault* (1998) engine, with the **Reforged** visual upgrade: a modern renderer
(shadows, glow, trails, battle marks, remastered skies and atmosphere, sharp UI text),
graphics presets with an in-game menu, and a loader for optional high-detail model packs.

It is the plain 1998 game, modernised in its visuals. It adds no units, levels or factions.

**You need your own copy of Urban Assault.** This repository contains no game data: no
levels, textures, models, sounds or videos from the original game. Those belong to
Microsoft. See [docs/GAME-DATA.md](docs/GAME-DATA.md).

This is an unofficial, non-commercial fan project. It is not affiliated with or endorsed by
Microsoft.

## What is in this repository

| Path | What |
|---|---|
| `src/` | The engine (C++17, SDL2, OpenGL). Builds with CMake. |
| `res/`, `fonts/`, `locale/`, `localexp/` | Engine shaders, the free fonts OpenUA ships, locale tables (from upstream OpenUA) |
| `packaging/macos/` | Script that builds the third-party libraries, the engine and an unsigned `.app` |
| `tools/reforge/texc/` | `uatexc`, the DDS (BC1/BC5) encoder used to build model packs |
| `tools/reforge/marks_atlas.cpp` | Look-dev CLI for the procedural battle-marks atlas |
| `docs/` | Build notes, how to supply game data, the Reforge settings, the upstream README |

## Building on macOS

Short version (Apple Silicon, macOS 11 or later, Xcode command line tools, CMake,
`pkg-config`):

```sh
packaging/macos/build-mac.sh          # third-party libs -> engine -> "UA Reforged.app"
```

Full instructions, including a quick developer build against Homebrew libraries, are in
[docs/BUILD-macOS.md](docs/BUILD-macOS.md).

## Playing

1. Build or download `UA Reforged.app`.
2. Start it. On first launch it asks for your Urban Assault folder (the one that contains
   `DATA/` and `LEVELS/`). Your folder is only read, never changed. No game yet? Choose
   *Get the game*: the app fetches the community OpenUA Standalone Package from the Metropolis
   Dawn community's own server for you (see [docs/GAME-DATA.md](docs/GAME-DATA.md)).
3. Optional, the new look: with no Reforge pack installed, the app offers to download it
   (a 1.6 GB download, 2.4 GB on disk, checked against SHA-256 sums shipped in the app) into
   `~/Library/Application Support/UA Reforged/reforge-pack/`. You can also unzip the pack
   there yourself. Without it the game keeps its 1998 look. See [docs/REFORGE.md](docs/REFORGE.md).

Graphics presets (Original, Low, Medium, High, Ultra) are in *Options > Graphics*.
*Original* draws the game as it looked in 1998.

## Credits

The engine is the work of the OpenUA authors. See [CREDITS.md](CREDITS.md).

- **OpenUA / UA_source** by Marisa-Chan and contributors: https://github.com/Marisa-Chan/UA_source
- macOS fixes by Paul W. Rankin
- *Urban Assault* (1998) was developed by TerraTools and published by Microsoft.
- The Reforged changes: Amelia Aura.

## Licence

GNU General Public License, version 2 ([LICENSE](LICENSE)), the same terms as upstream
OpenUA. Bundled fonts and libraries keep their own licences; see [CREDITS.md](CREDITS.md).
If you distribute a binary built from this code, you must also offer this source.

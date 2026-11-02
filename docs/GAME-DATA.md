# Supplying your own game data

UA Reforged is an engine. It contains no Urban Assault game files: no levels, textures,
models, sounds, music or videos. Those are Microsoft's copyright, and UA Reforged never hosts or
ships them. On first start the app fetches the free community game package for you (from the
Metropolis Dawn community's own server), or uses your own copy.

## Which copies work

| Source | Status |
|---|---|
| The community **OpenUA Standalone Package** (full game plus the Metropolis Dawn expansion, unmodified original files laid out for the OpenUA engine), hosted by the Metropolis Dawn community | Tested |
| Your own retail Urban Assault CD, installed (on Windows, or with a CD-to-folder copy) | Expected to work; the folder must contain `DATA/` and `LEVELS/` |
| Other repacks | Untested |

The Metropolis Dawn campaigns (Ghorkov, Taerkasten) need the expansion data
(`dataxp/`, `levelsxp/`, `Ghor_env/`, `Taer_env/`). The base game works without them.

## Using it with the macOS app

1. Put your Urban Assault folder anywhere on your Mac.
2. Start *UA Reforged.app*. It asks for the folder on first launch.

With no game folder set, the first start shows one dialog: **Set up everything** (the default)
or **I already have the game**. *Set up everything* downloads the OpenUA Standalone Package (445 MB)
straight from the Metropolis Dawn community's own server onto your Mac, and the Reforge pack with it
unless you untick it. It is hosted
by them, not by UA Reforged, and the files belong to Microsoft; UA Reforged never hosts,
mirrors or ships them. The app checks the link first, shows progress (Cancel works; the next
start resumes), unpacks the package into `~/Library/Application Support/UA Reforged/game-original/`,
checks that `DATA/` and `LEVELS/` hold the expected files and uses that folder. If the link is
broken (moved, a web page instead of the file, a stall, a damaged zip) the app says so in plain
words and opens the UA Reforged help page instead; nothing is left half set up.

3. The app never writes to that folder. Your profiles and settings are copied once to
   `~/Library/Application Support/UA Reforged/game/`, and all saving happens there.
4. To switch folders, delete `~/Library/Application Support/UA Reforged/game-folder.txt`.

## Using it with a self-built engine

Run the engine with your Urban Assault folder as the working directory, after copying this
repository's `res/` into it (see `BUILD-macOS.md`). Work on a copy of the folder if you want
to keep it untouched: the engine writes `SAVE/`, `ENV/` and settings files there.

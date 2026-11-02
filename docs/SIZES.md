# Download and disk sizes

The one table every size in the apps, the guides, the FAQ and the README comes from. Measured on 2026-11-02 from
the release files themselves. The apps take their numbers from `packaging/macos/downloads.txt` (`game_bytes`,
`pack_disk_bytes`, the `part` lines, `quest_apk`), so a dialog and this table say the same thing.

**Units:** 1 MB = 1,000,000 bytes, 1 GB = 1,000,000,000 bytes (as macOS Finder counts). Windows Explorer counts in
units of 1,048,576 bytes, so it shows the game package as "424 MB" and the Reforge pack as "1.50 GB".

## Desktop (macOS, Windows)

| What | Download | On disk | Notes |
|---|---|---|---|
| UA Reforged for Mac (zip) | 14 MB | 32 MB | `UA-Reforged-<version>-macOS-arm64.zip`, the app only |
| UA Reforged for Windows (installer) | 60 MB | 195 MB | `UA-Reforged-<version>-Windows-Setup.exe` |
| UA Reforged for Windows (zip) | 80 MB | 195 MB | `UA-Reforged-<version>-Windows-x64.zip`, the same files |
| The game: OpenUA Standalone Package | **445 MB** (445,036,699 bytes) | 675 MB | Fetched by *Set up everything* from the Metropolis Dawn community's server (hosted by them; the package can change). The app wants 1.5 GB free before it starts. |
| The Reforge pack (desktop) | **1.6 GB** in 2 parts (700 MB + 914 MB; 1,613,679,378 bytes) | **2.4 GB** (7,285 files) | Files that many levels share are stored once and linked. About **4.5 GB free** is needed while it installs (download + unpack). |
| *Set up everything* (game + pack) | **2.1 GB** | 3.1 GB | Plus the app |
| Google's platform tools (adb) | Mac 16 MB, Windows 8 MB | about the same | Only for *Install on my Quest*, downloaded once from Google |

## Meta Quest 3

| What | Download | On the headset | Notes |
|---|---|---|---|
| UA Reforged VR (APK) | **14 MB** (14,242,171 bytes) | | Installed from a computer (*Install on my Quest*), SideQuest or adb |
| The game: OpenUA Standalone Package | **445 MB** | 660 MB | Downloaded inside the headset, or copied from the computer |
| The Reforge pack for Quest | **712 MB** (712,134,306 bytes) | 1.1 GB | The Quest's own pack (textures made for its GPU), not the desktop parts |
| Together inside the headset | **1.2 GB** | 1.7 GB | Plus room to unpack; keep about 3 GB free |

## Where each number is used

- **Apps:** the setup dialog, the launcher and the pack offer print the sizes from `downloads.txt` (game:
  `game_bytes`; pack: the sum of the `part` lines and `pack_disk_bytes`). The Quest app reads its own
  `packaging/quest/downloads.txt` (`quest_part`).
- **Docs:** [INSTALL.md](INSTALL.md), [GAME-DATA.md](GAME-DATA.md), the [README](../README.md), the read-me files
  in the downloads (`README-mac.txt`, `README-Windows.txt`, `README-quest.txt`).
- **Site:** the Downloads page and the FAQ (`src/data/sizes.ts` holds the same table).

When a release changes a file, measure it again and update this table, `downloads.txt` and the site's
`sizes.ts` together.

# Building UA Reforged for Windows

The Windows build (64-bit, Windows 10 1803+ and 11) is cross-compiled on a Mac:

    brew install mingw-w64 makensis cmake
    packaging/windows/build-windows.sh            # all: engine, stage, installer, zip
    packaging/windows/build-windows.sh engine     # or one step: deps | engine | stage | installer | zip

Output in `build/windows/`: `UA-Reforged-<version>-Windows-Setup.exe` (NSIS, per-user, no administrator
rights) and `UA-Reforged-<version>-Windows-x64.zip` (the same files), each with a `.sha256`.
Neither contains game data.

## Dependencies
`packaging/windows/fetch-deps.py` builds a sysroot from MSYS2 `ucrt64` binary packages (SDL2 and its add-ons,
OpenAL Soft, Lua, Ogg/Vorbis) and the BtbN LGPL shared build of FFmpeg 8.1. Set `UA_WIN_DEPS=<dir>` to reuse one.
The build walks the engine's DLL imports and stops if anything outside the bundle is not a Windows system DLL.
`licenses/THIRD-PARTY.txt` lists which package each bundled DLL comes from.

## Engine changes for Windows
All are behind `#ifdef _WIN32`; other platforms compile exactly as before.
- `src/winport.h`: `mkdir(path, mode)`, `realpath()` (absolute path with `/` separators) and `strcasestr()`
  stand-ins; `near`/`far` (empty macros in `windows.h`) are undefined; `uaIsAbsPath()` accepts `C:/...`.
- `src/obj3d.cpp`: on-disk model, material and map paths are absolute when they start with a drive letter too.
- `src/system/gfx_glow.cpp`: `glBlendColor` (OpenGL 1.4) is loaded at run time; `opengl32.dll` exports 1.1 only.
- `src/vectors.h`: `<stdint.h>`.
- `src/main.cpp`: console output unbuffered, so `engine.log` is complete after a crash.

## Launcher
`UA Reforged.exe` (`packaging/windows/stub/`) starts `launcher\UA-Reforged.ps1` with Windows PowerShell, with no
console window. The script does what `packaging/macos/launcher.sh` does on the Mac:
- data in `%LOCALAPPDATA%\UA Reforged\` (`game\` is the engine's working folder; the player's game folder is
  linked in with directory junctions and never written to);
- "I have the game - choose folder" or "Get the game" (the community OpenUA Standalone Package, fetched from the
  Metropolis Dawn community's server), and the one-click Reforge pack download, in `launcher\downloads.ps1`
  (the twin of `packaging/macos/downloads.sh`: the same `downloads.txt`, `links.json` rules and failsafe; it
  uses the `curl.exe` that ships with Windows);
- the pack's shared files are rebuilt as NTFS hard links (copies where hard links fail).

Test switches (environment): the ones listed in `downloads.ps1`, plus `UA_DATA_DIR=<folder>` instead of
`%LOCALAPPDATA%\UA Reforged`.

## Uninstall
Removes the program files, shortcuts and the Add/Remove Programs entry. It asks whether to delete
`%LOCALAPPDATA%\UA Reforged` too (silent: `Uninstall.exe /S /PURGE`); junctions are removed as links, so the
player's own game folder is never touched.

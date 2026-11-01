# Building on macOS (Apple Silicon)

## Requirements

- An Apple Silicon Mac (M1 or later), macOS 11 or newer
- Xcode command line tools (`xcode-select --install`)
- CMake 3.16+ and `pkg-config` (for example from Homebrew: `brew install cmake pkg-config`)
- About 2 GB of free disk for the third-party library build

## Release build: the self-contained app

```sh
packaging/macos/build-mac.sh
```

This runs four steps, each skipped when already done (`FORCE=1` redoes everything):

1. **deps**: downloads and builds SDL2, SDL2_image, SDL2_ttf, SDL2_net, libogg, libvorbis,
   Lua 5.5 (static) and a decode-only, LGPL FFmpeg 8, all for macOS 11+, into
   `build/macos/deps/prefix`. The sha256 of every downloaded tarball is recorded.
2. **engine**: builds `src/` with CMake against that prefix (never against Homebrew).
3. **app**: assembles `build/macos/UA Reforged.app` with the libraries in
   `Contents/Frameworks`, the engine's shaders and fonts, the licences, and an ad-hoc
   signature. No game data goes into the app.
4. **zip**: `build/macos/UA-Reforged-<version>-macOS-arm64.zip` plus its `.sha256`.

To reuse a deps prefix you already built: `UA_DEPS_PREFIX=/path/to/prefix packaging/macos/build-mac.sh`.

## Developer build against Homebrew libraries

```sh
brew install cmake pkg-config sdl2 sdl2_image sdl2_ttf sdl2_net libvorbis lua ffmpeg@8
PKG_CONFIG_PATH=/opt/homebrew/opt/ffmpeg@8/lib/pkgconfig \
  cmake -S src -B build/dev -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/dev -j
```

Run it from inside your Urban Assault folder (the engine uses its working directory):

```sh
cd "/path/to/your/Urban Assault" && /path/to/build/dev/UA_source
```

Copy this repository's `res/` over the folder's `res/` first (the Reforged renderer needs
its shader versions). A Homebrew build is for development only: it depends on your Homebrew
install and is not suitable for giving to other people.

## Notes

- The engine needs an OpenGL 4.1 core context (all Apple Silicon Macs have one).
- Linux and Windows builds follow upstream OpenUA (`make nix`, `make msys`, see
  `docs/UPSTREAM-README.md`); they are untested with the Reforged changes.

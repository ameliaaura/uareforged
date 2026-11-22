# Building the Linux edition (new in 0.8.4)

The Linux edition is an x86_64 AppImage plus a tar.gz with the same files. It is built in a container, so any
machine with Docker can build it:

```sh
packaging/linux/docker-build.sh            # -> build/linux/UA-Reforged-<version>-Linux-x86_64.{AppImage,tar.gz}
packaging/linux/docker-build.sh all <dir>  # another output folder
```

- The container is Ubuntu 22.04 (`packaging/linux/Dockerfile`): glibc 2.35, so the result runs on that and newer
  (Ubuntu 22.04+, Mint 21+, Debian 12+, Fedora 36+, SteamOS 3.5+). Build on an x86_64 machine. Under amd64
  emulation on Apple Silicon (Rosetta) the compiler crashes now and then, and a crash inside a configure check
  silently gives a wrong configuration (seen with FFmpeg), so release builds are not made that way.
  The build tree lives in the Docker volume `ua-reforged-linux-work` (remove it for a clean build).
- Without Docker: on any Ubuntu 22.04 x86_64 system (a VM or WSL works) install the packages listed in the
  Dockerfile and run `UA_OUT=<out> UA_WORK=<build tree> packaging/linux/build-linux.sh`.
- `packaging/linux/build-linux.sh` (run inside the container) builds the pinned libraries from source (SDL2,
  SDL2_image, SDL2_ttf, SDL2_net, libogg, libvorbis, OpenAL Soft, Lua, a decode-only LGPL FFmpeg), the engine,
  and `ua-dialog` (the launcher's dialogs: SDL + SDL_ttf, mouse / keyboard / game controller). Steps:
  `deps`, `engine`, `appdir`, `package` (or `all`).
- The libraries go next to the engine in `usr/lib` and are found through the engine's RPATH (`$ORIGIN/../lib`),
  never through `LD_LIBRARY_PATH`, so programs the launcher starts (the browser, the folder picker) are not
  affected. The build fails if the engine needs any other library than glibc, libstdc++, libgcc, zlib and libGL;
  SDL and OpenAL Soft load X11 / Wayland / PipeWire / PulseAudio / ALSA / udev from the system at run time.
- Also shipped as they are, pinned in `licenses/third-party-sources.sha256`: the AppImage type2 runtime and a
  static curl (used only when the system has no curl; Ubuntu's desktop install has none).
- The launcher (`AppRun` / `./ua-reforged`) is `packaging/linux/launcher.sh` + `downloads.sh`, a port of the
  macOS launcher: the same first-launch setup, links.json check (`links.py`, needs python3, else the built-in
  links), pack download with SHA-256 checks, usage stats question and test switches. The addresses and pack
  checksums are the macOS app's `packaging/macos/downloads.txt`.
- The engine is the same source as on macOS and Windows; the Linux-only parts are guarded with `__linux__`:
  the usage stats platform `linux`, the OS name from `/etc/os-release`, the settings folder
  `$XDG_DATA_HOME/UA Reforged` and the stats sender (curl as a child process).

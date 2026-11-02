# Building UA Reforged VR for Meta Quest

UA Reforged VR is the same engine built as a native Android app (arm64-v8a, `NativeActivity` + OpenXR) for the
Meta Quest 3. There is no Java code and no Gradle: one script compiles the engine and its libraries with the
Android NDK, then packs, aligns and signs the APK with the SDK's own tools.

```sh
packaging/quest/build-quest.sh            # -> build-quest/UA-Reforged-VR-<version>-Quest3.apk (+ .sha256)
packaging/quest/build-quest.sh --verify   # same, then prints badging, signature, alignment and symbol checks
```

Like the desktop apps, the APK contains **no game data**. On first launch it offers to download the community
OpenUA Standalone Package onto the headset, or the player copies their own files (see [INSTALL.md](INSTALL.md#meta-quest-3)).

## Requirements

The release is built on macOS. Linux should work with the same tools but is untested.

| Tool | Version | Set with |
|---|---|---|
| Android SDK | build-tools **36.0.0**, platforms **android-34**, cmake **3.22.1** (with ninja) | `ANDROID_SDK_ROOT` |
| Android NDK | **27.2** (27.2.12479018) | `ANDROID_NDK_HOME` |
| JDK | **17** (for `apksigner` and `keytool`) | `JAVA_HOME` |
| Host CMake | **3.26** or newer (OpenAL Soft needs it) | `HOST_CMAKE` (default: `cmake` on `PATH`) |
| Python 3 | any recent | on `PATH` |

With Android Studio's SDK Manager or `sdkmanager`:

```sh
sdkmanager "build-tools;36.0.0" "platforms;android-34" "cmake;3.22.1" "ndk;27.2.12479018"
export ANDROID_SDK_ROOT=$HOME/Library/Android/sdk          # Linux: usually $HOME/Android/Sdk
export ANDROID_NDK_HOME=$ANDROID_SDK_ROOT/ndk/27.2.12479018
export JAVA_HOME=$(/usr/libexec/java_home -v 17)           # Linux: the path of your JDK 17
```

## What the script does

1. **Dependencies** (`packaging/quest/deps/`): fetches pinned, checksummed sources and builds them as static
   libraries into `build-quest/third_party/prefix-android-arm64`: SDL2 2.32.10 (patched so file opens need no
   JNI), SDL2_image 2.8.12, SDL2_ttf 2.24.0 (vendored FreeType), SDL2_net 2.4.0, OpenAL Soft 1.25.2
   (AAudio/OpenSL ES), libogg 1.3.6, libvorbis 1.3.7 and Lua 5.5.1. The Khronos OpenXR loader for Android comes
   from its official release (`fetch_openxr.sh`). FFmpeg is not built: in-game videos are skipped on the Quest
   (`src/system/movie_stub.cpp`). This step runs once; later builds reuse the prefix.
2. **Engine**: `src/` with `-DUA_VR=ON -DUA_VR_PREVIEW=ON -DUA_VR_NEWLOOK=ON` into `libuavr.so`, plus the platform
   layer and the in-headset start screens from `platform/quest/`. Build paths are mapped out of the binary.
3. **Stage**: stripped libraries, the engine's shaders and free fonts, `downloads.txt` (download addresses and
   checksums) and the licence texts from `packaging/quest/licenses/`, with a `SOURCE.txt` that names this repository.
4. **APK**: `aapt2` (manifest + icons), `apkzip.py` (fixed entry dates, stable order, native libraries stored
   uncompressed), `zipalign -P 16` (16 KB page alignment), `apksigner`.

Options (environment): `UAVR_OUT` (default `build-quest`), `UAVR_THIRD_PARTY`, `JOBS`, `BUILD_TYPE`
(default `RelWithDebInfo`; symbols stay in the unstripped `build-quest/cmake/libuavr.so`), `RELEASE_DATE`.

## Signing

Without signing settings the script makes a throwaway debug key in `build-quest/debug.keystore`. That is fine
for your own headset.

Android only installs an update over an existing app when both are signed with the **same key**. A build you
sign yourself therefore cannot update the released UA Reforged VR, and the other way round: uninstall one
before installing the other (uninstalling also removes the downloaded game and pack from the headset).

Release builds are signed with the project key, which is not in this repository:

```sh
UAVR_KEYSTORE=/path/to/release.jks UAVR_KEY_ALIAS=<alias> UAVR_KS_PASS_FILE=/path/to/password.txt \
  packaging/quest/build-quest.sh --verify
```

The password is read from the file, never from the command line.

## Installing your build

Turn on Developer Mode on the headset, connect it over USB and allow USB debugging (see
[INSTALL.md](INSTALL.md#meta-quest-3)), then:

```sh
adb install -r build-quest/UA-Reforged-VR-*-Quest3.apk
adb logcat -s UAVR          # the app's log
```

In the headset: *App Library > Unknown Sources > UA Reforged VR*.

## Testing without a headset

The VR module also builds into the desktop engine for quick checks: configure `src/` with `-DUA_VR=ON` and run
with `UA_VR=sim`. The simulator is a development aid only; check headset behaviour (comfort, frame rate,
controllers) on a real Quest.

## The Quest pack

The Quest's GPU cannot use the desktop pack's DDS textures, so the headset downloads its own Reforge pack.
`tools/vr/make_quest_pack.py` re-encodes the pack's textures to ASTC, and `tools/vr/make_quest_full_pack.py`
joins them with the pack files the headset keeps. Both work on an unpacked Reforge pack (no original game files
go in). See [RELEASING.md](RELEASING.md).

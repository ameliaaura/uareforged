# Cutting a release

How a UA Reforged release is made. Maintainers do this; contributors only need it to understand why the build
scripts are so strict about dates, checksums and file lists.

Principles:

- **No game data anywhere.** Not in the apps, not in the release assets, not in the pack (the pack holds only
  new work and files remade from the originals, each listed with how it was made).
- **Reproducible files.** The same source gives the same zip, byte for byte: one fixed timestamp on every entry,
  normalised file modes, no owner IDs or extra fields, no macOS metadata, a stable entry order.
- **Everything checksummed.** Every downloadable file has a SHA-256, published next to it and built into the
  apps that download it.

## 1. Version and notes

1. Desktop: the last `PortVersion` line in `src/version.h`. Quest: the first one, plus `VERSION` in
   `packaging/quest/build-quest.sh`, `versionName`/`versionCode` in `packaging/quest/AndroidManifest.xml`
   (the version code must go up, or headsets will not update).
2. Release date: `RELEASE_DATE` in `build-mac.sh` and `build-quest.sh`, `FIXED_TIME` in `build-windows.sh`.
   It becomes the one timestamp in every zip and APK entry.
3. Move the `[Unreleased]` items in `CHANGELOG.md` under the new version with today's date.

## 2. Build

From a clean clone of the release commit (no local changes, no untracked files):

```sh
packaging/macos/build-mac.sh                    # build/macos/UA-Reforged-<v>-macOS-arm64.zip (+ .sha256)
packaging/windows/build-windows.sh all          # build/windows/UA-Reforged-<v>-Windows-Setup.exe and -x64.zip (+ .sha256)
UAVR_KEYSTORE=… UAVR_KEY_ALIAS=… UAVR_KS_PASS_FILE=… \
  packaging/quest/build-quest.sh --verify       # build-quest/UA-Reforged-VR-<v>-Quest3.apk (+ .sha256)
```

What makes them reproducible:

- **macOS:** `build-mac.sh` copies the app with `ditto --norsrc --noextattr --noacl`, removes extended
  attributes, signs it ad hoc again, and zips it with `packaging/macos/mkzip.py` (no extra fields, one
  timestamp, modes 755/644, no `__MACOSX` entries).
- **Windows:** every staged file is set to `FIXED_TIME`; the zip is written by a small Python step with the same
  rules. The installer is NSIS with the same staged files.
- **Quest:** `apkzip.py` writes the APK with fixed entry dates and a stable order, then `zipalign -P 16` and
  `apksigner`. The release APK must be signed with the project key, or installed copies cannot update.

Check the builds:

```sh
codesign --verify --deep --strict "build/macos/UA Reforged.app"
unzip -l build/macos/UA-Reforged-*-macOS-arm64.zip | grep -c __MACOSX   # must be 0
```

For the Quest, read the `--verify` output: signature, 16 KB alignment, and no unexpected undefined symbols.

Then play each build once: a fresh first launch (throwaway user or data folder), *Get the game*, the pack
download, one level on Original and on High.

## 3. Checksums

Each build script writes a `.sha256` next to its file. For the release, collect them into one `SHA256SUMS`:

```sh
shasum -a 256 UA-Reforged-*.zip UA-Reforged-*.exe UA-Reforged-VR-*.apk UA-Reforged-pack-*.zip > SHA256SUMS
```

Players check a download with `shasum -a 256 <file>` (macOS), `Get-FileHash <file>` (Windows PowerShell) or
`sha256sum <file>` (Linux).

## 4. The Reforge pack

The pack is not built in this repository: it is made from the project's own asset sources, and it carries its
own licence (`LICENSE.txt`: Part A CC BY-NC 4.0 for new work, Part B Microsoft's Game Content Usage Rules for
everything remade from the 1998 game).

Each pack version ships:

- `MANIFEST.tsv`: every file with its size, SHA-256, how it was made and its licence part; `PROVENANCE.md`
  explains the classes.
- `LINKS.tsv` and `install-links.sh`: files that are identical across levels are stored once; the install script
  rebuilds the other paths as hard links (`--verify` checks every SHA-256 afterwards).
- Zip parts under 2 GB each, written with the same deterministic rules as the app zips.
- A Quest version with textures re-encoded to ASTC (`tools/vr/make_quest_pack.py`,
  `tools/vr/make_quest_full_pack.py`), because the Quest's GPU cannot use the desktop DDS maps.

After the pack is zipped:

1. Put the new `part <name> <bytes> <sha256>` lines in `packaging/macos/downloads.txt` (Mac and Windows) and the
   Quest lines in `packaging/quest/downloads.txt`. Rebuild the apps: the checksums are built in.
2. Update `pack_parts` (and the Quest entries) in the site's `release/links.json`.

## 5. Publish

1. Upload the pack parts and check that a ranged download answers `206` with `application/zip`.
2. Set the download addresses (`url`, `allow_host`, `quest_apk_url`) in the `downloads.txt` files if they moved,
   commit, and rebuild (step 2). Re-check every checksum.
3. Tag the release commit `v<version>` and create the GitHub release with the notes, the app files, `SHA256SUMS`
   and, for the Quest, the APK. The GPL source is the tagged commit (each app's `SOURCE.txt` names this repository).
4. Update the site's Downloads page (addresses, checksums, sizes) and `release/links.json`.
5. Download every asset from its public link, check the SHA-256, and run a fresh first launch once more.

## 6. After the release

- Watch the issues for install problems in the first days.
- If a download address breaks, fix it in the site's `release/links.json` first (the apps read it before every
  download), then in `downloads.txt` for the next release.

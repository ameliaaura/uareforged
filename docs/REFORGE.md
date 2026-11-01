# The Reforge visual upgrade

Everything the Reforged renderer adds is drawn by the engine itself (procedural shadows,
glow, trails, battle marks, atmosphere, the sharp UI text layer). The optional Reforge pack adds
remastered unit models, buildings and skies, HD textures, restored videos, sharp UI packs and
the floor pack. Without it the game keeps its 1998 look.

## The Reforge pack in the macOS app

The app looks for the pack in `~/Library/Application Support/UA Reforged/reforge-pack/`. When
none is there it offers a one-click download (`packaging/macos/downloads.sh`): the download
address and the size and SHA-256 of every part are in `packaging/macos/downloads.txt`. Each
link is checked first, each part's checksum is checked, the parts are unpacked next to the download, the pack's `install-links.sh --verify` rebuilds
and checks the shared files, and only then the folder is moved into place. A cancelled or broken
download resumes on the next start; the player's game folder is never touched. The launcher
links `hd-models/`, `hd-textures/` and `hd-fmv/` into the game's working folder and sets
`UA_HDMODELS_DIR`, `UA_SKY_DIR`, `UA_UI_DIR` and `UA_R2_GROUND_DIR`. "Don't ask again" writes
`~/Library/Application Support/UA Reforged/reforge-pack-offer-off`; delete it to get the offer
back. A broken link (no answer, HTTP error, a web page instead of the file, wrong size, a stall
of 30 s after two automatic resumes, a bad checksum) gives a plain notice with *Open help page*
or *Try again later*, removes the partial files, and the game starts with the 1998 look.

Before any download the app reads `links_url` (the site's `release/links.json`): its
`game_url`, `game_min_bytes`, `pack_url`, `pack_parts`, `help_url`, `pack_help_url` and
`message` replace the built-in values, so a broken link can be fixed without a new app. The
file is only parsed and checked (`packaging/macos/links.js`: strict types, https only, hosts on
the `allow_host` list of `downloads.txt`); one bad value rejects the whole file and the built-in
values are used. Testing: `UA_LINKS_URL`, `UA_GAME_URL`, `UA_PACK_URL` (other addresses),
`UA_GAME_AUTO=get`, `UA_PACK_AUTO=yes|no`, `UA_NOTICE_AUTO=help|folder|later`, `UA_PACK_NOUI=1`.

The campaign map always shows the 1998 picture (the pack carries no campaign-map pictures).

## Graphics presets

*Options > Graphics* offers **Original** (the 1998 renderer, unchanged), Low, Medium, High
and Ultra, with a first-launch benchmark that picks one. The choice is saved in
`SAVE/gfx.txt`. `UA_GFX_DEFAULT=<preset>` sets the default, `UA_GFX_BENCH=0` skips the
benchmark.

## Model pack layout

The engine reads `hd-models/` in its working directory, or the folder named by
`UA_HDMODELS_DIR`:

```
hd-models/
  VP_<NAME>.obj / .mtl         one per visual prototype the pack replaces (plus VPf/VPw states)
  VP_<NAME>_albedo|normal|orm  PNG + DDS (BC1/BC5), authored far mips (_mip1.._mip5)
  fx/        fx.txt + procedural effect flipbooks and genesis (beam-in) shells
  trails/    trails.txt + trail sprites
  sky/<sky>/ sky.txt + sky_{1024,2048,4096}.dds (+ sky3d_* for 3D clouds), atmos.txt
```

A prototype with no files in the pack keeps its 1998 model. `UA_SKY_DIR` points at the sky
folder (the macOS app sets it for you).

## Optional AI commander tuning

The engine runs the 1998 enemy commander unless an `ai-tuning.txt` sits in the game
folder. `docs/ai-tuning.example.txt` documents every key; with no file present the
behaviour is the original one.

## Useful variables

| Variable | Effect |
|---|---|
| `UA_HDMODELS_DIR` | model pack folder (a missing folder turns HD models off) |
| `UA_SKY_DIR` | remastered sky folder |
| `UA_GFX_DEFAULT`, `UA_GFX_BENCH` | default preset, first-launch benchmark on/off |
| `UA_WINDOW_SIZE=WxH` | window size |
| `UA_MAXFPS` | frame cap |
| `UA_AUTOSTART_LEVEL=<n>` | jump straight into level n (testing) |

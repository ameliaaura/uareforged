# Credits and licences

## Engine

**OpenUA (UA_source)**: an open-source re-implementation of the Urban Assault engine.
Upstream: https://github.com/Marisa-Chan/UA_source. Licence: GPLv2 (see `LICENSE`).

Upstream authors and contributors (from the upstream history):

- Marisa-Chan (lead author)
- MaxN
- Anton Yarcev
- zidane
- omor1
- GoldStar611

macOS port fixes (GL 4.1 core profile, libc++ build fixes): Paul W. Rankin.

The Reforged changes in this repository (macOS port work, renderer, graphics presets,
model-pack loader, the UI text layer, controller support and the optional AI commander
tuning) are by Amelia Aura and are released under the same GPLv2 terms.

## The game

*Urban Assault* (1998) was developed by TerraTools and published by Microsoft. All game
content (levels, art, models, sounds, music, videos, text) is Microsoft's and is **not**
part of this repository.

## Bundled third-party code

| Component | Where | Licence |
|---|---|---|
| {fmt} | `src/fmtlib/` | MIT (see `src/fmtlib/LICENSE.rst`) |
| any-lite | `src/any-lite/` | Boost Software License 1.0 |
| stb_image, stb_dxt | `tools/reforge/texc/` | Public domain / MIT (see file headers) |

## Fonts (in `fonts/`, from upstream)

- Liberation Mono, Sans, Serif 2.00.1: SIL Open Font License 1.1
- Xolonium by Severin Meyer: SIL Open Font License
- Press Start 2P by Cody "CodeMan38" Boisclair: SIL Open Font License
- Textar (https://github.com/yamacraft/textar-font): IPA Font License 1.0

## Libraries linked by the macOS app

Built from source by `packaging/macos/build-mac.sh`; their licence texts are copied into
the app at `Contents/Resources/licenses/`.

| Library | Licence |
|---|---|
| SDL2, SDL2_image, SDL2_ttf, SDL2_net | zlib |
| libogg, libvorbis | BSD-3-Clause |
| FFmpeg (decode-only build, no GPL or non-free parts) | LGPL 2.1 or later |
| Lua 5.5 (linked statically) | MIT |
| OpenAL (Apple system framework) | system |

## Contributors

Everyone whose pull request is merged is listed in the release notes of the version that includes it, and in
the repository's contributor graph. Thank you.

---

UA Reforged is made in Cape Town, South Africa, with thanks to everyone above.

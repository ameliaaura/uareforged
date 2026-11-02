# Contributing to UA Reforged

Thanks for wanting to help. UA Reforged is a small fan project, and every fix, test report and idea makes
it better. This page explains how to build the engine, how to send a change, and the few rules that keep
the project legal and easy to maintain.

**Quick summary**

- Small fixes (typos, docs, crashes, build breaks, clear bugs) are always welcome as a pull request.
- For a bigger feature, **open an issue first** so we can agree on the approach before you spend time on it.
- Sign off every commit: `git commit -s` (see [Sign-off](#sign-off-dco)).
- **Never commit Microsoft game data or anything made from it.** Read [Game data](#game-data-never-in-this-repository).
- Questions and ideas go to [Discussions](https://github.com/ameliaaura/uareforged/discussions); bugs go to
  [Issues](https://github.com/ameliaaura/uareforged/issues).

By taking part you agree to the [Code of Conduct](CODE_OF_CONDUCT.md).

## Ways to help without writing code

- **Play and report.** A good bug report (platform, version, steps, the log file) is worth a lot. Use the
  bug form in Issues; it tells you where the log is.
- **Test on hardware we don't have.** Intel Macs, older Windows GPUs, the Quest 3S, Quest 2 and Quest Pro are
  all untested. "It works on X" reports are useful too: post them in Discussions > Show and tell.
- **Check fidelity.** If something in the New Look does not match your memory of the 1998 game, say so,
  with a screenshot if you can.
- **Improve the docs.** If a step confused you, it will confuse the next person.

## Building

| Platform | Guide | Short version |
|---|---|---|
| macOS (Apple Silicon) | [docs/BUILD-macOS.md](docs/BUILD-macOS.md) | `packaging/macos/build-mac.sh` |
| Windows (x64, cross-built on a Mac) | [docs/BUILD-Windows.md](docs/BUILD-Windows.md) | `packaging/windows/build-windows.sh` |
| Meta Quest 3 | [docs/BUILD-quest.md](docs/BUILD-quest.md) | `packaging/quest/build-quest.sh` |

For day-to-day engine work on a Mac, the Homebrew developer build in
[docs/BUILD-macOS.md](docs/BUILD-macOS.md#developer-build-against-homebrew-libraries) is the fastest loop.
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) is a short map of the source tree.

To run the game you need your own Urban Assault files; see [docs/GAME-DATA.md](docs/GAME-DATA.md).

## Sending a change

1. **Fork** the repository and create a branch from `main`. Name it after what it does, for example
   `fix/tank-stuck-on-ramp` or `docs/windows-build-typo`.
2. **Keep it focused.** One pull request = one change. A bug fix plus an unrelated refactor is two pull requests.
3. **Build it** on at least one platform, and play the part of the game you touched. Say in the pull request
   what you tested and on what.
4. **Sign off your commits** (`git commit -s`).
5. **Open the pull request** against `main` and fill in the template. Link the issue it closes (`Fixes #123`).
6. CI builds the macOS and Windows versions and checks the sign-off. Both must pass before a merge.
7. A maintainer reviews it. Expect questions; they are not a "no". Pull requests are **squash-merged**, so
   your branch can have as many small commits as you like.

If a pull request goes quiet for a few weeks we may ask whether you still want to finish it, or finish it
ourselves with credit to you.

### Bigger features: issue first

Anything that changes how the game plays, adds a dependency, adds a platform, changes the file layout or
the pack format, or is more than a few hundred lines: please open a **feature request** first. Some ideas
do not fit the project (see [What we do not take](#what-we-do-not-take)), and it is kinder to say so before
the work, not after.

## Code style

Follow the code around you. The engine is a re-implementation of a 1998 code base and mixes several styles;
consistency within a file matters more than any global rule.

- C++17, 4-space indentation, braces as the surrounding code uses them.
- New Reforged features live in their own files where possible (`src/reforge_*.cpp`, `src/system/*`),
  with a short comment at the top that says what the file does.
- **The Original preset must stay the 1998 game.** New visuals go behind a graphics level
  (`gfxLevel()` in `src/gfxsettings.h`) and are off in Original.
- Platform code sits behind `#ifdef _WIN32`, `__APPLE__` or `__ANDROID__`; other platforms must compile
  exactly as before.
- Shell scripts: `bash` with `set -euo pipefail`. Windows launcher scripts: plain ASCII, Windows
  PowerShell 5.1 compatible (the build checks this).
- Comments say *why*, in plain English.
- Do not reformat files you are not otherwise changing; it hides the real diff.

## Sign-off (DCO)

The engine is GPLv2, and every contribution must be yours to give. We use the
[Developer Certificate of Origin](https://developercertificate.org/) (DCO) instead of a contributor licence
agreement. Adding a `Signed-off-by` line to a commit means you certify the DCO for that commit:

```
Signed-off-by: Your Name <you@example.com>
```

`git commit -s` adds it for you, using your `user.name` and `user.email`. The name can be the one you use
publicly, but the line must match the commit author. Forgot? Fix the last commit with
`git commit --amend -s --no-edit`, or a whole branch with `git rebase --signoff main`, then force-push your
branch.

Your contribution is licensed under the GNU GPL version 2, the licence of the engine.

## Game data: never in this repository

*Urban Assault* and everything in it belongs to Microsoft. This repository contains only engine code, build
scripts, free fonts and docs. Pull requests that add any of the following are closed without merging, and
the files are removed from history if they slip through:

- original game files: levels, scripts, textures, models, sounds, music, videos, text, fonts, installers,
  the demo, saves made from them;
- **anything derived from them**: upscaled or converted textures, extracted or rebuilt models, ripped audio,
  screenshots used as assets, palette or sprite dumps, the Reforge pack or parts of it;
- links in the code to unofficial copies of the game other than the community package the app already uses.

Test data for a fix should be described in the pull request ("level 1, the second wave of tanks"), not
committed. If you are not sure whether something counts, ask in Discussions first.

The Reforge pack has its own licence and is not developed in this repository.

## What we do not take

- Anything that makes the project commercial: paid features, ads, donation walls in the app, telemetry.
- Changes that alter the 1998 game in the Original preset.
- Cheats or online-play changes that break fair play between friends.
- Code under a licence that is not compatible with GPLv2.
- Large generated or binary files. Small icons and test images made by you are fine; say so in the pull request.

## Reporting security problems

Please do not open a public issue. See [SECURITY.md](SECURITY.md).

## Thank you

The engine stands on years of work by the OpenUA authors and the Metropolis Dawn community. Contributors are
credited in the release notes and in [CREDITS.md](CREDITS.md).

# Security policy

## Supported versions

Only the latest release of each app gets fixes.

| App | Supported |
|---|---|
| UA Reforged for macOS, latest 0.8.x | yes |
| UA Reforged for Windows, latest 0.8.x | yes |
| UA Reforged VR for Meta Quest, latest 0.9.x | yes |
| Anything older | no; please update |

## What counts as a security problem

UA Reforged downloads files for the player (the community game package, the Reforge pack, the Quest app and
Google's Android platform tools) and checks them before it uses them. Please report privately if you find a
way to:

- make the app download, unpack or run something other than the files listed in `downloads.txt`, for example
  by getting past the checksum, size or host checks, or the `links.json` rules;
- make an unpack write outside the app's own data folder (path traversal, links);
- make the launcher run commands from file names, folder names or `links.json` values;
- crash the engine with a crafted save, level or multiplayer packet in a way that could run code;
- install anything on a Quest headset other than UA Reforged VR, or change other apps or settings on it.

Ordinary bugs and crashes with the normal game files are not security issues: please use the
[bug form](https://github.com/ameliaaura/uareforged/issues/new/choose).

## How to report

Use GitHub's **private vulnerability reporting**: open the repository's **Security** tab and choose
**Report a vulnerability**. Only the maintainers can see the report.

If that does not work for you, email **ameliaofaura@gmail.com** with "UA Reforged security" in the subject.

Please include the version, the platform, what an attacker controls, and the steps to reproduce. A proof of
concept helps, but please do not include Microsoft game files.

## What happens next

- We aim to acknowledge a report within 7 days.
- We will tell you whether we can reproduce it and what we plan to do.
- Fixes ship in a new release with the problem described in the release notes. We are happy to credit you
  there, or to keep you anonymous; tell us which.

This is an unpaid fan project, so there is no bug bounty. Thank you for helping keep players safe.

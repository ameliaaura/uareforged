UA REFORGED - for Macs with Apple Silicon (M1 or later), macOS 11 or newer
==========================================================================

The 1998 game Urban Assault on the open-source OpenUA engine, with modern
graphics. Unofficial, free, non-commercial fan project.

YOU NEED THE ORIGINAL GAME FILES. This download contains no game files at
all: on first start the app fetches the free community game package for you,
or uses your own copy. See "Your game files" below.


INSTALLING
----------
1. Unzip the download and drag "UA Reforged.app" into Applications.
2. Open it the first time. The app is not notarised by Apple, so macOS blocks
   the first start once:
   a. Open UA Reforged. macOS says it "was blocked"; click Done.
   b. Open System Settings > Privacy & Security, scroll down to Security:
      "UA Reforged was blocked to protect your Mac". Click Open Anyway.
   c. Click Open Anyway again and confirm with Touch ID or your password.
   This needs an administrator: on a standard (non-admin) account, macOS asks
   for an administrator's name and password. You only do this once.
   (On macOS 14 and older, Control-click "UA Reforged" > Open > Open also
   works. On macOS 15 and newer it does not.)
3. Set up UA Reforged. The first start shows one dialog. Click "Set up
   everything": the app downloads the game (the free OpenUA Standalone
   Package, 445 MB, from the Metropolis Dawn community) and the Reforge pack
   (1.6 GB) and shows one progress window for both. Then pick a campaign and
   click Play.
   Already have the game? Click "I already have the game" and choose its
   folder (the one that contains DATA and LEVELS). Untick "Include the
   Reforge pack" to keep the 1998 look for now.
   UA Reforged only reads your game folder; your saves and settings are kept
   in UA Reforged's own folder (~/Library/Application Support/UA Reforged).


YOUR GAME FILES
---------------
"Set up everything" downloads the community OpenUA Standalone Package (the
full game plus the Metropolis Dawn expansion, 445 MB) straight from the
Metropolis Dawn community's server onto your Mac. It is hosted by them, not by
us, and the game files belong to Microsoft. If that link is not working, the
app says so and opens the UA Reforged help page with other ways to get the
game. You can also use your own retail Urban Assault CD install (copy the
installed folder to your Mac) with "I already have the game".
To pick a different folder later, delete
    ~/Library/Application Support/UA Reforged/game-folder.txt


THE REFORGE PACK (OPTIONAL)
---------------------------
The new look: remastered units, buildings and skies, HD textures, restored
videos, sharp menus and the floor pack (a 1.6 GB download, 2.4 GB on disk;
it needs about 4.5 GB free while it installs). "Set up everything" includes
it. The app checks every file and sets the pack up. You can cancel; the next
start offers to resume. No internet: the game starts with the 1998 look.
Skipped it? Every start shows a small launcher; while the pack is missing it
has a "Get the Reforge pack" button.
Manual install: unzip every pack part so that this folder exists:
    ~/Library/Application Support/UA Reforged/reforge-pack/hd-models/
Things the pack does not cover keep their 1998 look (the campaign map always
does).


INSTALL ON MY QUEST (UA REFORGED VR)
-----------------------------------
UA Reforged VR is the Meta Quest app. To install it from this Mac: turn on
Developer Mode for the headset (once, in the Meta Horizon phone app), plug the
headset in with a USB-C cable, open UA Reforged and click "Install on my
Quest" (on the first-start dialog and on the launcher every start; your Mac
does not need the game for this). The app also offers it by itself when it
sees a headset. It downloads the VR app (14 MB) and, the first time, Google's
Android platform tools (adb, 16 MB) from Google, checks both, installs or
updates the app, and can copy your game files to the headset so it does not
download the game again. Nothing else on the headset changes. In the headset:
App Library > Unknown Sources > UA Reforged VR.
Help: https://uareforged.pages.dev/downloads/#quest-computer-h


SIZES
-----
Mac app: 14 MB download, 32 MB installed. Game: 445 MB download, 675 MB on
disk. Reforge pack: 1.6 GB download, 2.4 GB on disk. Full list:
https://github.com/ameliaaura/uareforged/blob/main/docs/SIZES.md


GRAPHICS
--------
Options > Graphics: Original (the 1998 look), Low, Medium, High, Ultra.


LICENCE
-------
Engine: GNU GPL version 2 (LICENSE-engine-GPL-2.0.txt). The matching source
code: https://github.com/ameliaaura/uareforged (see SOURCE.txt). Third-party
licences are inside the app: UA Reforged.app/Contents/Resources/licenses/;
the font licences (SIL OFL 1.1, IPA 1.0) are next to the fonts in
UA Reforged.app/Contents/Resources/engine/fonts/. The Reforge pack has its own
LICENSE.txt.
Urban Assault and all of its content belong to Microsoft.

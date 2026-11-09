UA REFORGED - for Windows 10 (version 1803 or newer) and Windows 11, 64-bit
===========================================================================

The 1998 game Urban Assault on the open-source OpenUA engine, with modern
graphics. Unofficial, free, non-commercial fan project.

YOU NEED THE ORIGINAL GAME FILES. This download contains no game files at
all: on first start the app fetches the free community game package for you,
or uses your own copy. See "Your game files" below.


INSTALLING
----------
1. Run UA-Reforged-@VERSION@-Windows-Setup.exe.
   It is not signed, so Windows SmartScreen may say "Windows protected your
   PC". Click "More info", then "Run anyway".
2. The installer puts UA Reforged in your own account (no administrator
   rights needed, in your Programs folder under AppData), with "UA Reforged"
   in the Start menu (and on the desktop if you keep that box ticked).
   Uninstall it from Settings > Apps, or with "Uninstall UA Reforged" in the
   Start menu.
   Want it somewhere else? The zip has the same files: unzip it anywhere and
   start "UA Reforged.exe".
3. Set up UA Reforged. The first start shows one dialog. Click "Set up
   everything": the app downloads the game (the free OpenUA Standalone
   Package, 445 MB, from the Metropolis Dawn community) and the Reforge pack
   (1.6 GB) and shows one progress window for both. Then pick a campaign and
   click Play.
   Already have the game? Click "I already have the game" and choose its
   folder (the one that contains DATA and LEVELS). Untick "Include the
   Reforge pack" to keep the 1998 look for now.
   UA Reforged only reads your game folder; your saves and settings are kept
   in UA Reforged's own folder (%LOCALAPPDATA%\UA Reforged).


YOUR GAME FILES
---------------
"Set up everything" downloads the community OpenUA Standalone Package (the
full game plus the Metropolis Dawn expansion, 445 MB) straight from the
Metropolis Dawn community's server onto your PC. It is hosted by them, not by
us, and the game files belong to Microsoft. If that link is not working, the
app says so and opens the UA Reforged help page with other ways to get the
game. You can also use your own retail Urban Assault CD install with "I
already have the game".
To pick a different folder later, delete
    %LOCALAPPDATA%\UA Reforged\game-folder.txt


THE REFORGE PACK (OPTIONAL)
---------------------------
The new look: remastered units, buildings and skies, HD textures, restored
videos, sharp menus and the floor pack (a 1.6 GB download, 2.4 GB on disk;
it needs about 4.5 GB free while it installs). "Set up everything" includes
it. The app checks every file and sets the pack up. You can cancel; the next
start offers to resume. No internet: the game starts with the 1998 look.
Skipped it? Every start shows a small launcher; while the pack is missing it
has a "Get the Reforge pack" button.
Manual install: unzip every pack part into %LOCALAPPDATA%\UA Reforged\ so
that this folder exists:
    %LOCALAPPDATA%\UA Reforged\reforge-pack\hd-models\
The next start sets the pack up (it rebuilds the shared files as hard links).
Things the pack does not cover keep their 1998 look (the campaign map always
does).


INSTALL ON MY QUEST (UA REFORGED VR)
-----------------------------------
UA Reforged VR is the Meta Quest app. To install it from this PC: turn on
Developer Mode for the headset (once, in the Meta Horizon phone app), plug the
headset in with a USB-C cable, start UA Reforged and click "Install on my
Quest" (on the first-start dialog and on the launcher every start; your PC
does not need the game for this). The Start menu shortcut "Install UA
Reforged VR on my Quest" does the same, and the app offers it by itself when
it sees a headset. Windows may also need Meta's "Oculus ADB Drivers".
It downloads the VR app (14 MB) and, the first time, Google's Android
platform tools (adb, 8 MB) from Google, checks both, installs or updates the
app, and can copy your game files to the headset so it does not download the
game again. Nothing else on the headset changes. In the headset: App Library
> Unknown Sources > UA Reforged VR.
Help: https://uareforged.pages.dev/downloads/#quest-computer-h


SIZES
-----
Windows installer: 60 MB download, 195 MB installed (the zip: 80 MB). Game:
445 MB download, 675 MB on disk. Reforge pack: 1.6 GB download, 2.4 GB on
disk. Full list: https://github.com/ameliaaura/uareforged/blob/main/docs/SIZES.md


GRAPHICS
--------
Options > Graphics: Original (the 1998 look), Low, Medium, High, Ultra.
Needs a graphics card with OpenGL 4.1 or newer and a current driver.


IF SOMETHING GOES WRONG
-----------------------
The launcher writes %LOCALAPPDATA%\UA Reforged\launcher.log, the engine
%LOCALAPPDATA%\UA Reforged\engine.log and
%LOCALAPPDATA%\UA Reforged\game\ENV\ypa_log.txt.


PRIVACY
-------
Anonymous usage stats are optional and off unless you say yes (the first start
asks once; Options > SEND ANONYMOUS USAGE STATS changes it). When on: crash
reports, a performance summary after each mission, how missions end and how
long they load, how long the app is open, the kind of controls used, coarse
hardware steps (memory, cores, refresh rate, screen size), error codes and a
usage count. No name, no IP, no files, no personal data. Options > REPORT A
BUG opens the feedback form on the website in your browser. Details:
https://github.com/ameliaaura/uareforged/blob/main/docs/usage-stats.md

LICENCE
-------
Engine: GNU GPL version 2 (LICENSE-engine-GPL-2.0.txt). The matching source
code: https://github.com/ameliaaura/uareforged (see SOURCE.txt). Third-party
licences (SDL2, OpenAL Soft, Lua, Ogg/Vorbis, FFmpeg under the LGPL and the
libraries they use) are in the licenses folder; the font licences (SIL OFL
1.1, IPA 1.0) are next to the fonts in engine\fonts\. The Reforge pack has
its own LICENSE.txt.
Urban Assault and all of its content belong to Microsoft.

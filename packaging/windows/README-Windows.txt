UA REFORGED - for Windows 10 (version 1803 or newer) and Windows 11, 64-bit
===========================================================================

The 1998 game Urban Assault on the open-source OpenUA engine, with modern
graphics. Unofficial, free, non-commercial fan project.

YOU NEED YOUR OWN COPY OF URBAN ASSAULT. This download contains no game
files at all. See "Your game files" below.


INSTALLING
----------
1. Run UA-Reforged-@VERSION@-Windows-Setup.exe.
   It is not signed, so Windows SmartScreen may say "Windows protected your
   PC". Click "More info", then "Run anyway".
2. The installer puts UA Reforged in your own account (no administrator
   rights needed), by default in
       %LOCALAPPDATA%\Programs\UA Reforged
   with "UA Reforged" in the Start menu (and on the desktop if you keep that
   box ticked). Uninstall it from Settings > Apps, or with "Uninstall UA
   Reforged" in the Start menu.
   No installer? The zip has the same files: unzip it anywhere and start
   "UA Reforged.exe".
3. On first start choose "I have the game - choose folder" and pick the
   folder that contains DATA and LEVELS, or "Get the game" (see below).
   UA Reforged only reads that folder; your saves and settings are copied to
       %LOCALAPPDATA%\UA Reforged\game\
   and the game writes only there.


YOUR GAME FILES
---------------
Use either:
- your own retail Urban Assault CD install, or
- the community "OpenUA Standalone Package" (full game plus the Metropolis
  Dawn expansion), hosted by the Metropolis Dawn community, not by us.
Point the app at that folder. Or choose "Get the game" on first start: the
app downloads the OpenUA Standalone Package (about 445 MB) straight from the
Metropolis Dawn community's server (hosted by them, not by us; the files are
Microsoft's) into
    %LOCALAPPDATA%\UA Reforged\game-original\
If that link is not working, the app says so and opens the UA Reforged help
page with other ways to get the game.
To pick a different folder later, delete
    %LOCALAPPDATA%\UA Reforged\game-folder.txt


THE REFORGE PACK (OPTIONAL)
---------------------------
The new look: remastered units, buildings and skies, HD textures, restored
videos, sharp menus and the floor pack (a 1.6 GB download, 2.4 GB on disk).
On first start (after you pick your game folder) the app offers to download
it: click "Download". The app checks every file, sets the pack up and then
starts the game with the new look. You can cancel; the next start offers to
resume. "Not now" or no internet: the game starts with the 1998 look.
"Don't ask again" stops the offer; to get it back, delete
    %LOCALAPPDATA%\UA Reforged\reforge-pack-offer-off
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
headset in with a USB-C cable, and start UA Reforged. It sees the headset and
offers "Install on my Quest" (or use the Start menu shortcut "Install UA
Reforged VR on my Quest"). Windows may also need Meta's "Oculus ADB Drivers".
It downloads the VR app and, the first time, Google's Android platform tools
(adb) from Google, checks both, installs or updates the app, and can copy your
game files to the headset so it does not download the game again. Nothing
else on the headset changes. In the headset: App Library > Unknown Sources >
UA Reforged VR. Help: https://uareforged.pages.dev/downloads/#quest-computer-h


GRAPHICS
--------
Options > Graphics: Original (the 1998 look), Low, Medium, High, Ultra.
Needs a graphics card with OpenGL 4.1 or newer and a current driver.


IF SOMETHING GOES WRONG
-----------------------
The launcher writes %LOCALAPPDATA%\UA Reforged\launcher.log, the engine
%LOCALAPPDATA%\UA Reforged\engine.log and
%LOCALAPPDATA%\UA Reforged\game\ENV\ypa_log.txt.


LICENCE
-------
Engine: GNU GPL version 2 (LICENSE-engine-GPL-2.0.txt). The matching source
code: https://github.com/ameliaaura/uareforged (see SOURCE.txt). Third-party
licences (SDL2, OpenAL Soft, Lua, Ogg/Vorbis, FFmpeg under the LGPL and the
libraries they use) are in the licenses folder; the font licences (SIL OFL
1.1, IPA 1.0) are next to the fonts in engine\fonts\. The Reforge pack has
its own LICENSE.txt.
Urban Assault and all of its content belong to Microsoft.

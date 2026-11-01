UA REFORGED - for Macs with Apple Silicon (M1 or later), macOS 11 or newer
==========================================================================

The 1998 game Urban Assault on the open-source OpenUA engine, with modern
graphics. Unofficial, free, non-commercial fan project.

YOU NEED YOUR OWN COPY OF URBAN ASSAULT. This download contains no game
files at all. See "Your game files" below.


INSTALLING
----------
1. Unzip the download and drag "UA Reforged.app" into Applications.
2. The app is not notarised by Apple, so the first start needs one extra step:
   right-click (or Control-click) "UA Reforged" > Open > Open.
   If macOS 15 or newer still refuses: System Settings > Privacy & Security,
   scroll down, "UA Reforged was blocked" > Open Anyway.
3. On first start choose "I have the game - choose folder" and pick the folder
   that contains DATA and LEVELS, or "Get the game" (see below). UA Reforged
   only reads that folder; your saves and settings are copied to
       ~/Library/Application Support/UA Reforged/game/
   and the game writes only there.


YOUR GAME FILES
---------------
Use either:
- your own retail Urban Assault CD install (copy the installed folder to your
  Mac), or
- the community "OpenUA Standalone Package" (full game plus the Metropolis
  Dawn expansion), hosted by the Metropolis Dawn community, not by us.
Point the app at that folder. Or choose "Get the game" on first start: the
app downloads the OpenUA Standalone Package (about 445 MB) straight from the
Metropolis Dawn community's server (hosted by them, not by us; the files are
Microsoft's) into
    ~/Library/Application Support/UA Reforged/game-original/
If that link is not working, the app says so and opens the UA Reforged help
page with other ways to get the game.
To pick a different folder later, delete
    ~/Library/Application Support/UA Reforged/game-folder.txt


THE REFORGE PACK (OPTIONAL)
---------------------------
The new look: remastered units, buildings and skies, HD textures, restored
videos, sharp menus and the floor pack (a 1.6 GB download, 2.4 GB on disk).
On first start (after you pick your game folder) the app offers to download
it: click "Download". The app checks every file, sets the pack up and then
starts the game with the new look. You can cancel; the next start offers to
resume. "Not now" or no internet: the game starts with the 1998 look.
"Don't ask again" stops the offer; to get it back, delete
    ~/Library/Application Support/UA Reforged/reforge-pack-offer-off
Manual install: unzip every pack part so that this folder exists:
    ~/Library/Application Support/UA Reforged/reforge-pack/hd-models/
Things the pack does not cover keep their 1998 look (the campaign map always
does).


INSTALL ON MY QUEST (UA REFORGED VR)
-----------------------------------
UA Reforged VR is the Meta Quest app. To install it from this Mac: turn on
Developer Mode for the headset (once, in the Meta Horizon phone app), plug the
headset in with a USB-C cable, and open UA Reforged. It sees the headset and
offers "Install on my Quest" (or hold the Option key while opening the app).
It downloads the VR app and, the first time, Google's Android platform tools
(adb) from Google, checks both, installs or updates the app, and can copy your
game files to the headset so it does not download the game again. Nothing
else on the headset changes. In the headset: App Library > Unknown Sources >
UA Reforged VR. Help: https://uareforged.pages.dev/downloads/#quest-computer-h


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

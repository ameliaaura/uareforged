UA REFORGED - for Linux PCs (x86_64), NEW in 0.8.5
====================================================

The 1998 game Urban Assault on the open-source OpenUA engine, with modern
graphics. Unofficial, free, non-commercial fan project.

YOU NEED THE ORIGINAL GAME FILES. This download contains no game files at
all: on first start the app fetches the free community game package for you,
or uses your own copy. See "Your game files" below.

The Linux edition is new. It is tested on Ubuntu with Mesa's software
renderer and under Windows' WSL (see the release notes for what was tested);
please report anything that goes wrong (the "Report a bug" button in the
game's Options, or the UA Reforged site).


WHAT YOU NEED
-------------
- A 64-bit PC (x86_64) with a current desktop Linux: Ubuntu 22.04 or newer,
  Linux Mint 21 or newer, Debian 12 or newer, Fedora 36 or newer, SteamOS 3.5
  or newer, or another distribution with glibc 2.35 or newer.
- A graphics driver with OpenGL 4.1 or newer (Mesa for AMD / Intel, or the
  NVIDIA driver). X11 and Wayland desktops both work.
- About 3.5 GB free disk space with the Reforge pack (4.5 GB while it
  installs), 1 GB without.
- Mouse and keyboard, or a game controller (Xbox, PlayStation and most others
  that SDL knows). The setup dialogs also work with a controller: d-pad to
  move, A to choose, B to close.


INSTALLING
----------
AppImage (one file):
1. Make it executable: right-click UA-Reforged-0.8.5-Linux-x86_64.AppImage >
   Properties > Permissions > "Allow executing file as program", or in a
   terminal:  chmod +x UA-Reforged-0.8.5-Linux-x86_64.AppImage
2. Double-click it (or run ./UA-Reforged-0.8.5-Linux-x86_64.AppImage).
   It needs FUSE, which every current desktop has. If it says FUSE is
   missing, use the tar.gz instead, or run it with --appimage-extract-and-run.

tar.gz (a folder, no FUSE needed):
1. Unpack it anywhere:  tar xzf UA-Reforged-0.8.5-Linux-x86_64.tar.gz
2. Run ./ua-reforged inside the folder.

Then set up UA Reforged. The first start shows one dialog. Choose "Set up
everything": the app downloads the game (the free OpenUA Standalone Package,
445 MB, from the Metropolis Dawn community) and the Reforge pack (1.6 GB)
and shows one progress window for both. Then pick a campaign (Play).
Already have the game? Choose "I already have the game" and pick its folder
(the one that contains DATA and LEVELS). Untick "Include the Reforge pack" to
keep the 1998 look for now. The folder picker needs zenity or kdialog (most
desktops have one); without them, start once from a terminal with
    ./UA-Reforged-0.8.5-Linux-x86_64.AppImage --game-dir /path/to/your/game
UA Reforged only reads your game folder; your saves and settings are kept in
UA Reforged's own folder (~/.local/share/UA Reforged).

Application menu: run the AppImage (or ./ua-reforged) once with --add-to-menu.
Steam / SteamOS / Big Picture on a TV: in Steam, "Add a Non-Steam Game" and
pick the AppImage (or the ua-reforged file of the tar.gz folder).

The first time a mission starts with the Reforge pack, the game runs a short
graphics test (about 20 seconds, a camera tour) and picks the graphics preset
that suits your PC. Options > Graphics changes it at any time.

New versions: when one is out, a short notice says so before the game starts.
"Update now" (AppImage only) downloads it, checks its signature and puts it in
place of your AppImage (same file name and place, so menu entries and Steam
shortcuts keep working), then starts it. It needs openssl (installed on most
desktops) and an AppImage in a folder of yours. The tar.gz folder, or anything
unexpected, opens the download page instead.


YOUR GAME FILES
---------------
"Set up everything" downloads the community OpenUA Standalone Package (the
full game plus the Metropolis Dawn expansion, 445 MB) straight from the
Metropolis Dawn community's server onto your PC. It is hosted by them, not by
us, and the game files belong to Microsoft. If that link is not working, the
app says so and opens the UA Reforged help page with other ways to get the
game. You can also use your own retail Urban Assault CD install (copy the
installed folder to your PC) with "I already have the game".
To pick a different folder later, delete
    ~/.local/share/UA Reforged/game-folder.txt


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
    ~/.local/share/UA Reforged/reforge-pack/hd-models/


ANONYMOUS USAGE STATS
---------------------
After the setup the app asks once whether to share anonymous stats and crash
reports. It is off unless you say yes; Options > Send anonymous usage stats
changes it. Details: https://uareforged.pages.dev/privacy


TROUBLESHOOTING
---------------
- Logs: ~/.local/share/UA Reforged/launcher.log and
  ~/.local/share/UA Reforged/game/ENV/ypa_log.txt
- The game uses X11 (also under Wayland, through XWayland). To use Wayland
  directly, start it with SDL_VIDEODRIVER=wayland in the environment.
- No sound: the game uses OpenAL Soft with PipeWire, PulseAudio or ALSA,
  whichever your system has.
- Black screen or "OpenGL" errors: update the graphics driver; the game needs
  OpenGL 4.1.


LICENCES
--------
The engine is free software under the GNU GPL version 2 (LICENSE-engine-
GPL-2.0.txt); its source is at https://github.com/ameliaaura/uareforged
(SOURCE.txt). The libraries in usr/lib (SDL2, SDL2_image, SDL2_ttf,
SDL2_net, OpenAL Soft, FFmpeg, libogg, libvorbis) and the bundled curl keep
their own licences: usr/share/ua-reforged/licenses/.
Urban Assault is (c) Microsoft; its game files are not part of this download.

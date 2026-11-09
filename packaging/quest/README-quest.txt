UA Reforged VR 0.9 for Meta Quest - install guide
==================================================

UA Reforged VR is Urban Assault (1998) in virtual reality, built and tested on the Meta Quest 3. The Quest 3S
has the same chip and should work, but is untested; Quest 2 and Quest Pro are untested. It is a free, unofficial
fan project and is not in the Meta Horizon Store: you install it yourself ("sideloading"). The app does not
contain the game. On first launch it can download the game for you (see step 4).

You need: your Quest, a USB-C cable, a Mac or Windows PC, and a free Meta developer account.


1. Turn on Developer Mode (once)
--------------------------------
Meta's own steps: https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/

In short:
  a. Create or join a developer organisation at https://developers.meta.com/horizon/ (free; Meta may ask
     you to verify your account).
  b. In the Meta Horizon app on your phone: Devices > your headset > Headset settings > Developer mode > On.
  c. Restart the headset.
  d. Connect the headset to your computer with the USB-C cable, put it on and choose "Allow" when it asks
     to allow USB debugging (tick "Always allow from this computer").


2a. Install with SideQuest (easiest)
------------------------------------
  a. Install SideQuest (Advanced Installer) from https://sidequestvr.com/setup-howto on your Mac or PC.
  b. Connect the headset. SideQuest shows a green dot when it sees it.
  c. Drag UA-Reforged-VR-0.9.1-Quest3.apk onto the SideQuest window (or use "Install APK file from
     folder" at the top). Wait for "Success".


2b. Or install with adb (command line)
--------------------------------------
adb comes with Google's "SDK Platform-Tools":
https://developer.android.com/tools/releases/platform-tools

  Mac (Terminal):        ./adb install -r UA-Reforged-VR-0.9.1-Quest3.apk
  Windows (PowerShell):  .\adb.exe install -r UA-Reforged-VR-0.9.1-Quest3.apk

"Success" means it is installed. "unauthorized" means step 1d is still waiting in the headset.


3. Find the app
---------------
In the headset: App Library > the filter at the top right > "Unknown Sources". Start "UA Reforged VR".


4. First launch
---------------
A panel appears in front of you. Point a controller at a button and pull the trigger (or move with the
thumbstick and press A).

  * "Get the game" downloads the community OpenUA Standalone Package (about 445 MB) straight from the
    Metropolis Dawn community's server onto the headset. UA Reforged does not host or ship the game files;
    they belong to Microsoft.
    Or choose "I'll copy the files myself" and copy your own Urban Assault folder (DATA, LEVELS and the
    rest) into Android/data/io.github.ameliaaura.uareforged.vr/files/ua on the headset with SideQuest or
    adb, then "Check again".
  * Then the app offers the Reforge pack for Quest (about 712 MB download, the new look: remastered units,
    buildings, skies, with textures made for the Quest). Without it the game keeps its 1998 look. You can say
    "Not now" and get it on a later start.
  * Use Wi-Fi and keep the headset on (on the charger is best) while it downloads. If a download breaks,
    the app says what went wrong and offers the help page: https://uareforged.pages.dev/downloads/


5. Playing
----------
  * Pause > Settings > LOOK switches between NEW LOOK and ORIGINAL (it applies at the next level).
  * Pause > Settings > DISPLAY sets the refresh rate (90 Hz by default).
  * Recentre the view: hold the Meta button on the right controller.
  * Anonymous usage stats: optional, off unless you say yes on the first start. Change it any time in
    Pause > Settings > CONTROLS, PRIVACY. When on: crash reports, performance summaries, error codes and a
    usage count; no name, no IP, no files, no personal data. Details: docs/usage-stats.md in the source.


Uninstall: App Library > Unknown Sources > UA Reforged VR > ... > Uninstall (this also removes the
downloaded game and pack from the headset).

Source code (GPL v2) and help: https://github.com/ameliaaura/uareforged

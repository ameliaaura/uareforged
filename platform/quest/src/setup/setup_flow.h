// The headset's start screens, before the engine runs: on first launch "Get the game" (the community OpenUA
// Standalone Package, fetched from its own server onto the headset) and the Quest pack (the Reforge pack's files the
// headset uses plus ASTC textures, one download with its own SHA-256), with the same failsafe as the desktop app; on
// every launch the campaign choice when the add-on campaigns are installed.
//
// Shown as one flat panel (an OpenXR quad layer) in front of the player; point a controller at a button and pull
// the trigger, or move the focus with a thumbstick and press A / X. B / Y = back or cancel.
#pragma once

#include <string>
#include <vector>

struct android_app;

namespace uavr {
class XrPlatform;
namespace setup {

// Returns true with the engine's extra arguments (campaign) when the game should start; false when the player
// quit or the app is closing. Writes <dataDir>/uavr.cfg (the app's settings for this data set).
bool RunSetup(android_app* app, XrPlatform& xr, const std::string& dataDir, std::vector<std::string>* engineArgs);

// True when <dir> holds the original game's DATA and LEVELS with the key files (any letter case).
bool GameValid(const std::string& dir);

}  // namespace setup
}  // namespace uavr

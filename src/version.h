#ifndef UA_VERSION_H
#define UA_VERSION_H

#include <string>

namespace Common {

// Identity of this port. Shown bottom-right on the title screen (where the
// original shipped a version label), in the SDL window title and in the engine
// log, so a screenshot always says which build produced it.
//
// Note for whoever touches the title screen next: the *button* in the very
// bottom-right corner is NOT a version label — it is the language requester
// (BTN_LANGUAGE, caption = locale string id 86). Every shipped .lng retires
// that id with the "<OBSOLETE>" marker, so its caption comes from the built-in
// default "LOCALE" (see Locale::Text::Get).
constexpr const char *PortName    = "Urban Assault: Reforged";
#ifdef UA_VR_NEWLOOK
constexpr const char *PortVersion = "0.9.0"; // the Quest app (UA Reforged VR)
#else
constexpr const char *PortVersion = "0.8.0";
#endif

// Short label for the title screen (the corner next to the language button)
// and anywhere else a compact identity fits better than the full one.
#ifdef UA_VR_NEWLOOK
constexpr const char *ShellLabel  = "UA Reforged VR 0.9 (OpenUA)";
#else
constexpr const char *ShellLabel  = "UA Reforged 0.8 (OpenUA)";
#endif

inline std::string PortIdentity()
{
    return std::string(PortName) + " " + PortVersion;
}

}

#endif

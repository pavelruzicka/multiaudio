// Installing without an installer: the program copies itself into the user's
// own program folder, adds a Start Menu shortcut, and can register itself to
// start with Windows. Everything is per-user, so none of it needs admin
// rights, and all of it is reversible.
#pragma once

#include <string>

#include "util.h"

namespace ma {

std::wstring ExecutablePath();
std::wstring InstallDirectory();  // %LOCALAPPDATA%\Programs\multiaudio
std::wstring InstalledExePath();

// True when the program running right now is the installed copy.
bool RunningInstalled();

// Copies this executable into place and adds the Start Menu shortcut.
// `message` receives something worth showing the user either way.
bool Install(bool startWithWindows, std::wstring* message);

// Removes the shortcut, the startup entry and the settings. The installed
// executable is deleted, or scheduled for deletion if it is the one running.
bool Uninstall(std::wstring* message);

bool StartsWithWindows();
bool SetStartWithWindows(bool enabled);

}  // namespace ma

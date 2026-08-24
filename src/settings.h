// What the tray app remembers between runs, kept in
// HKEY_CURRENT_USER\Software\multiaudio.
#pragma once

#include <string>
#include <vector>

#include "mirror.h"
#include "util.h"

namespace ma {

struct Settings {
    bool enabled = true;
    int latencyMs = 40;
    std::wstring sourceId;                  // empty means the Windows default device
    std::vector<std::wstring> excludedIds;  // destinations switched off in the menu

    // Turns the stored settings into what the engine expects.
    MirrorOptions toOptions() const;
};

// False on the very first run, which is when it is worth offering to install.
bool SettingsExist();

Settings LoadSettings();
void SaveSettings(const Settings& settings);

}  // namespace ma

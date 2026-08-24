#include "settings.h"

namespace ma {
namespace {

constexpr wchar_t kKey[] = L"Software\\multiaudio";
constexpr wchar_t kExcludedSeparator = L';';  // endpoint ids never contain one

std::wstring ReadString(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<BYTE*>(value.data()),
                         &bytes) != ERROR_SUCCESS) {
        return {};
    }
    value.resize(wcslen(value.c_str()));
    return value;
}

bool ReadDword(HKEY key, const wchar_t* name, DWORD* out) {
    DWORD type = 0;
    DWORD size = sizeof(DWORD);
    return RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(out), &size) ==
               ERROR_SUCCESS &&
           type == REG_DWORD;
}

void WriteString(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void WriteDword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

}  // namespace

MirrorOptions Settings::toOptions() const {
    MirrorOptions options;
    options.source = sourceId.empty() ? L"default" : sourceId;
    options.latencyMs = latencyMs;
    options.excludeIds = excludedIds;
    options.followDefault = sourceId.empty();
    return options;
}

bool SettingsExist() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}

Settings LoadSettings() {
    Settings settings;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return settings;  // first run: the defaults are what we want
    }

    DWORD number = 0;
    if (ReadDword(key, L"Enabled", &number)) settings.enabled = number != 0;
    if (ReadDword(key, L"LatencyMs", &number) && number >= 5 && number <= 500) {
        settings.latencyMs = static_cast<int>(number);
    }
    settings.sourceId = ReadString(key, L"SourceId");

    const std::wstring excluded = ReadString(key, L"ExcludedIds");
    size_t start = 0;
    while (start < excluded.size()) {
        const size_t end = excluded.find(kExcludedSeparator, start);
        const std::wstring piece = excluded.substr(
            start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!piece.empty()) settings.excludedIds.push_back(piece);
        if (end == std::wstring::npos) break;
        start = end + 1;
    }

    RegCloseKey(key);
    return settings;
}

void SaveSettings(const Settings& settings) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }

    WriteDword(key, L"Enabled", settings.enabled ? 1 : 0);
    WriteDword(key, L"LatencyMs", static_cast<DWORD>(settings.latencyMs));
    WriteString(key, L"SourceId", settings.sourceId);

    std::wstring excluded;
    for (const auto& id : settings.excludedIds) {
        if (!excluded.empty()) excluded += kExcludedSeparator;
        excluded += id;
    }
    WriteString(key, L"ExcludedIds", excluded);

    RegCloseKey(key);
}

}  // namespace ma

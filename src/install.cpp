#include "install.h"

#include <shlobj.h>

#include <string>

namespace ma {
namespace {

constexpr wchar_t kAppName[] = L"multiaudio";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

std::wstring KnownFolder(int folder) {
    wchar_t path[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(nullptr, folder, nullptr, SHGFP_TYPE_CURRENT, path))) return {};
    return path;
}

std::wstring ShortcutPath() {
    const std::wstring programs = KnownFolder(CSIDL_PROGRAMS);
    if (programs.empty()) return {};
    return programs + L"\\" + kAppName + L".lnk";
}

// Creates every directory along the path, like `mkdir -p`.
bool MakeDirectories(const std::wstring& path) {
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != L'\\') continue;
        if (i < 3) continue;  // skip the drive letter
        const std::wstring partial = path.substr(0, i);
        if (!CreateDirectoryW(partial.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    return true;
}

bool WriteShortcut(const std::wstring& target, const std::wstring& shortcut) {
    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(__uuidof(ShellLink), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IShellLinkW), link.put_void());
    if (FAILED(hr)) return false;

    link->SetPath(target.c_str());
    link->SetDescription(L"Play the same sound through every output device");

    const size_t slash = target.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
        link->SetWorkingDirectory(target.substr(0, slash).c_str());
    }

    ComPtr<IPersistFile> file;
    hr = link->QueryInterface(__uuidof(IPersistFile), file.put_void());
    if (FAILED(hr)) return false;
    return SUCCEEDED(file->Save(shortcut.c_str(), TRUE));
}

bool PathsEqual(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && a.size() == b.size() &&
           CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

}  // namespace

std::wstring ExecutablePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr, path.data(),
                                                 static_cast<DWORD>(path.size()));
        if (written == 0) return {};
        if (written < path.size()) {
            path.resize(written);
            return path;
        }
        path.resize(path.size() * 2);  // truncated; try again with more room
    }
}

std::wstring InstallDirectory() {
    const std::wstring local = KnownFolder(CSIDL_LOCAL_APPDATA);
    if (local.empty()) return {};
    return local + L"\\Programs\\" + kAppName;
}

std::wstring InstalledExePath() {
    const std::wstring directory = InstallDirectory();
    if (directory.empty()) return {};
    return directory + L"\\" + kAppName + L".exe";
}

bool RunningInstalled() { return PathsEqual(ExecutablePath(), InstalledExePath()); }

bool Install(bool startWithWindows, std::wstring* message) {
    const std::wstring source = ExecutablePath();
    const std::wstring target = InstalledExePath();
    if (source.empty() || target.empty()) {
        if (message) *message = L"Could not work out where to install to.";
        return false;
    }

    if (!PathsEqual(source, target)) {
        if (!MakeDirectories(InstallDirectory())) {
            if (message) *message = L"Could not create " + InstallDirectory();
            return false;
        }
        if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) {
            const DWORD error = GetLastError();
            if (message) {
                *message = L"Could not copy the program into place";
                if (error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED) {
                    *message += L": a copy is already running from there. Exit it first.";
                } else {
                    *message += L".";
                }
            }
            return false;
        }
    }

    const std::wstring shortcut = ShortcutPath();
    const bool shortcutMade = !shortcut.empty() && WriteShortcut(target, shortcut);

    if (startWithWindows) SetStartWithWindows(true);

    if (message) {
        *message = L"Installed to " + target;
        if (shortcutMade) *message += L"\nAdded to the Start Menu.";
        if (startWithWindows) *message += L"\nIt will start with Windows.";
    }
    return true;
}

bool Uninstall(std::wstring* message) {
    SetStartWithWindows(false);

    const std::wstring shortcut = ShortcutPath();
    if (!shortcut.empty()) DeleteFileW(shortcut.c_str());

    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\multiaudio");

    const std::wstring target = InstalledExePath();
    std::wstring note;
    if (!target.empty()) {
        if (PathsEqual(ExecutablePath(), target)) {
            // Windows will not delete a running program, so queue it for the
            // next restart.
            MoveFileExW(target.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            note = L"\nThe program file is removed at the next restart.";
        } else if (DeleteFileW(target.c_str())) {
            RemoveDirectoryW(InstallDirectory().c_str());
        }
    }

    if (message) {
        *message = L"Removed the Start Menu shortcut, the startup entry and the settings." + note;
    }
    return true;
}

bool StartsWithWindows() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    const LSTATUS status = RegQueryValueExW(key, kAppName, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool SetStartWithWindows(bool enabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &key) != ERROR_SUCCESS) {
        return false;
    }

    bool ok = false;
    if (enabled) {
        // Prefer the installed copy: a shortcut to wherever it was downloaded
        // would break the moment that file is moved.
        std::wstring target = InstalledExePath();
        DWORD attributes = target.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(target.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) target = ExecutablePath();

        const std::wstring value = L"\"" + target + L"\"";
        ok = RegSetValueExW(key, kAppName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) ==
             ERROR_SUCCESS;
    } else {
        const LSTATUS status = RegDeleteValueW(key, kAppName);
        ok = status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }

    RegCloseKey(key);
    return ok;
}

}  // namespace ma

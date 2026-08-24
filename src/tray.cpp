#include "tray.h"

#include "util.h"  // brings in windows.h, which shellapi.h needs first

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

#include "devices.h"
#include "install.h"
#include "mirror.h"
#include "settings.h"

namespace ma {
namespace {

constexpr wchar_t kWindowClass[] = L"multiaudioTrayWindow";
constexpr wchar_t kAppTitle[] = L"multiaudio";
constexpr wchar_t kInstanceMutex[] = L"Local\\multiaudio.instance";

constexpr UINT WM_TRAY_ICON = WM_APP + 1;
constexpr UINT WM_ENGINE_STATUS = WM_APP + 2;
constexpr UINT WM_SHOW_EXISTING = WM_APP + 3;

enum MenuId : UINT {
    kIdNone = 0,
    kIdToggle = 100,
    kIdStartup,
    kIdInstall,
    kIdUninstall,
    kIdExit,
    kIdSourceBase = 1000,   // +0 is "Windows default", +1.. are devices
    kIdSinkBase = 2000,
    kIdLatencyBase = 3000,
};

const int kLatencyChoices[] = {15, 25, 40, 80, 150};

template <size_t N>
void CopyTo(wchar_t (&destination)[N], const std::wstring& text) {
    const size_t count = std::min(text.size(), N - 1);
    std::copy_n(text.begin(), count, destination);
    destination[count] = L'\0';
}

// ---------------------------------------------------------------------------
// The tray icon, drawn at runtime so the program stays a single file with no
// image resources. A pair of headphones: blue when mirroring, grey when off.
// ---------------------------------------------------------------------------
float Coverage(float distance) {  // distance is negative inside the shape
    return std::max(0.0f, std::min(1.0f, 0.5f - distance));
}

HICON MakeTrayIcon(int size, bool on) {
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP colour = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!colour || !pixels) {
        if (colour) DeleteObject(colour);
        return nullptr;
    }

    const float extent = static_cast<float>(size);
    const float centreX = extent * 0.5f;
    const float bandY = extent * 0.56f;        // centre of the headband arc
    const float bandRadius = extent * 0.34f;   // to the middle of the band
    const float bandHalf = extent * 0.055f;    // half its thickness
    const float cupY = extent * 0.60f;
    const float cupRadius = extent * 0.155f;
    const float cupX = bandRadius;

    const float red = on ? 59.0f : 138.0f;     // #3B82F6 mirroring, #8A8F98 off
    const float green = on ? 130.0f : 143.0f;
    const float blue = on ? 246.0f : 152.0f;

    auto* out = static_cast<BYTE*>(pixels);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;

            // Headband: the upper half of a ring.
            float alpha = 0.0f;
            if (py <= bandY) {
                const float dx = px - centreX;
                const float dy = py - bandY;
                const float ring = std::fabs(std::sqrt(dx * dx + dy * dy) - bandRadius);
                alpha = Coverage(ring - bandHalf);
            }

            // Ear cups at each end of the band.
            for (int side = -1; side <= 1; side += 2) {
                const float dx = px - (centreX + static_cast<float>(side) * cupX);
                const float dy = py - cupY;
                alpha = std::max(alpha, Coverage(std::sqrt(dx * dx + dy * dy) - cupRadius));
            }

            BYTE* pixel = out + (static_cast<size_t>(y) * size + x) * 4;
            pixel[0] = static_cast<BYTE>(blue * alpha);  // premultiplied BGRA
            pixel[1] = static_cast<BYTE>(green * alpha);
            pixel[2] = static_cast<BYTE>(red * alpha);
            pixel[3] = static_cast<BYTE>(255.0f * alpha);
        }
    }

    std::vector<BYTE> maskBits(static_cast<size_t>(size) * ((size + 15) / 16) * 2, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());

    ICONINFO iconInfo = {};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmMask = mask;
    iconInfo.hbmColor = colour;
    HICON icon = CreateIconIndirect(&iconInfo);

    DeleteObject(colour);
    if (mask) DeleteObject(mask);
    return icon;
}

// ---------------------------------------------------------------------------
// The application.
// ---------------------------------------------------------------------------
class TrayApp {
public:
    bool create(HINSTANCE instance);
    int run();

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void showMenu();
    void handleCommand(UINT command);
    void refreshIcon();
    void applySettings();
    void notify(const std::wstring& title, const std::wstring& text);

    std::wstring statusLine() const;

    HWND window_ = nullptr;
    NOTIFYICONDATAW icon_ = {};
    HICON iconOn_ = nullptr;
    HICON iconOff_ = nullptr;
    UINT taskbarCreatedMessage_ = 0;
    bool iconAdded_ = false;

    Settings settings_;
    MirrorEngine engine_;

    // Rebuilt every time the menu opens, so the menu ids map to real devices.
    std::vector<DeviceInfo> menuDevices_;
};

TrayApp* g_app = nullptr;

bool TrayApp::create(HINSTANCE instance) {
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &TrayApp::WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) return false;

    // A normal window that is never shown: a message-only window would not
    // receive the TaskbarCreated broadcast when Explorer restarts.
    window_ = CreateWindowExW(0, kWindowClass, kAppTitle, WS_OVERLAPPED, CW_USEDEFAULT,
                              CW_USEDEFAULT, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!window_) return false;

    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");

    const int size = GetSystemMetrics(SM_CXSMICON);
    iconOn_ = MakeTrayIcon(size, true);
    iconOff_ = MakeTrayIcon(size, false);

    icon_.cbSize = sizeof(icon_);
    icon_.hWnd = window_;
    icon_.uID = 1;
    icon_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    icon_.uCallbackMessage = WM_TRAY_ICON;
    icon_.hIcon = iconOff_;
    CopyTo(icon_.szTip, std::wstring(kAppTitle));
    iconAdded_ = Shell_NotifyIconW(NIM_ADD, &icon_) != FALSE;

    settings_ = LoadSettings();
    engine_.setStatusCallback([this] { PostMessageW(window_, WM_ENGINE_STATUS, 0, 0); });
    applySettings();
    engine_.startBackground();
    refreshIcon();
    return true;
}

int TrayApp::run() {
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    engine_.requestStop();
    if (iconAdded_) Shell_NotifyIconW(NIM_DELETE, &icon_);
    if (iconOn_) DestroyIcon(iconOn_);
    if (iconOff_) DestroyIcon(iconOff_);
    return 0;
}

LRESULT CALLBACK TrayApp::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (g_app) return g_app->handleMessage(window, message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT TrayApp::handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == taskbarCreatedMessage_ && taskbarCreatedMessage_ != 0) {
        // Explorer restarted and took the notification area with it.
        iconAdded_ = Shell_NotifyIconW(NIM_ADD, &icon_) != FALSE;
        refreshIcon();
        return 0;
    }

    switch (message) {
        case WM_TRAY_ICON:
            switch (LOWORD(lParam)) {
                case WM_LBUTTONUP:
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    showMenu();
                    return 0;
                case WM_LBUTTONDBLCLK:
                    handleCommand(kIdToggle);
                    return 0;
                default:
                    return 0;
            }

        case WM_ENGINE_STATUS:
            refreshIcon();
            return 0;

        case WM_SHOW_EXISTING:
            notify(kAppTitle, L"multiaudio is already running - it is here in the "
                              L"notification area.");
            return 0;

        case WM_ENDSESSION:
            engine_.requestStop();
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

std::wstring TrayApp::statusLine() const {
    const EngineStatus status = engine_.status();
    switch (status.state) {
        case EngineState::Off:
            return L"Switched off";
        case EngineState::Waiting:
            return status.message.empty() ? L"Waiting" : L"Waiting: " + status.message;
        case EngineState::Mirroring:
            break;
    }
    const size_t count = status.sinks.size();
    return L"Mirroring " + status.source + L" to " + std::to_wstring(count) +
           (count == 1 ? L" device" : L" devices") + L", " +
           std::to_wstring(status.latencyMs) + L" ms behind";
}

void TrayApp::refreshIcon() {
    const EngineStatus status = engine_.status();
    icon_.hIcon = status.state == EngineState::Mirroring ? iconOn_ : iconOff_;
    CopyTo(icon_.szTip, std::wstring(kAppTitle) + L" - " + statusLine());
    icon_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    if (iconAdded_) Shell_NotifyIconW(NIM_MODIFY, &icon_);
}

void TrayApp::notify(const std::wstring& title, const std::wstring& text) {
    NOTIFYICONDATAW balloon = icon_;
    balloon.uFlags = NIF_INFO;
    CopyTo(balloon.szInfoTitle, title);
    CopyTo(balloon.szInfo, text);
    balloon.dwInfoFlags = NIIF_NONE;
    if (iconAdded_) Shell_NotifyIconW(NIM_MODIFY, &balloon);
}

void TrayApp::applySettings() {
    engine_.setOptions(settings_.toOptions());
    engine_.setEnabled(settings_.enabled);
}

void TrayApp::showMenu() {
    const EngineStatus status = engine_.status();

    ComPtr<IMMDeviceEnumerator> enumerator;
    menuDevices_.clear();
    if (CreateDeviceEnumerator(&enumerator)) {
        ListRenderDevices(enumerator.get(), &menuDevices_);
    }

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (settings_.enabled ? MF_CHECKED : 0), kIdToggle, L"Enabled");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | MF_DISABLED, kIdNone, statusLine().c_str());
    for (const auto& sink : status.sinks) {
        AppendMenuW(menu, MF_STRING | MF_DISABLED, kIdNone, (L"    " + sink).c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Which device to mirror from.
    HMENU sources = CreatePopupMenu();
    AppendMenuW(sources, MF_STRING, kIdSourceBase, L"Windows default device");
    UINT selectedSource = kIdSourceBase;
    for (size_t i = 0; i < menuDevices_.size(); ++i) {
        const UINT id = kIdSourceBase + 1 + static_cast<UINT>(i);
        AppendMenuW(sources, MF_STRING, id, menuDevices_[i].name.c_str());
        if (!settings_.sourceId.empty() && menuDevices_[i].id == settings_.sourceId) {
            selectedSource = id;
        }
    }
    CheckMenuRadioItem(sources, kIdSourceBase,
                       kIdSourceBase + static_cast<UINT>(menuDevices_.size()), selectedSource,
                       MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sources), L"Mirror from");

    // Which devices to mirror to.
    HMENU destinations = CreatePopupMenu();
    bool anyDestination = false;
    for (size_t i = 0; i < menuDevices_.size(); ++i) {
        const DeviceInfo& device = menuDevices_[i];
        const bool isSource = settings_.sourceId.empty()
                                  ? device.isDefault
                                  : device.id == settings_.sourceId;
        if (isSource) continue;
        const bool excluded = std::find(settings_.excludedIds.begin(),
                                        settings_.excludedIds.end(),
                                        device.id) != settings_.excludedIds.end();
        AppendMenuW(destinations, MF_STRING | (excluded ? 0 : MF_CHECKED),
                    kIdSinkBase + static_cast<UINT>(i), device.name.c_str());
        anyDestination = true;
    }
    if (!anyDestination) {
        AppendMenuW(destinations, MF_STRING | MF_DISABLED, kIdNone, L"(nothing else plugged in)");
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(destinations), L"Mirror to");

    // How far behind the source the other devices run.
    HMENU latency = CreatePopupMenu();
    UINT selectedLatency = kIdLatencyBase;
    for (size_t i = 0; i < std::size(kLatencyChoices); ++i) {
        const UINT id = kIdLatencyBase + static_cast<UINT>(i);
        AppendMenuW(latency, MF_STRING, id,
                    (std::to_wstring(kLatencyChoices[i]) + L" ms").c_str());
        if (kLatencyChoices[i] == settings_.latencyMs) selectedLatency = id;
    }
    CheckMenuRadioItem(latency, kIdLatencyBase,
                       kIdLatencyBase + static_cast<UINT>(std::size(kLatencyChoices)) - 1,
                       selectedLatency, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(latency), L"Latency");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (StartsWithWindows() ? MF_CHECKED : 0), kIdStartup,
                L"Start with Windows");
    if (RunningInstalled()) {
        AppendMenuW(menu, MF_STRING, kIdUninstall, L"Uninstall...");
    } else {
        AppendMenuW(menu, MF_STRING, kIdInstall, L"Install for this user...");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdExit, L"Exit");

    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(window_);  // so the menu closes when it loses focus
    const UINT chosen = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, window_,
        nullptr));
    PostMessageW(window_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (chosen != kIdNone) handleCommand(chosen);
}

void TrayApp::handleCommand(UINT command) {
    if (command >= kIdSourceBase && command < kIdSourceBase + 1 + menuDevices_.size()) {
        const size_t index = command - kIdSourceBase;
        settings_.sourceId = index == 0 ? std::wstring() : menuDevices_[index - 1].id;
        SaveSettings(settings_);
        applySettings();
        return;
    }
    if (command >= kIdSinkBase && command < kIdSinkBase + menuDevices_.size()) {
        const std::wstring& id = menuDevices_[command - kIdSinkBase].id;
        auto& excluded = settings_.excludedIds;
        const auto found = std::find(excluded.begin(), excluded.end(), id);
        if (found == excluded.end()) {
            excluded.push_back(id);
        } else {
            excluded.erase(found);
        }
        SaveSettings(settings_);
        applySettings();
        return;
    }
    if (command >= kIdLatencyBase && command < kIdLatencyBase + std::size(kLatencyChoices)) {
        settings_.latencyMs = kLatencyChoices[command - kIdLatencyBase];
        SaveSettings(settings_);
        applySettings();
        return;
    }

    switch (command) {
        case kIdToggle: {
            settings_.enabled = !settings_.enabled;
            SaveSettings(settings_);
            engine_.setEnabled(settings_.enabled);
            refreshIcon();
            break;
        }
        case kIdStartup:
            SetStartWithWindows(!StartsWithWindows());
            break;
        case kIdInstall: {
            std::wstring message;
            const bool ok = Install(true, &message);
            MessageBoxW(nullptr, message.c_str(), kAppTitle,
                        MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
            if (!ok) break;

            // Hand over to the copy that was just installed.
            const std::wstring installed = InstalledExePath();
            if (iconAdded_) Shell_NotifyIconW(NIM_DELETE, &icon_);
            iconAdded_ = false;
            ShellExecuteW(nullptr, L"open", installed.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            PostQuitMessage(0);
            break;
        }
        case kIdUninstall: {
            if (MessageBoxW(nullptr, L"Remove multiaudio from this computer?", kAppTitle,
                            MB_YESNO | MB_ICONQUESTION) != IDYES) {
                break;
            }
            std::wstring message;
            Uninstall(&message);
            MessageBoxW(nullptr, message.c_str(), kAppTitle, MB_OK | MB_ICONINFORMATION);
            PostQuitMessage(0);
            break;
        }
        case kIdExit:
            PostQuitMessage(0);
            break;
        default:
            break;
    }
}

}  // namespace

int RunTrayApp() {
    // One copy is enough: two would fight over the same devices.
    Handle instanceLock;
    instanceLock.attach(CreateMutexW(nullptr, TRUE, kInstanceMutex));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kWindowClass, nullptr);
        if (existing) PostMessageW(existing, WM_SHOW_EXISTING, 0, 0);
        return 0;
    }

    ComApartment com;  // the UI thread is where the shell objects are used

    // First run: offer to put it somewhere permanent, since a program that
    // starts with Windows should not live in the Downloads folder.
    if (!SettingsExist() && !RunningInstalled()) {
        SaveSettings(Settings{});  // whatever the answer, do not ask again
        const int answer = MessageBoxW(
            nullptr,
            L"Install multiaudio and start it with Windows?\n\n"
            L"It is copied into your own program folder and added to the Start "
            L"Menu. No admin rights are needed, and the tray menu can undo it.",
            kAppTitle, MB_YESNO | MB_ICONQUESTION);
        if (answer == IDYES) {
            std::wstring message;
            if (Install(true, &message)) {
                instanceLock.reset();  // let the installed copy take the lock
                ShellExecuteW(nullptr, L"open", InstalledExePath().c_str(), nullptr, nullptr,
                              SW_SHOWNORMAL);
                return 0;
            }
            MessageBoxW(nullptr, message.c_str(), kAppTitle, MB_OK | MB_ICONERROR);
        }
    }

    TrayApp app;
    g_app = &app;
    if (!app.create(GetModuleHandleW(nullptr))) {
        MessageBoxW(nullptr, L"multiaudio could not start its notification area icon.", kAppTitle,
                    MB_OK | MB_ICONERROR);
        g_app = nullptr;
        return 1;
    }

    const int result = app.run();
    g_app = nullptr;
    return result;
}

}  // namespace ma

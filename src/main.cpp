// multiaudio - play the same sound through every output device at once.
//
// Windows sends each program's audio to a single playback device. This tool
// captures whatever is playing on one device (the "source") and plays it back
// on all the others, so two pairs of headphones hear the same thing.
//
// Started with no arguments it runs as a notification-area (tray) app. The
// command line options are for setting it up and for looking at what Windows
// reports.

#include "util.h"

#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include "devices.h"
#include "install.h"
#include "mirror.h"
#include "tray.h"

namespace {

ma::MirrorEngine* g_engine = nullptr;

BOOL WINAPI ConsoleHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (g_engine) {
                ma::LogInfo("");
                ma::LogInfo("Stopping...");
                g_engine->requestStop();
            }
            return TRUE;
        default:
            return FALSE;
    }
}

// A window-subsystem program has no console of its own. For the command line
// options we borrow the one we were started from, or make a new one.
bool AttachToConsole() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole()) return false;
    FILE* stream = nullptr;
    stream = freopen("CONOUT$", "w", stdout);
    stream = freopen("CONOUT$", "w", stderr);
    stream = freopen("CONIN$", "r", stdin);
    (void)stream;
    SetConsoleOutputCP(CP_UTF8);
    return true;
}

// True when this program is the only thing attached to the console, which
// means the window is ours and closes the moment we return.
bool OwnsConsole() {
    DWORD processes[2] = {0, 0};
    return GetConsoleProcessList(processes, 2) <= 1;
}

void PauseIfWeOwnTheConsole() {
    if (!OwnsConsole()) return;
    printf("\nPress Enter to close this window...");
    fflush(stdout);
    (void)fgetc(stdin);
}

void PrintUsage() {
    printf(
        "multiaudio - play the same sound through every output device at once\n"
        "\n"
        "Started with no arguments, it runs in the notification area: click the\n"
        "icon for the on/off switch, which device to mirror from and to, and\n"
        "whether to start with Windows.\n"
        "\n"
        "Usage:\n"
        "  multiaudio [options]\n"
        "\n"
        "Setting up:\n"
        "  --install              Copy this program into your own program folder,\n"
        "                         add it to the Start Menu and start it with\n"
        "                         Windows. No admin rights needed.\n"
        "  --uninstall            Undo all of that, including the settings.\n"
        "\n"
        "Looking around:\n"
        "  --list                 Show the playback devices and exit.\n"
        "\n"
        "Running in a console instead of the tray:\n"
        "  --console              Mirror in this window until Ctrl+C.\n"
        "  --source <device>      Device to mirror from: \"default\", a number from\n"
        "                         --list, or part of a device name.\n"
        "  --to <name>            Only mirror to devices matching <name>.\n"
        "                         May be given more than once.\n"
        "  --exclude <name>       Never mirror to devices matching <name>.\n"
        "  --latency <ms>         How far the mirrored devices lag the source,\n"
        "                         5-500. Default: 40.\n"
        "  --no-follow-default    Do not follow changes of the default device.\n"
        "  --verbose              Print extra detail.\n"
        "\n"
        "Volume is per device: use the Windows volume mixer to balance them.\n");
}

bool ListDevices() {
    ma::ComPtr<IMMDeviceEnumerator> enumerator;
    std::vector<ma::DeviceInfo> devices;
    if (!ma::CreateDeviceEnumerator(&enumerator) ||
        !ma::ListRenderDevices(enumerator.get(), &devices)) {
        ma::LogError("could not ask Windows for the playback devices");
        return false;
    }

    if (devices.empty()) {
        ma::LogInfo("No active playback devices found.");
        return true;
    }

    ma::LogInfo("Playback devices:");
    for (size_t i = 0; i < devices.size(); ++i) {
        ma::LogInfo("  %zu. %s%s", i + 1, ma::Utf8(devices[i].name).c_str(),
                    devices[i].isDefault ? "   (default)" : "");
    }
    ma::LogInfo("");
    ma::LogInfo("Use the number or part of the name with --source, --to or --exclude.");
    return true;
}

// Reads the value that follows a flag, or reports what was missing.
bool TakeValue(int argc, wchar_t** argv, int* index, const wchar_t* flag, std::wstring* out) {
    if (*index + 1 >= argc) {
        ma::LogError("%s needs a value", ma::Utf8(flag).c_str());
        return false;
    }
    *out = argv[++(*index)];
    return true;
}

void ReportStatus(const ma::EngineStatus& status) {
    switch (status.state) {
        case ma::EngineState::Off:
            ma::LogInfo("Stopped.");
            return;
        case ma::EngineState::Waiting:
            ma::LogInfo("Waiting: %s", ma::Utf8(status.message).c_str());
            return;
        case ma::EngineState::Mirroring:
            break;
    }
    ma::LogInfo("Mirroring %s to %zu device%s:", ma::Utf8(status.source).c_str(),
                status.sinks.size(), status.sinks.size() == 1 ? "" : "s");
    for (const auto& sink : status.sinks) {
        ma::LogInfo("  -> %s", ma::Utf8(sink).c_str());
    }
}

int RunConsole(const ma::MirrorOptions& options) {
    ma::ComApartment com;
    if (FAILED(com.hr())) {
        ma::LogError("could not initialize COM: %s", ma::HrText(com.hr()).c_str());
        return 1;
    }

    ma::MirrorEngine engine;
    engine.setOptions(options);
    engine.setEnabled(true);
    engine.setStatusCallback([&engine] { ReportStatus(engine.status()); });

    ma::LogInfo("Mirroring. Press Ctrl+C to stop.");

    g_engine = &engine;
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    engine.runForeground();
    SetConsoleCtrlHandler(ConsoleHandler, FALSE);
    g_engine = nullptr;
    return 0;
}

int RunCommandLine(int argc, wchar_t** argv) {
    ma::MirrorOptions options;
    bool wantsList = false;
    bool wantsConsole = false;
    bool wantsInstall = false;
    bool wantsUninstall = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        std::wstring value;

        if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            PrintUsage();
            return 0;
        } else if (arg == L"--list" || arg == L"-l") {
            wantsList = true;
        } else if (arg == L"--console" || arg == L"-c") {
            wantsConsole = true;
        } else if (arg == L"--install") {
            wantsInstall = true;
        } else if (arg == L"--uninstall") {
            wantsUninstall = true;
        } else if (arg == L"--source" || arg == L"-s") {
            if (!TakeValue(argc, argv, &i, L"--source", &options.source)) return 2;
            options.followDefault = false;
            wantsConsole = true;
        } else if (arg == L"--to" || arg == L"-t") {
            if (!TakeValue(argc, argv, &i, L"--to", &value)) return 2;
            options.include.push_back(value);
            wantsConsole = true;
        } else if (arg == L"--exclude" || arg == L"-x") {
            if (!TakeValue(argc, argv, &i, L"--exclude", &value)) return 2;
            options.exclude.push_back(value);
            wantsConsole = true;
        } else if (arg == L"--latency") {
            if (!TakeValue(argc, argv, &i, L"--latency", &value)) return 2;
            options.latencyMs = static_cast<int>(wcstol(value.c_str(), nullptr, 10));
            if (options.latencyMs < 5 || options.latencyMs > 500) {
                ma::LogError("--latency must be between 5 and 500 milliseconds");
                return 2;
            }
            wantsConsole = true;
        } else if (arg == L"--no-follow-default") {
            options.followDefault = false;
        } else if (arg == L"--verbose" || arg == L"-v") {
            ma::g_verbose = true;
        } else {
            ma::LogError("unknown option \"%s\" (try --help)", ma::Utf8(arg).c_str());
            return 2;
        }
    }

    if (wantsInstall || wantsUninstall) {
        ma::ComApartment com;  // the Start Menu shortcut is a COM object
        std::wstring message;
        const bool ok = wantsUninstall ? ma::Uninstall(&message) : ma::Install(true, &message);
        ma::LogInfo("%s", ma::Utf8(message).c_str());
        return ok ? 0 : 1;
    }

    if (wantsList) {
        ma::ComApartment com;
        if (FAILED(com.hr())) {
            ma::LogError("could not initialize COM: %s", ma::HrText(com.hr()).c_str());
            return 1;
        }
        return ListDevices() ? 0 : 1;
    }

    if (wantsConsole) return RunConsole(options);

    PrintUsage();
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool hasArguments = argv != nullptr && argc > 1;

    int result = 0;
    if (hasArguments) {
        AttachToConsole();
        result = RunCommandLine(argc, argv);
        PauseIfWeOwnTheConsole();
    } else {
        result = ma::RunTrayApp();
    }

    if (argv) LocalFree(argv);
    return result;
}

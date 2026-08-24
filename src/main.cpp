// multiaudio - play the same sound through every output device at once.
//
// Windows sends each program's audio to a single playback device. This tool
// captures whatever is playing on one device (the "source") and plays it back
// on all the others, so two pairs of headphones hear the same thing.

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include "devices.h"
#include "mirror.h"
#include "util.h"

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

void PrintUsage() {
    printf(
        "multiaudio - mirror one playback device to all the others\n"
        "\n"
        "Usage:\n"
        "  multiaudio [options]\n"
        "\n"
        "Options:\n"
        "  --list                 Show the playback devices and exit.\n"
        "  --source <device>      Device to mirror from: \"default\", a number from\n"
        "                         --list, or part of a device name.\n"
        "                         Default: the current Windows default device.\n"
        "  --to <name>            Only mirror to devices whose name contains <name>.\n"
        "                         May be given more than once. Default: every other\n"
        "                         playback device.\n"
        "  --exclude <name>       Never mirror to devices whose name contains <name>.\n"
        "                         May be given more than once.\n"
        "  --latency <ms>         How far the mirrored devices lag the source,\n"
        "                         5-500. Lower is tighter but more likely to\n"
        "                         crackle. Default: 40.\n"
        "  --no-follow-default    Do not reconnect when the default device changes.\n"
        "  --verbose              Print extra detail.\n"
        "  --help                 Show this help.\n"
        "\n"
        "Examples:\n"
        "  multiaudio                                 Mirror the default device\n"
        "                                             everywhere else.\n"
        "  multiaudio --to \"USB\" --to \"Realtek\"       Mirror to just those two.\n"
        "  multiaudio --exclude \"HDMI\"                Mirror everywhere but HDMI.\n"
        "  multiaudio --source \"CABLE Input\"          Mirror a virtual cable to\n"
        "                                             every real device.\n"
        "\n"
        "Volume is per device: use the Windows volume mixer to balance them.\n"
        "Press Ctrl+C to stop.\n");
}

bool ListDevices() {
    ma::ComPtr<IMMDeviceEnumerator> enumerator;
    if (!ma::CreateDeviceEnumerator(&enumerator)) return false;

    std::vector<ma::DeviceInfo> devices;
    if (!ma::ListRenderDevices(enumerator.get(), &devices)) return false;

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

// True when this program is the only thing attached to the console, which
// means Windows created the window for us because it was started from
// Explorer. In that case the window - and every message in it - disappears the
// instant we return, so we wait for a key first.
bool OwnsConsole() {
    DWORD processes[2] = {0, 0};
    return GetConsoleProcessList(processes, 2) <= 1;
}

void PauseIfLaunchedFromExplorer() {
    if (!OwnsConsole()) return;
    printf("\nPress Enter to close this window...");
    fflush(stdout);
    (void)fgetc(stdin);
}

int Run(int argc, wchar_t** argv) {
    ma::MirrorOptions options;
    bool wantsList = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        std::wstring value;

        if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            PrintUsage();
            return 0;
        } else if (arg == L"--list" || arg == L"-l") {
            wantsList = true;
        } else if (arg == L"--source" || arg == L"-s") {
            if (!TakeValue(argc, argv, &i, L"--source", &options.source)) return 2;
        } else if (arg == L"--to" || arg == L"-t") {
            if (!TakeValue(argc, argv, &i, L"--to", &value)) return 2;
            options.include.push_back(value);
        } else if (arg == L"--exclude" || arg == L"-x") {
            if (!TakeValue(argc, argv, &i, L"--exclude", &value)) return 2;
            options.exclude.push_back(value);
        } else if (arg == L"--latency") {
            if (!TakeValue(argc, argv, &i, L"--latency", &value)) return 2;
            options.latencyMs = static_cast<int>(wcstol(value.c_str(), nullptr, 10));
            if (options.latencyMs < 5 || options.latencyMs > 500) {
                ma::LogError("--latency must be between 5 and 500 milliseconds");
                return 2;
            }
        } else if (arg == L"--no-follow-default") {
            options.followDefault = false;
        } else if (arg == L"--verbose" || arg == L"-v") {
            ma::g_verbose = true;
        } else {
            ma::LogError("unknown option \"%s\" (try --help)", ma::Utf8(arg).c_str());
            return 2;
        }
    }

    ma::ComApartment com;
    if (FAILED(com.hr())) {
        ma::LogError("could not initialize COM: %s", ma::HrText(com.hr()).c_str());
        return 1;
    }

    if (wantsList) {
        return ListDevices() ? 0 : 1;
    }

    ma::MirrorEngine engine;
    if (!engine.start(options)) return 1;

    ma::LogInfo("Latency: about %d ms behind the source. Press Ctrl+C to stop.",
                options.latencyMs);

    g_engine = &engine;
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    engine.run();
    SetConsoleCtrlHandler(ConsoleHandler, FALSE);
    g_engine = nullptr;

    ma::LogInfo("Stopped.");
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const int result = Run(argc, argv);
    PauseIfLaunchedFromExplorer();
    return result;
}

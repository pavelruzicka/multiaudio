#include "util.h"

#include <audioclient.h>

#include <cstdarg>
#include <cwctype>

namespace ma {

bool g_verbose = false;

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

bool ContainsNoCase(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    auto lower = [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); };
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() && lower(haystack[i + j]) == lower(needle[j])) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}

std::string HrText(HRESULT hr) {
    switch (hr) {
        case AUDCLNT_E_DEVICE_INVALIDATED:
            return "device was removed or reconfigured (AUDCLNT_E_DEVICE_INVALIDATED)";
        case AUDCLNT_E_DEVICE_IN_USE:
            return "device is in use by another program in exclusive mode (AUDCLNT_E_DEVICE_IN_USE)";
        case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
            return "device is held in exclusive mode (AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED)";
        case AUDCLNT_E_UNSUPPORTED_FORMAT:
            return "device does not support the requested format (AUDCLNT_E_UNSUPPORTED_FORMAT)";
        case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
            return "endpoint could not be created (AUDCLNT_E_ENDPOINT_CREATE_FAILED)";
        case AUDCLNT_E_SERVICE_NOT_RUNNING:
            return "the Windows Audio service is not running (AUDCLNT_E_SERVICE_NOT_RUNNING)";
        case AUDCLNT_E_RESOURCES_INVALIDATED:
            return "stream resources were invalidated (AUDCLNT_E_RESOURCES_INVALIDATED)";
        default:
            break;
    }

    char* message = nullptr;
    const DWORD chars = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&message), 0, nullptr);

    std::string text;
    if (chars && message) {
        text.assign(message, chars);
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
            text.pop_back();
        }
    }
    if (message) LocalFree(message);

    char code[32];
    snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
    if (text.empty()) return code;
    return text + " (" + code + ")";
}

namespace {

void Emit(FILE* stream, const char* prefix, const char* fmt, va_list args) {
    char line[1024];
    vsnprintf(line, sizeof(line), fmt, args);
    fprintf(stream, "%s%s\n", prefix, line);
    fflush(stream);
}

}  // namespace

void LogInfo(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Emit(stdout, "", fmt, args);
    va_end(args);
}

void LogVerbose(const char* fmt, ...) {
    if (!g_verbose) return;
    va_list args;
    va_start(args, fmt);
    Emit(stdout, "  . ", fmt, args);
    va_end(args);
}

void LogError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Emit(stderr, "error: ", fmt, args);
    va_end(args);
}

}  // namespace ma

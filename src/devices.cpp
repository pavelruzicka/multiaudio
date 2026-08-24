#include "devices.h"

#include <propsys.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace ma {
namespace {

// PKEY_Device_FriendlyName. Declared here so we do not need INITGUID, which
// would force every GUID in the Windows headers into this object file.
const PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

}  // namespace

bool CreateDeviceEnumerator(ComPtr<IMMDeviceEnumerator>* out) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        __uuidof(IMMDeviceEnumerator), enumerator.put_void());
    if (FAILED(hr)) {
        LogVerbose("no audio device enumerator: %s", HrText(hr).c_str());
        return false;
    }
    *out = std::move(enumerator);
    return true;
}

std::wstring GetDeviceId(IMMDevice* device) {
    if (!device) return {};
    CoMem<WCHAR> id;
    if (FAILED(device->GetId(id.put()))) return {};
    return id.get() ? std::wstring(id.get()) : std::wstring();
}

std::wstring GetFriendlyName(IMMDevice* device) {
    if (!device) return L"(unknown device)";

    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, properties.put()))) {
        return L"(unnamed device)";
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name = L"(unnamed device)";
    if (SUCCEEDED(properties->GetValue(kPkeyDeviceFriendlyName, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

bool ListRenderDevices(IMMDeviceEnumerator* enumerator, std::vector<DeviceInfo>* out) {
    if (!enumerator || !out) return false;
    out->clear();

    ComPtr<IMMDeviceCollection> collection;
    HRESULT hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put());
    if (FAILED(hr)) {
        LogVerbose("could not list playback devices: %s", HrText(hr).c_str());
        return false;
    }

    std::wstring defaultId;
    ComPtr<IMMDevice> defaultDevice;
    if (GetDefaultRenderDevice(enumerator, &defaultDevice)) {
        defaultId = GetDeviceId(defaultDevice.get());
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put()))) continue;

        DeviceInfo info;
        info.id = GetDeviceId(device.get());
        info.name = GetFriendlyName(device.get());
        info.isDefault = !info.id.empty() && info.id == defaultId;
        if (!info.id.empty()) out->push_back(std::move(info));
    }
    return true;
}

bool GetDefaultRenderDevice(IMMDeviceEnumerator* enumerator, ComPtr<IMMDevice>* out) {
    if (!enumerator || !out) return false;
    ComPtr<IMMDevice> device;
    const HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
    if (FAILED(hr)) {
        LogVerbose("no default playback device: %s", HrText(hr).c_str());
        return false;
    }
    *out = std::move(device);
    return true;
}

bool GetRenderDeviceById(IMMDeviceEnumerator* enumerator, const std::wstring& id,
                         ComPtr<IMMDevice>* out) {
    if (!enumerator || !out || id.empty()) return false;
    ComPtr<IMMDevice> device;
    const HRESULT hr = enumerator->GetDevice(id.c_str(), device.put());
    if (FAILED(hr)) return false;
    *out = std::move(device);
    return true;
}

bool ResolveRenderDevice(IMMDeviceEnumerator* enumerator, const std::wstring& spec,
                         DeviceInfo* out, std::wstring* reason) {
    auto explain = [reason](std::wstring text) {
        if (reason) *reason = std::move(text);
        return false;
    };

    if (!enumerator || !out) return false;

    std::vector<DeviceInfo> devices;
    if (!ListRenderDevices(enumerator, &devices)) {
        return explain(L"the Windows audio service is not answering");
    }
    if (devices.empty()) return explain(L"no playback device is plugged in");

    // An exact endpoint id, as stored by the tray menu.
    for (const auto& device : devices) {
        if (device.id == spec) {
            *out = device;
            return true;
        }
    }

    if (spec.empty() || spec == L"default") {
        for (const auto& device : devices) {
            if (device.isDefault) {
                *out = device;
                return true;
            }
        }
        *out = devices.front();
        return true;
    }

    // A bare number is an index from --list.
    if (!spec.empty() && std::all_of(spec.begin(), spec.end(), [](wchar_t c) { return iswdigit(c); })) {
        const long index = wcstol(spec.c_str(), nullptr, 10);
        if (index >= 1 && static_cast<size_t>(index) <= devices.size()) {
            *out = devices[static_cast<size_t>(index) - 1];
            return true;
        }
        return explain(L"there is no playback device number " + std::to_wstring(index));
    }

    // Otherwise match the friendly name.
    std::vector<const DeviceInfo*> matches;
    for (const auto& device : devices) {
        if (ContainsNoCase(device.name, spec)) matches.push_back(&device);
    }
    if (matches.empty()) return explain(L"no playback device matches \"" + spec + L"\"");
    if (matches.size() > 1) {
        return explain(L"\"" + spec + L"\" matches " + std::to_wstring(matches.size()) +
                       L" devices - be more specific");
    }
    *out = *matches.front();
    return true;
}

}  // namespace ma

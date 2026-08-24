// Thin wrappers over the MMDevice API: enumerate playback endpoints, resolve
// one from a command-line spec, read friendly names.
#pragma once

#include <mmdeviceapi.h>

#include <string>
#include <vector>

#include "util.h"

namespace ma {

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

bool CreateDeviceEnumerator(ComPtr<IMMDeviceEnumerator>* out);

std::wstring GetDeviceId(IMMDevice* device);
std::wstring GetFriendlyName(IMMDevice* device);

// Active playback (render) endpoints, in enumeration order.
bool ListRenderDevices(IMMDeviceEnumerator* enumerator, std::vector<DeviceInfo>* out);

bool GetDefaultRenderDevice(IMMDeviceEnumerator* enumerator, ComPtr<IMMDevice>* out);
bool GetRenderDeviceById(IMMDeviceEnumerator* enumerator, const std::wstring& id,
                         ComPtr<IMMDevice>* out);

// Resolves "default", a 1-based index from --list, or a name substring.
// Returns false and explains why if nothing (or more than one thing) matches.
bool ResolveRenderDevice(IMMDeviceEnumerator* enumerator, const std::wstring& spec,
                         DeviceInfo* out);

}  // namespace ma

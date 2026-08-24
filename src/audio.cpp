#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ma {
namespace {

// Defined locally so the project links the same way under MSVC and MinGW
// without pulling in <ksmedia.h> / INITGUID.
const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

constexpr float kInv15 = 1.0f / 32768.0f;
constexpr float kInv23 = 1.0f / 8388608.0f;
constexpr float kInv31 = 1.0f / 2147483648.0f;

inline float Clamp(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

inline int32_t ReadInt24(const uint8_t* p) {
    const int32_t value = static_cast<int32_t>(p[0]) | (static_cast<int32_t>(p[1]) << 8) |
                          (static_cast<int32_t>(p[2]) << 16);
    return (value & 0x800000) ? (value | ~0x00FFFFFF) : value;  // sign extend
}

inline void WriteInt24(uint8_t* p, int32_t value) {
    p[0] = static_cast<uint8_t>(value & 0xFF);
    p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
}

}  // namespace

bool DescribeFormat(const WAVEFORMATEX* wfx, StreamFormat* out) {
    if (!wfx || !out) return false;

    StreamFormat format;
    format.channels = wfx->nChannels;
    format.sampleRate = wfx->nSamplesPerSec;
    format.bytesPerFrame = wfx->nBlockAlign;

    WORD tag = wfx->wFormatTag;
    WORD bits = wfx->wBitsPerSample;

    if (tag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        if (IsEqualGUID(ext->SubFormat, kSubtypeFloat)) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (IsEqualGUID(ext->SubFormat, kSubtypePcm)) {
            tag = WAVE_FORMAT_PCM;
        } else {
            return false;  // compressed / unknown payload, nothing we can mirror
        }
    }

    if (tag == WAVE_FORMAT_IEEE_FLOAT && bits == 32) {
        format.type = SampleType::Float32;
    } else if (tag == WAVE_FORMAT_PCM && bits == 16) {
        format.type = SampleType::Int16;
    } else if (tag == WAVE_FORMAT_PCM && bits == 24) {
        format.type = SampleType::Int24;
    } else if (tag == WAVE_FORMAT_PCM && bits == 32) {
        format.type = SampleType::Int32;
    } else {
        return false;
    }

    if (!format.bytesPerFrame) {
        format.bytesPerFrame = format.channels * (bits / 8u);
    }
    if (!format.valid()) return false;

    *out = format;
    return true;
}

std::string FormatSummary(const StreamFormat& format) {
    const char* type = "?";
    switch (format.type) {
        case SampleType::Float32: type = "float32"; break;
        case SampleType::Int16: type = "int16"; break;
        case SampleType::Int24: type = "int24"; break;
        case SampleType::Int32: type = "int32"; break;
        case SampleType::Unsupported: type = "unsupported"; break;
    }
    char text[96];
    snprintf(text, sizeof(text), "%u Hz, %u ch, %s", format.sampleRate, format.channels, type);
    return text;
}

void ConvertToFloat(const void* src, const StreamFormat& format, size_t frames, float* dst) {
    const size_t samples = frames * format.channels;
    switch (format.type) {
        case SampleType::Float32:
            std::memcpy(dst, src, samples * sizeof(float));
            break;
        case SampleType::Int16: {
            const auto* in = static_cast<const int16_t*>(src);
            for (size_t i = 0; i < samples; ++i) dst[i] = in[i] * kInv15;
            break;
        }
        case SampleType::Int24: {
            const auto* in = static_cast<const uint8_t*>(src);
            for (size_t i = 0; i < samples; ++i) {
                dst[i] = static_cast<float>(ReadInt24(in + i * 3)) * kInv23;
            }
            break;
        }
        case SampleType::Int32: {
            const auto* in = static_cast<const int32_t*>(src);
            for (size_t i = 0; i < samples; ++i) dst[i] = static_cast<float>(in[i]) * kInv31;
            break;
        }
        case SampleType::Unsupported:
            std::memset(dst, 0, samples * sizeof(float));
            break;
    }
}

void ConvertFromFloat(const float* src, size_t frames, const StreamFormat& format, void* dst) {
    const size_t samples = frames * format.channels;
    switch (format.type) {
        case SampleType::Float32: {
            auto* out = static_cast<float*>(dst);
            for (size_t i = 0; i < samples; ++i) out[i] = Clamp(src[i]);
            break;
        }
        case SampleType::Int16: {
            auto* out = static_cast<int16_t*>(dst);
            for (size_t i = 0; i < samples; ++i) {
                out[i] = static_cast<int16_t>(std::lrintf(Clamp(src[i]) * 32767.0f));
            }
            break;
        }
        case SampleType::Int24: {
            auto* out = static_cast<uint8_t*>(dst);
            for (size_t i = 0; i < samples; ++i) {
                WriteInt24(out + i * 3, static_cast<int32_t>(std::lrintf(Clamp(src[i]) * 8388607.0f)));
            }
            break;
        }
        case SampleType::Int32: {
            auto* out = static_cast<int32_t*>(dst);
            for (size_t i = 0; i < samples; ++i) {
                out[i] = static_cast<int32_t>(std::llrintf(Clamp(src[i]) * 2147483520.0f));
            }
            break;
        }
        case SampleType::Unsupported:
            std::memset(dst, 0, frames * format.bytesPerFrame);
            break;
    }
}

void MapChannels(const float* src, unsigned srcChannels, float* dst, unsigned dstChannels,
                 size_t frames) {
    if (srcChannels == dstChannels) {
        std::memcpy(dst, src, frames * srcChannels * sizeof(float));
        return;
    }

    // Mono source: feed the front pair (or every channel of a mono sink).
    if (srcChannels == 1) {
        for (size_t f = 0; f < frames; ++f) {
            const float value = src[f];
            float* out = dst + f * dstChannels;
            std::memset(out, 0, dstChannels * sizeof(float));
            out[0] = value;
            if (dstChannels >= 2) out[1] = value;
        }
        return;
    }

    // Fold down to stereo or mono. Channel order is the standard WAVE order:
    // FL FR FC LFE BL BR SL SR, with LFE dropped.
    if (dstChannels <= 2) {
        constexpr float kAtt = 0.7071f;
        for (size_t f = 0; f < frames; ++f) {
            const float* in = src + f * srcChannels;
            float left = in[0];
            float right = in[1];
            switch (srcChannels) {
                case 2:
                    break;
                case 3:  // FL FR FC
                    left += kAtt * in[2];
                    right += kAtt * in[2];
                    break;
                case 4:  // FL FR BL BR (quad)
                    left += kAtt * in[2];
                    right += kAtt * in[3];
                    break;
                case 5:  // FL FR FC BL BR
                    left += kAtt * (in[2] + in[3]);
                    right += kAtt * (in[2] + in[4]);
                    break;
                case 6:  // FL FR FC LFE BL BR
                    left += kAtt * (in[2] + in[4]);
                    right += kAtt * (in[2] + in[5]);
                    break;
                case 8:  // FL FR FC LFE BL BR SL SR
                    left += kAtt * (in[2] + in[4] + in[6]);
                    right += kAtt * (in[2] + in[5] + in[7]);
                    break;
                default:  // unknown layout: alternate the extras left/right
                    for (unsigned c = 2; c < srcChannels; ++c) {
                        (c % 2 == 0 ? left : right) += kAtt * in[c];
                    }
                    break;
            }
            if (dstChannels == 1) {
                dst[f] = 0.5f * (left + right);
            } else {
                dst[f * 2] = left;
                dst[f * 2 + 1] = right;
            }
        }
        return;
    }

    // Anything else: copy the channels both sides have, silence the rest.
    const unsigned shared = std::min(srcChannels, dstChannels);
    for (size_t f = 0; f < frames; ++f) {
        const float* in = src + f * srcChannels;
        float* out = dst + f * dstChannels;
        std::memcpy(out, in, shared * sizeof(float));
        if (dstChannels > shared) {
            std::memset(out + shared, 0, (dstChannels - shared) * sizeof(float));
        }
    }
}

}  // namespace ma

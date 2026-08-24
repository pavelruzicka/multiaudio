// Format description plus the two conversions every stream needs: PCM bytes
// to/from float32, and a channel-count remap between source and sink.
#pragma once

#include "util.h"  // brings in windows.h first, which mmreg.h needs

#include <mmreg.h>

#include <cstddef>
#include <string>

namespace ma {

enum class SampleType { Unsupported, Float32, Int16, Int24, Int32 };

struct StreamFormat {
    unsigned channels = 0;
    unsigned sampleRate = 0;
    SampleType type = SampleType::Unsupported;
    unsigned bytesPerFrame = 0;

    bool valid() const { return channels > 0 && sampleRate > 0 && type != SampleType::Unsupported; }
};

// Reads a WAVEFORMATEX (or WAVEFORMATEXTENSIBLE) into our own description.
bool DescribeFormat(const WAVEFORMATEX* wfx, StreamFormat* out);

std::string FormatSummary(const StreamFormat& format);

// PCM bytes -> interleaved float32, and back. `frames` counts frames, not samples.
void ConvertToFloat(const void* src, const StreamFormat& format, size_t frames, float* dst);
void ConvertFromFloat(const float* src, size_t frames, const StreamFormat& format, void* dst);

// Interleaved float32 srcChannels -> dstChannels. Handles the common cases
// (mono/stereo/surround) and falls back to a straight copy of the channels
// that both sides have.
void MapChannels(const float* src, unsigned srcChannels, float* dst, unsigned dstChannels,
                 size_t frames);

}  // namespace ma

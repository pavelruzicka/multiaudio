// Remapping between channel layouts: stereo to a 7.1 device, a 5.1 film
// soundtrack down to a pair of headphones, and so on.
//
// Channel order is the standard WAVE order: FL FR FC LFE BL BR SL SR.
//
// Deliberately free of Windows headers so it can be tested on its own
// (see tests/audio_test.cpp).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace ma {

// Interleaved float32, srcChannels in, dstChannels out.
inline void MapChannels(const float* src, unsigned srcChannels, float* dst, unsigned dstChannels,
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

    // Fold down to stereo or mono, dropping the LFE.
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
                default:  // unknown layout: alternate the extras left and right
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

    // Into a device with more channels than the source: place the channels
    // both sides have and leave the rest silent.
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

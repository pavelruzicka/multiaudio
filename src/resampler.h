// Linear resampler with continuous drift correction.
//
// Two sound cards never agree on what "48000 Hz" means; they differ by a few
// parts per million, which is enough to drain or overflow a fixed buffer
// within minutes of playback. Rather than let that happen and click, each
// destination resamples with a ratio that is trimmed by a fraction of a
// percent to hold its buffer at a target fill level.
//
// Deliberately free of Windows headers so the arithmetic can be tested on its
// own (see tests/resampler_test.cpp).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace ma {

class DriftResampler {
public:
    // Never change the rate by more than this fraction: any more would be
    // audible as pitch drift.
    static constexpr double kMaxRateTrim = 0.002;  // 0.2 %
    // Roughly how long a correction is spread over.
    static constexpr double kTrimSeconds = 4.0;

    void configure(unsigned channels, unsigned sourceRate, unsigned destRate, size_t targetFrames,
                   size_t maxOutputFrames) {
        channels_ = channels ? channels : 1;
        sourceRate_ = sourceRate ? sourceRate : 1;
        targetFrames_ = targetFrames;
        baseStep_ = static_cast<double>(sourceRate_) / static_cast<double>(destRate ? destRate : 1);

        // Enough source frames for the largest output block, plus the extra
        // frame interpolation reads past the end.
        const size_t maxSource =
            static_cast<size_t>(static_cast<double>(maxOutputFrames) * baseStep_ *
                                (1.0 + kMaxRateTrim)) +
            4;
        pending_.assign(maxSource * channels_, 0.0f);
        reset();
    }

    void reset() {
        pendingFrames_ = 0;
        fracPos_ = 0.0;
        priming_ = true;
    }

    bool priming() const { return priming_; }
    size_t pendingFrames() const { return pendingFrames_; }
    double lastStep() const { return lastStep_; }
    unsigned long long underruns() const { return underruns_; }

    // Writes `outputFrames` interleaved frames at the destination rate into
    // `out`. `queued` is how many source frames are waiting upstream, and
    // `fetch(dst, n)` must copy up to n of them and return how many it wrote.
    //
    // Returns false when there was not enough audio, in which case `out` is
    // untouched and the caller should play silence: that happens while nothing
    // is playing on the source, and for the moment after audio starts again
    // while the buffer refills.
    template <class Fetch>
    bool process(size_t outputFrames, size_t queued, const Fetch& fetch, float* out) {
        if (outputFrames == 0) return true;

        const size_t buffered = queued + pendingFrames_;

        // After a gap, wait for the target amount of audio before restarting,
        // otherwise the next block would run dry again immediately.
        if (priming_) {
            if (buffered < targetFrames_) return false;
            priming_ = false;
            fracPos_ = 0.0;
        }

        const double error = static_cast<double>(buffered) - static_cast<double>(targetFrames_);
        double trim = error / (static_cast<double>(sourceRate_) * kTrimSeconds);
        trim = std::max(-kMaxRateTrim, std::min(kMaxRateTrim, trim));
        const double step = baseStep_ * (1.0 + trim);
        lastStep_ = step;

        // Interpolating the last output frame reads one source frame past it.
        const size_t required =
            static_cast<size_t>(std::floor(fracPos_ + static_cast<double>(outputFrames - 1) * step)) +
            2;
        if (pendingFrames_ < required) {
            if (pending_.size() < required * channels_) {
                pending_.resize(required * channels_);
            }
            pendingFrames_ += fetch(pending_.data() + pendingFrames_ * channels_,
                                    required - pendingFrames_);
        }
        if (pendingFrames_ < required) {
            // Not enough audio for a whole block. Drop the fragment that is
            // left: after a gap, playback restarts from freshly captured
            // audio rather than replaying the tail of what came before.
            priming_ = true;
            pendingFrames_ = 0;
            fracPos_ = 0.0;
            ++underruns_;
            return false;
        }

        for (size_t i = 0; i < outputFrames; ++i) {
            const double position = fracPos_ + static_cast<double>(i) * step;
            const size_t index = static_cast<size_t>(position);
            const float frac = static_cast<float>(position - static_cast<double>(index));
            const float* a = &pending_[index * channels_];
            const float* b = &pending_[(index + 1) * channels_];
            float* dst = &out[i * channels_];
            for (unsigned c = 0; c < channels_; ++c) {
                dst[c] = a[c] + (b[c] - a[c]) * frac;
            }
        }

        // Drop the source frames we are past, keeping the fractional offset so
        // the next block continues exactly where this one stopped.
        const double endPosition = fracPos_ + static_cast<double>(outputFrames) * step;
        size_t consumed = static_cast<size_t>(endPosition);
        if (consumed > pendingFrames_) consumed = pendingFrames_;
        fracPos_ = endPosition - static_cast<double>(consumed);
        if (consumed > 0) {
            std::memmove(pending_.data(), pending_.data() + consumed * channels_,
                         (pendingFrames_ - consumed) * channels_ * sizeof(float));
            pendingFrames_ -= consumed;
        }
        return true;
    }

private:
    unsigned channels_ = 1;
    unsigned sourceRate_ = 48000;
    size_t targetFrames_ = 0;
    double baseStep_ = 1.0;

    std::vector<float> pending_;  // source frames read but not yet consumed
    size_t pendingFrames_ = 0;
    double fracPos_ = 0.0;
    bool priming_ = true;
    double lastStep_ = 1.0;
    unsigned long long underruns_ = 0;
};

}  // namespace ma

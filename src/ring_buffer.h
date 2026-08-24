// Fixed-size ring of interleaved float frames. One producer (the capture
// thread) and one consumer (a sink's render thread) per instance; a plain
// mutex is more than fast enough for the block sizes we move around.
#pragma once

#include <cstring>
#include <mutex>
#include <vector>

namespace ma {

class FrameRing {
public:
    void reset(size_t channels, size_t capacityFrames) {
        std::lock_guard<std::mutex> lock(mutex_);
        channels_ = channels ? channels : 1;
        capacity_ = capacityFrames ? capacityFrames : 1;
        buffer_.assign(capacity_ * channels_, 0.0f);
        head_ = 0;
        count_ = 0;
        overruns_ = 0;
    }

    size_t capacity() const { return capacity_; }

    size_t available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = 0;
        count_ = 0;
    }

    // Appends frames, dropping the oldest data if the consumer has fallen
    // behind. Dropping keeps latency bounded instead of letting it grow.
    void write(const float* frames, size_t frameCount) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frameCount >= capacity_) {
            // Only the newest capacity_ frames can possibly fit.
            frames += (frameCount - capacity_) * channels_;
            frameCount = capacity_;
        }
        for (size_t i = 0; i < frameCount; ++i) {
            const size_t slot = (head_ + count_) % capacity_;
            std::memcpy(&buffer_[slot * channels_], &frames[i * channels_],
                        channels_ * sizeof(float));
            if (count_ == capacity_) {
                head_ = (head_ + 1) % capacity_;  // overwrite oldest
                ++overruns_;
            } else {
                ++count_;
            }
        }
    }

    // Reads up to frameCount frames; returns how many were actually copied.
    size_t read(float* out, size_t frameCount) {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t take = frameCount < count_ ? frameCount : count_;
        for (size_t i = 0; i < take; ++i) {
            const size_t slot = (head_ + i) % capacity_;
            std::memcpy(&out[i * channels_], &buffer_[slot * channels_], channels_ * sizeof(float));
        }
        head_ = (head_ + take) % capacity_;
        count_ -= take;
        return take;
    }

    unsigned long long overruns() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return overruns_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<float> buffer_;
    size_t channels_ = 1;
    size_t capacity_ = 1;
    size_t head_ = 0;
    size_t count_ = 0;
    unsigned long long overruns_ = 0;
};

}  // namespace ma

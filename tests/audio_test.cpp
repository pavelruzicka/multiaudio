// Tests for the parts of multiaudio that are just arithmetic: the drift
// resampler, the channel map and the ring buffer. These headers are free of
// Windows types, so this builds and runs anywhere:
//
//     g++ -std=c++17 -O2 -o audio_test tests/audio_test.cpp && ./audio_test

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <vector>

#include "../src/channel_map.h"
#include "../src/resampler.h"
#include "../src/ring_buffer.h"

namespace {

int g_failures = 0;

constexpr double kPi = 3.14159265358979323846;

void Check(bool condition, const char* what) {
    if (!condition) {
        printf("  FAIL  %s\n", what);
        ++g_failures;
    } else {
        printf("  ok    %s\n", what);
    }
}

// Stands in for the capture side: frames go in, the resampler pulls them out.
class SourceQueue {
public:
    explicit SourceQueue(unsigned channels) : channels_(channels) {}

    // A sawtooth ramp, one per frame. Values stay small so that float32 holds
    // them (and their interpolations) exactly; the test skips the wraps.
    static constexpr int kRampPeriod = 251;

    // Each channel gets its own offset, so a channel that is dropped,
    // duplicated or swapped shows up rather than hiding behind an identical
    // neighbour.
    static constexpr float kChannelOffset = 1000.0f;

    void produceRamp(size_t frames) {
        for (size_t i = 0; i < frames; ++i) {
            const float value = static_cast<float>(counter_ % kRampPeriod);
            ++counter_;
            for (unsigned c = 0; c < channels_; ++c) {
                data_.push_back(value + static_cast<float>(c) * kChannelOffset);
            }
        }
    }

    void produceConstant(size_t frames, float value) {
        for (size_t i = 0; i < frames * channels_; ++i) data_.push_back(value);
    }

    void produceSine(size_t frames, double frequency, double rate) {
        for (size_t i = 0; i < frames; ++i) {
            const float value = static_cast<float>(std::sin(2.0 * kPi * frequency * phase_ / rate));
            phase_ += 1.0;
            for (unsigned c = 0; c < channels_; ++c) data_.push_back(value);
        }
    }

    size_t frames() const { return data_.size() / channels_; }

    // Matches the Fetch contract of DriftResampler::process.
    size_t fetch(float* dst, size_t frames) {
        const size_t take = frames < this->frames() ? frames : this->frames();
        for (size_t i = 0; i < take * channels_; ++i) {
            dst[i] = data_.front();
            data_.pop_front();
        }
        return take;
    }

private:
    unsigned channels_;
    std::deque<float> data_;
    long long counter_ = 0;
    double phase_ = 0.0;
};

// A ramp stays a ramp through linear interpolation, so the difference between
// two consecutive output samples must equal the resampling step. A gap, a
// repeated frame or a jump at a block boundary shows up immediately.
//
// The source is a sawtooth, so samples at (or next to) a wrap are not part of
// the ramp and are excluded: a wrap shows up as a negative difference, and the
// sample interpolated across it sits between the two ramps.
void TestRampContinuity(unsigned sourceRate, unsigned destRate, const char* label) {
    constexpr unsigned kChannels = 2;
    constexpr size_t kBlock = 480;
    const size_t target = sourceRate / 25;  // 40 ms

    ma::DriftResampler resampler;
    resampler.configure(kChannels, sourceRate, destRate, target, kBlock);

    SourceQueue source(kChannels);
    std::vector<float> out(kBlock * kChannels);

    std::vector<float> history;   // channel 0 of every frame played
    std::vector<double> steps;    // the step in force for that frame
    std::vector<char> restarts;   // 1 where silence broke the stream
    bool channelsMatch = true;
    bool restarted = true;

    for (int block = 0; block < 400; ++block) {
        // What a source running at exactly the right speed would deliver,
        // plus the priming amount at the start.
        const size_t produce =
            static_cast<size_t>(kBlock * static_cast<double>(sourceRate) / destRate) +
            (block == 0 ? target + 8 : 0);
        source.produceRamp(produce);

        const bool produced = resampler.process(
            kBlock, source.frames(), [&](float* dst, size_t n) { return source.fetch(dst, n); },
            out.data());
        if (!produced) {
            restarted = true;
            continue;
        }

        for (size_t i = 0; i < kBlock; ++i) {
            const double gap = static_cast<double>(out[i * kChannels + 1]) -
                               static_cast<double>(out[i * kChannels]);
            if (std::fabs(gap - SourceQueue::kChannelOffset) > 0.01) channelsMatch = false;
            history.push_back(out[i * kChannels]);
            steps.push_back(resampler.lastStep());
            restarts.push_back(restarted ? 1 : 0);
            restarted = false;
        }
    }

    auto difference = [&](size_t i) {
        return static_cast<double>(history[i]) - static_cast<double>(history[i - 1]);
    };

    double worstError = 0.0;
    size_t compared = 0;
    for (size_t i = 1; i + 1 < history.size(); ++i) {
        if (restarts[i]) continue;
        // Skip the wrap itself and the samples on either side of it.
        if (difference(i) < 0.0 || difference(i + 1) < 0.0 ||
            (i >= 2 && difference(i - 1) < 0.0)) {
            continue;
        }
        worstError = std::max(worstError, std::fabs(difference(i) - steps[i]));
        ++compared;
    }

    printf("%s: %zu samples compared, worst step error %.6f (step %.6f)\n", label, compared,
           worstError, static_cast<double>(sourceRate) / destRate);
    Check(compared > 100000, "produced a long continuous stream");
    Check(channelsMatch, "channels stay separate and correctly interleaved");
    Check(worstError < 0.001, "no gaps, repeats or jumps between blocks");
}

// A source clock that runs slightly fast or slow must not drain or overflow
// the buffer: the resampling ratio has to absorb it. Buffer levels are
// measured where the resampler sees them, just before each block is pulled.
void TestDriftHeld(double clockError, double expectedMs, const char* label) {
    constexpr unsigned kChannels = 2;
    constexpr unsigned kRate = 48000;
    constexpr size_t kBlock = 480;     // 10 ms
    const size_t target = kRate / 25;  // 40 ms

    ma::DriftResampler resampler;
    resampler.configure(kChannels, kRate, kRate, target, kBlock);

    SourceQueue source(kChannels);
    std::vector<float> out(kBlock * kChannels);
    // Primed so that the first block sees exactly the target buffered.
    source.produceConstant(target - kBlock, 0.25f);

    double owed = 0.0;
    size_t minBuffered = target * 100;
    size_t maxBuffered = 0;
    unsigned long long underrunsAtStart = 0;
    const int blocks = 6000;         // 60 seconds of 10 ms blocks
    const int measureFrom = 2000;    // 20 s: five time constants of the trim

    for (int block = 0; block < blocks; ++block) {
        owed += kBlock * (1.0 + clockError);
        const size_t produce = static_cast<size_t>(owed);
        owed -= static_cast<double>(produce);
        source.produceConstant(produce, 0.25f);

        const size_t buffered = source.frames() + resampler.pendingFrames();
        resampler.process(kBlock, source.frames(),
                          [&](float* dst, size_t n) { return source.fetch(dst, n); }, out.data());

        if (block == measureFrom) underrunsAtStart = resampler.underruns();
        if (block > measureFrom) {
            minBuffered = std::min(minBuffered, buffered);
            maxBuffered = std::max(maxBuffered, buffered);
        }
    }

    const double lowMs = 1000.0 * static_cast<double>(minBuffered) / kRate;
    const double highMs = 1000.0 * static_cast<double>(maxBuffered) / kRate;
    printf("%s: buffer held between %.2f and %.2f ms (expected about %.1f)\n", label, lowMs, highMs,
           expectedMs);
    Check(resampler.underruns() == underrunsAtStart, "no underruns once running");
    Check(lowMs > expectedMs - 1.0 && highMs < expectedMs + 1.0,
          "buffer stayed put for a minute of playback");
}

// A 1 kHz tone should come out at the same amplitude, with no step between
// samples big enough to be heard as a click.
void TestToneFidelity() {
    constexpr unsigned kChannels = 1;
    constexpr unsigned kSourceRate = 48000;
    constexpr unsigned kDestRate = 44100;
    constexpr size_t kBlock = 441;
    const size_t target = kSourceRate / 25;

    ma::DriftResampler resampler;
    resampler.configure(kChannels, kSourceRate, kDestRate, target, kBlock);

    SourceQueue source(kChannels);
    std::vector<float> out(kBlock * kChannels);
    source.produceSine(target + kBlock, 1000.0, kSourceRate);

    double peak = 0.0;
    double energy = 0.0;
    double worstJump = 0.0;
    size_t samples = 0;
    bool havePrevious = false;
    float previous = 0.0f;

    for (int block = 0; block < 200; ++block) {
        source.produceSine(
            static_cast<size_t>(kBlock * double(kSourceRate) / kDestRate) + 1, 1000.0, kSourceRate);
        const bool produced = resampler.process(
            kBlock, source.frames(), [&](float* dst, size_t n) { return source.fetch(dst, n); },
            out.data());
        if (!produced) {
            havePrevious = false;
            continue;
        }
        for (size_t i = 0; i < kBlock; ++i) {
            const float value = out[i];
            peak = std::max(peak, std::fabs(static_cast<double>(value)));
            energy += static_cast<double>(value) * value;
            ++samples;
            if (havePrevious) {
                worstJump = std::max(worstJump, std::fabs(static_cast<double>(value - previous)));
            }
            previous = value;
            havePrevious = true;
        }
    }

    const double rms = std::sqrt(energy / static_cast<double>(samples));
    // A 1 kHz sine at 44.1 kHz moves at most 2*pi*1000/44100 = 0.1425 per sample.
    printf("1 kHz tone 48000 -> 44100: peak %.4f, rms %.4f (ideal 0.7071), largest step %.4f\n",
           peak, rms, worstJump);
    Check(peak > 0.98 && peak < 1.02, "amplitude preserved");
    Check(rms > 0.69 && rms < 0.72, "energy preserved");
    Check(worstJump < 0.2, "no discontinuity large enough to click");
}

// Nothing playing, then audio again: silence must be reported while the
// buffer is empty, and playback must resume cleanly once it refills.
void TestSilenceAndRecovery() {
    constexpr unsigned kChannels = 2;
    constexpr unsigned kRate = 48000;
    constexpr size_t kBlock = 480;
    const size_t target = kRate / 25;

    ma::DriftResampler resampler;
    resampler.configure(kChannels, kRate, kRate, target, kBlock);

    SourceQueue source(kChannels);
    std::vector<float> out(kBlock * kChannels, -1.0f);

    auto pull = [&] {
        return resampler.process(kBlock, source.frames(),
                                 [&](float* dst, size_t n) { return source.fetch(dst, n); },
                                 out.data());
    };

    Check(!pull(), "reports silence when nothing has been captured");

    source.produceConstant(target / 2, 0.5f);
    Check(!pull(), "still silent while the buffer is below the target");

    source.produceConstant(target, 0.5f);
    Check(pull(), "starts once the target is buffered");
    Check(out[0] == 0.5f, "plays the captured audio, not silence");

    // Drain everything, which starves it, then recover.
    for (int i = 0; i < 10; ++i) pull();
    const unsigned long long underruns = resampler.underruns();
    Check(underruns >= 1, "counted the underrun when the source stopped");
    Check(!pull(), "silent again after the source stops");
    Check(resampler.underruns() == underruns, "one underrun counted per gap, not per block");

    source.produceConstant(target * 2, 0.25f);
    Check(pull(), "recovers when audio comes back");
    Check(out[0] == 0.25f, "plays the new audio after recovering");
}

void TestRingBuffer() {
    ma::FrameRing ring;
    ring.reset(2, 8);

    const float frames[8] = {1, 1, 2, 2, 3, 3, 4, 4};
    ring.write(frames, 4);
    Check(ring.available() == 4, "ring counts written frames");

    float out[8] = {0};
    Check(ring.read(out, 2) == 2, "ring reads what was asked for");
    Check(out[0] == 1.0f && out[2] == 2.0f, "ring returns frames in order");
    Check(ring.available() == 2, "ring tracks what is left");

    // Overfill: the oldest frames are dropped so latency stays bounded.
    for (int i = 0; i < 6; ++i) ring.write(frames, 4);
    Check(ring.available() == 8, "ring never exceeds its capacity");
    Check(ring.overruns() > 0, "ring reports dropped frames");

    Check(ring.read(out, 4) == 4, "ring still readable after overflow");
    // 24 frames were written into a ring holding 8, so the last two copies of
    // 1,2,3,4 are what is left.
    Check(out[0] == 1.0f && out[2] == 2.0f && out[4] == 3.0f && out[6] == 4.0f,
          "ring keeps the newest frames when it overflows");

    ring.clear();
    Check(ring.available() == 0, "ring clears");
    Check(ring.read(out, 4) == 0, "empty ring reads nothing");
}

// The channel map is what stands between a stereo source and a device with a
// different layout. A bug here is heard as one silent side, so every case
// checks that left and right both survive and stay on their own side.
void TestChannelMap() {
    // Two frames, left and right clearly distinct.
    const float stereo[4] = {-0.5f, 0.25f, -0.75f, 0.5f};
    float out[16] = {0};

    ma::MapChannels(stereo, 2, out, 2, 2);
    Check(out[0] == -0.5f && out[1] == 0.25f && out[2] == -0.75f && out[3] == 0.5f,
          "stereo to stereo is untouched");

    // Into a 5.1 or 7.1 device: the front pair carries the audio, the rest is
    // silent. Both sides must still be there.
    for (unsigned channels : {6u, 8u}) {
        std::vector<float> wide(channels * 2, 99.0f);
        ma::MapChannels(stereo, 2, wide.data(), channels, 2);
        bool restSilent = true;
        for (unsigned c = 2; c < channels; ++c) {
            if (wide[c] != 0.0f || wide[channels + c] != 0.0f) restSilent = false;
        }
        Check(wide[0] == -0.5f && wide[1] == 0.25f, "stereo keeps both sides on a wide device");
        Check(wide[channels] == -0.75f && wide[channels + 1] == 0.5f,
              "stereo stays aligned on later frames of a wide device");
        Check(restSilent, "unused channels of a wide device are silent");
    }

    // Mono source into a stereo device: both ears, not just the left.
    const float mono[2] = {0.5f, -0.25f};
    ma::MapChannels(mono, 1, out, 2, 2);
    Check(out[0] == 0.5f && out[1] == 0.5f, "mono reaches both ears");
    Check(out[2] == -0.25f && out[3] == -0.25f, "mono reaches both ears on later frames");

    // Stereo into a mono device: the average, with neither side dropped.
    const float pair[2] = {1.0f, 0.0f};
    ma::MapChannels(pair, 2, out, 1, 1);
    Check(out[0] == 0.5f, "stereo folds to mono without dropping a side");

    // 5.1 down to headphones: audio present only on the left of the source
    // must stay left, and the centre must reach both.
    const float left51[6] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};    // FL only
    const float centre51[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};  // FC only
    const float lfe51[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};     // LFE only
    ma::MapChannels(left51, 6, out, 2, 1);
    Check(out[0] == 1.0f && out[1] == 0.0f, "5.1 front left stays left");
    ma::MapChannels(centre51, 6, out, 2, 1);
    Check(out[0] > 0.7f && out[0] == out[1], "5.1 centre reaches both sides equally");
    ma::MapChannels(lfe51, 6, out, 2, 1);
    Check(out[0] == 0.0f && out[1] == 0.0f, "5.1 LFE is dropped, not mixed in");

    // Quad: channels 2 and 3 are the back pair, not a centre.
    const float quad[4] = {0.0f, 0.0f, 1.0f, 0.0f};  // back left only
    ma::MapChannels(quad, 4, out, 2, 1);
    Check(out[0] > 0.7f && out[1] == 0.0f, "quad back left stays left");

    // 7.1 sides.
    float surround[8] = {0};
    surround[7] = 1.0f;  // side right only
    ma::MapChannels(surround, 8, out, 2, 1);
    Check(out[0] == 0.0f && out[1] > 0.7f, "7.1 side right stays right");

    // An unusual layout must not silence a side either.
    const float odd[5] = {0.5f, 0.25f, 0.1f, 0.1f, 0.1f};
    ma::MapChannels(odd, 5, out, 2, 1);
    Check(out[0] > 0.0f && out[1] > 0.0f, "an odd layout still feeds both sides");
}

}  // namespace

int main() {
    printf("channel map\n");
    TestChannelMap();

    printf("\nring buffer\n");
    TestRingBuffer();

    printf("\nramp continuity\n");
    TestRampContinuity(48000, 48000, "  48000 -> 48000");
    TestRampContinuity(48000, 44100, "  48000 -> 44100");
    TestRampContinuity(44100, 48000, "  44100 -> 48000");
    TestRampContinuity(96000, 48000, "  96000 -> 48000");

    printf("\nclock drift (target 40 ms)\n");
    TestDriftHeld(0.0, 40.0, "  matched clocks     ");
    TestDriftHeld(+50e-6, 40.2, "  source 50 ppm fast ");
    TestDriftHeld(-50e-6, 39.8, "  source 50 ppm slow ");
    TestDriftHeld(+500e-6, 42.0, "  source 500 ppm fast");
    TestDriftHeld(-500e-6, 38.0, "  source 500 ppm slow");

    printf("\ntone fidelity\n");
    TestToneFidelity();

    printf("\nsilence and recovery\n");
    TestSilenceAndRecovery();

    printf("\n%s\n", g_failures == 0 ? "all checks passed" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}

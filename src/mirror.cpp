#include "mirror.h"

#include <audioclient.h>
#include <avrt.h>

#include <algorithm>
#include <cstring>

#include "channel_map.h"
#include "resampler.h"
#include "ring_buffer.h"

namespace ma {
namespace {

constexpr REFERENCE_TIME kMsToRefTime = 10000;  // 100-ns units per millisecond

}  // namespace

// ---------------------------------------------------------------------------
// SinkStream: one playback device.
//
// The capture thread pushes source-rate float frames into ring_; this class's
// own thread pulls them out, resamples to the device's rate, remaps channels
// and writes them to WASAPI. Two devices never share a clock, so the resample
// ratio is trimmed continuously to hold the buffer at its target fill level.
// ---------------------------------------------------------------------------
class SinkStream {
public:
    SinkStream(ComPtr<IMMDevice> device, DeviceInfo info)
        : device_(std::move(device)), info_(std::move(info)) {}

    ~SinkStream() { stop(); }

    const DeviceInfo& info() const { return info_; }
    const StreamFormat& format() const { return sinkFormat_; }
    bool dead() const { return dead_.load(std::memory_order_relaxed); }
    unsigned long long underruns() const { return resampler_.underruns(); }

    bool open(const StreamFormat& sourceFormat, int latencyMs) {
        sourceFormat_ = sourceFormat;

        HRESULT hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       client_.put_void());
        if (FAILED(hr)) {
            LogError("cannot open \"%s\": %s", Utf8(info_.name).c_str(), HrText(hr).c_str());
            return false;
        }

        CoMem<WAVEFORMATEX> mixFormat;
        hr = client_->GetMixFormat(mixFormat.put());
        if (FAILED(hr) || !DescribeFormat(mixFormat.get(), &sinkFormat_)) {
            LogError("cannot use the audio format of \"%s\"", Utf8(info_.name).c_str());
            return false;
        }

        // Ask for a buffer twice the target latency so a late wake-up does not
        // immediately starve the device.
        const REFERENCE_TIME duration =
            static_cast<REFERENCE_TIME>(latencyMs) * 2 * kMsToRefTime;
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 duration, 0, mixFormat.get(), nullptr);
        if (FAILED(hr)) {
            LogError("cannot start playback on \"%s\": %s", Utf8(info_.name).c_str(),
                     HrText(hr).c_str());
            return false;
        }

        event_.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!event_) {
            LogError("cannot create an event for \"%s\"", Utf8(info_.name).c_str());
            return false;
        }
        hr = client_->SetEventHandle(event_.get());
        if (SUCCEEDED(hr)) hr = client_->GetBufferSize(&bufferFrames_);
        if (SUCCEEDED(hr)) {
            hr = client_->GetService(__uuidof(IAudioRenderClient), renderService_.put_void());
        }
        if (FAILED(hr)) {
            LogError("cannot set up playback on \"%s\": %s", Utf8(info_.name).c_str(),
                     HrText(hr).c_str());
            return false;
        }

        // How much audio to keep buffered ahead of this device.
        size_t targetFrames =
            static_cast<size_t>(sourceFormat_.sampleRate) * static_cast<size_t>(latencyMs) / 1000;
        if (targetFrames < 64) targetFrames = 64;

        ring_.reset(sourceFormat_.channels,
                    std::max<size_t>(targetFrames * 6, sourceFormat_.sampleRate));
        resampler_.configure(sourceFormat_.channels, sourceFormat_.sampleRate,
                             sinkFormat_.sampleRate, targetFrames, bufferFrames_);

        // Sized once here so the render thread never allocates.
        resampled_.assign(static_cast<size_t>(bufferFrames_) * sourceFormat_.channels, 0.0f);
        mapped_.assign(static_cast<size_t>(bufferFrames_) * sinkFormat_.channels, 0.0f);

        // Hand the device a buffer of silence before starting so it has
        // something to play while the first audio arrives.
        BYTE* buffer = nullptr;
        if (SUCCEEDED(renderService_->GetBuffer(bufferFrames_, &buffer))) {
            renderService_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
        }

        hr = client_->Start();
        if (FAILED(hr)) {
            LogError("cannot start \"%s\": %s", Utf8(info_.name).c_str(), HrText(hr).c_str());
            return false;
        }

        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread([this] { threadMain(); });
        return true;
    }

    // Called from the capture thread.
    void push(const float* frames, size_t frameCount) { ring_.write(frames, frameCount); }

    void stop() {
        running_.store(false, std::memory_order_relaxed);
        if (event_) SetEvent(event_.get());
        if (thread_.joinable()) thread_.join();
        if (client_) client_->Stop();
        renderService_.reset();
        client_.reset();
        event_.reset();
    }

private:
    void threadMain() {
        ComApartment com;

        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        while (running_.load(std::memory_order_relaxed)) {
            const DWORD wait = WaitForSingleObject(event_.get(), 200);
            if (!running_.load(std::memory_order_relaxed)) break;
            if (wait != WAIT_OBJECT_0) continue;  // timed out; look at the device again

            UINT32 padding = 0;
            HRESULT hr = client_->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                fail(hr);
                break;
            }
            if (bufferFrames_ <= padding) continue;
            const UINT32 frames = bufferFrames_ - padding;

            BYTE* buffer = nullptr;
            hr = renderService_->GetBuffer(frames, &buffer);
            if (FAILED(hr)) {
                fail(hr);
                break;
            }
            const bool wroteAudio = render(frames, buffer);
            renderService_->ReleaseBuffer(frames, wroteAudio ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
        }

        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    }

    void fail(HRESULT hr) {
        dead_.store(true, std::memory_order_relaxed);
        LogVerbose("\"%s\" stopped: %s", Utf8(info_.name).c_str(), HrText(hr).c_str());
    }

    // Fills `out` with `frameCount` device frames. Returns false when it wrote
    // silence, so the caller can flag the buffer as silent.
    bool render(UINT32 frameCount, BYTE* out) {
        if (frameCount == 0) return true;

        const bool produced =
            resampler_.process(frameCount, ring_.available(),
                               [this](float* dst, size_t frames) { return ring_.read(dst, frames); },
                               resampled_.data());
        if (!produced) {
            std::memset(out, 0, static_cast<size_t>(frameCount) * sinkFormat_.bytesPerFrame);
            return false;
        }

        MapChannels(resampled_.data(), sourceFormat_.channels, mapped_.data(),
                    sinkFormat_.channels, frameCount);
        ConvertFromFloat(mapped_.data(), frameCount, sinkFormat_, out);
        return true;
    }

    ComPtr<IMMDevice> device_;
    DeviceInfo info_;

    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> renderService_;
    Handle event_;
    UINT32 bufferFrames_ = 0;

    StreamFormat sourceFormat_;
    StreamFormat sinkFormat_;

    FrameRing ring_;             // source-rate float frames, filled by the capture thread
    DriftResampler resampler_;   // source rate -> this device's rate
    std::vector<float> resampled_;
    std::vector<float> mapped_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> dead_{false};
};

// ---------------------------------------------------------------------------
// Watches for devices being plugged in, removed, or made default.
// ---------------------------------------------------------------------------
class DeviceChangeNotifier : public IMMNotificationClient {
public:
    DeviceChangeNotifier(std::atomic<bool>* changed, bool followDefault)
        : changed_(changed), followDefault_(followDefault) {}

    virtual ~DeviceChangeNotifier() = default;

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&refCount_);
        if (count == 0) delete this;
        return static_cast<ULONG>(count);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(IMMNotificationClient))) {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        signal();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
        signal();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
        signal();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (followDefault_ && flow == eRender && role == eConsole) signal();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;
    }

private:
    void signal() { changed_->store(true, std::memory_order_relaxed); }

    LONG refCount_ = 1;
    std::atomic<bool>* changed_;
    bool followDefault_;
};

// ---------------------------------------------------------------------------
// MirrorEngine
// ---------------------------------------------------------------------------
MirrorEngine::MirrorEngine() {
    stopEvent_.attach(CreateEventW(nullptr, TRUE, FALSE, nullptr));
}

MirrorEngine::~MirrorEngine() {
    closeStreams();
    if (notifier_ && enumerator_) {
        enumerator_->UnregisterEndpointNotificationCallback(notifier_.get());
    }
}

bool MirrorEngine::start(const MirrorOptions& options) {
    options_ = options;

    if (!enumerator_ && !CreateDeviceEnumerator(&enumerator_)) return false;

    if (!notifier_) {
        // The constructor starts the object at one reference, which the
        // ComPtr now owns.
        notifier_.attach(new DeviceChangeNotifier(&devicesChanged_, options_.followDefault));
        const HRESULT hr = enumerator_->RegisterEndpointNotificationCallback(notifier_.get());
        if (FAILED(hr)) {
            LogVerbose("device change notifications unavailable: %s", HrText(hr).c_str());
        }
    }

    if (!openSource()) return false;
    if (!openSinks()) {
        closeStreams();
        return false;
    }

    captureFailed_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    captureThread_ = std::make_unique<std::thread>([this] { captureLoop(); });
    return true;
}

bool MirrorEngine::openSource() {
    if (!ResolveRenderDevice(enumerator_.get(), options_.source, &sourceInfo_)) return false;

    ComPtr<IMMDevice> device;
    if (!GetRenderDeviceById(enumerator_.get(), sourceInfo_.id, &device)) {
        LogError("the source device disappeared before it could be opened");
        return false;
    }

    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  captureClient_.put_void());
    if (FAILED(hr)) {
        LogError("cannot open the source device: %s", HrText(hr).c_str());
        return false;
    }

    CoMem<WAVEFORMATEX> mixFormat;
    hr = captureClient_->GetMixFormat(mixFormat.put());
    if (FAILED(hr) || !DescribeFormat(mixFormat.get(), &sourceFormat_)) {
        LogError("cannot use the audio format of the source device");
        return false;
    }

    // A generous capture buffer: the loopback stream is polled, and a late
    // poll should not cost us any audio.
    const REFERENCE_TIME duration = 500 * kMsToRefTime;
    hr = captureClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                    duration, 0, mixFormat.get(), nullptr);
    if (SUCCEEDED(hr)) {
        hr = captureClient_->GetService(__uuidof(IAudioCaptureClient), captureService_.put_void());
    }
    if (SUCCEEDED(hr)) hr = captureClient_->Start();
    if (FAILED(hr)) {
        LogError("cannot capture from \"%s\": %s", Utf8(sourceInfo_.name).c_str(),
                 HrText(hr).c_str());
        return false;
    }

    LogInfo("Source: %s  [%s]", Utf8(sourceInfo_.name).c_str(),
            FormatSummary(sourceFormat_).c_str());
    return true;
}

bool MirrorEngine::openSinks() {
    std::vector<DeviceInfo> devices;
    if (!ListRenderDevices(enumerator_.get(), &devices)) return false;

    for (const auto& info : devices) {
        if (info.id == sourceInfo_.id) continue;  // never mirror a device back into itself

        bool wanted = options_.include.empty();
        for (const auto& pattern : options_.include) {
            if (ContainsNoCase(info.name, pattern)) wanted = true;
        }
        for (const auto& pattern : options_.exclude) {
            if (ContainsNoCase(info.name, pattern)) wanted = false;
        }
        if (!wanted) {
            LogVerbose("skipping %s", Utf8(info.name).c_str());
            continue;
        }

        ComPtr<IMMDevice> device;
        if (!GetRenderDeviceById(enumerator_.get(), info.id, &device)) continue;

        auto sink = std::make_unique<SinkStream>(std::move(device), info);
        if (!sink->open(sourceFormat_, options_.latencyMs)) continue;

        const StreamFormat& format = sink->format();
        const char* note = "";
        if (format.sampleRate != sourceFormat_.sampleRate) note = "  (resampled)";
        LogInfo("  -> %s  [%s]%s", Utf8(info.name).c_str(), FormatSummary(format).c_str(), note);
        sinks_.push_back(std::move(sink));
    }

    if (sinks_.empty()) {
        LogError("nothing to mirror to: no other playback device is available");
        LogInfo("");
        LogInfo("Active playback devices Windows reports:");
        for (const auto& info : devices) {
            LogInfo("  - %s%s", Utf8(info.name).c_str(),
                    info.id == sourceInfo_.id ? "   <- the source" : "");
        }
        LogInfo("");
        if (devices.size() <= 1) {
            LogInfo("Only this one device is active, so there is no second device to");
            LogInfo("mirror to. Two pairs of headphones plugged into the same sound");
            LogInfo("card are one device to Windows, and this cannot split them - that");
            LogInfo("needs a second output: a USB headset, a USB sound card, HDMI, or a");
            LogInfo("virtual cable. Devices that are disabled or unplugged do not");
            LogInfo("appear here; check Sound settings > All sound devices.");
        } else if (!options_.include.empty() || !options_.exclude.empty()) {
            LogInfo("The --to / --exclude filters left nothing. Drop them, or match one");
            LogInfo("of the names above.");
        }
        return false;
    }
    return true;
}

void MirrorEngine::captureLoop() {
    ComApartment com;

    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    const DWORD pollMs = static_cast<DWORD>(std::max(1, options_.latencyMs / 4));

    while (running_.load(std::memory_order_relaxed)) {
        UINT32 packetFrames = 0;
        HRESULT hr = captureService_->GetNextPacketSize(&packetFrames);
        if (FAILED(hr)) {
            captureFailed_.store(true, std::memory_order_relaxed);
            LogVerbose("capture stopped: %s", HrText(hr).c_str());
            break;
        }

        while (packetFrames > 0 && running_.load(std::memory_order_relaxed)) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = captureService_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
            if (FAILED(hr)) {
                captureFailed_.store(true, std::memory_order_relaxed);
                LogVerbose("capture stopped: %s", HrText(hr).c_str());
                break;
            }

            if (frames > 0) {
                const size_t samples = static_cast<size_t>(frames) * sourceFormat_.channels;
                captureScratch_.resize(samples);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(captureScratch_.begin(), captureScratch_.end(), 0.0f);
                } else {
                    ConvertToFloat(data, sourceFormat_, frames, captureScratch_.data());
                }
                for (auto& sink : sinks_) {
                    sink->push(captureScratch_.data(), frames);
                }
            }

            captureService_->ReleaseBuffer(frames);
            if (FAILED(captureService_->GetNextPacketSize(&packetFrames))) break;
        }

        WaitForSingleObject(stopEvent_.get(), pollMs);
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

void MirrorEngine::closeStreams() {
    // The capture thread polls at a fraction of the latency, so clearing the
    // flag is enough to bring it down promptly.
    running_.store(false, std::memory_order_relaxed);
    if (captureThread_ && captureThread_->joinable()) captureThread_->join();
    captureThread_.reset();

    for (auto& sink : sinks_) sink->stop();
    sinks_.clear();

    if (captureClient_) captureClient_->Stop();
    captureService_.reset();
    captureClient_.reset();
}

void MirrorEngine::run() {
    while (!stopRequested_.load(std::memory_order_relaxed)) {
        WaitForSingleObject(stopEvent_.get(), 500);
        if (stopRequested_.load(std::memory_order_relaxed)) break;

        bool reopen = devicesChanged_.load(std::memory_order_relaxed) ||
                      captureFailed_.load(std::memory_order_relaxed);
        for (const auto& sink : sinks_) {
            if (sink->dead()) reopen = true;
        }
        if (!reopen) continue;

        // Device changes arrive in bursts (a USB headset appears as several
        // notifications); let them settle before reopening anything.
        WaitForSingleObject(stopEvent_.get(), 800);
        if (stopRequested_.load(std::memory_order_relaxed)) break;

        LogInfo("");
        LogInfo("Playback devices changed, reconnecting...");
        closeStreams();

        bool waitingAnnounced = false;
        while (!stopRequested_.load(std::memory_order_relaxed)) {
            devicesChanged_.store(false, std::memory_order_relaxed);
            if (start(options_)) break;
            closeStreams();
            if (!waitingAnnounced) {
                LogInfo("Waiting for a usable playback device...");
                waitingAnnounced = true;
            }
            WaitForSingleObject(stopEvent_.get(), 2000);
        }
    }

    for (const auto& sink : sinks_) {
        if (sink->underruns() > 0) {
            LogVerbose("%s: %llu buffer underruns", Utf8(sink->info().name).c_str(),
                       sink->underruns());
        }
    }
    closeStreams();
}

void MirrorEngine::requestStop() {
    stopRequested_.store(true, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    if (stopEvent_) SetEvent(stopEvent_.get());
}

}  // namespace ma

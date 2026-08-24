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

// Watches for devices being plugged in, removed, or made default. Every
// notification simply pokes the engine, which works out what changed.
// ---------------------------------------------------------------------------
class DeviceChangeNotifier : public IMMNotificationClient {
public:
    DeviceChangeNotifier(MirrorEngine* engine, bool followDefault)
        : engine_(engine), followDefault_(followDefault) {}

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
        engine_->notifyDevicesChanged();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
        engine_->notifyDevicesChanged();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
        engine_->notifyDevicesChanged();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (followDefault_ && flow == eRender && role == eConsole) engine_->notifyDevicesChanged();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;
    }

private:
    LONG refCount_ = 1;
    MirrorEngine* engine_;
    bool followDefault_;
};

// ---------------------------------------------------------------------------
// MirrorEngine
// ---------------------------------------------------------------------------
namespace {
constexpr DWORD kServiceTickMs = 500;    // how often the engine looks around
constexpr DWORD kOpenRetryMs = 10000;    // wait this long before retrying a device
}  // namespace

MirrorEngine::MirrorEngine() {
    wakeEvent_.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    captureStopEvent_.attach(CreateEventW(nullptr, TRUE, FALSE, nullptr));
}

MirrorEngine::~MirrorEngine() {
    requestStop();
    if (serviceThread_ && serviceThread_->joinable()) serviceThread_->join();
    serviceThread_.reset();
    closeStreams();
    if (notifier_ && enumerator_) {
        enumerator_->UnregisterEndpointNotificationCallback(notifier_.get());
    }
}

void MirrorEngine::setOptions(const MirrorOptions& options) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        options_ = options;
    }
    optionsChanged_.store(true, std::memory_order_relaxed);
    if (wakeEvent_) SetEvent(wakeEvent_.get());
}

MirrorOptions MirrorEngine::options() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return options_;
}

void MirrorEngine::setEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_relaxed);
    if (wakeEvent_) SetEvent(wakeEvent_.get());
}

EngineStatus MirrorEngine::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void MirrorEngine::setStatusCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    onStatusChanged_ = std::move(callback);
}

void MirrorEngine::notifyDevicesChanged() {
    devicesChanged_.store(true, std::memory_order_relaxed);
    if (wakeEvent_) SetEvent(wakeEvent_.get());
}

void MirrorEngine::requestStop() {
    stopRequested_.store(true, std::memory_order_relaxed);
    captureRunning_.store(false, std::memory_order_relaxed);
    if (captureStopEvent_) SetEvent(captureStopEvent_.get());
    if (wakeEvent_) SetEvent(wakeEvent_.get());
}

void MirrorEngine::startBackground() {
    serviceThread_ = std::make_unique<std::thread>([this] { serviceLoop(); });
}

void MirrorEngine::runForeground() { serviceLoop(); }

void MirrorEngine::serviceLoop() {
    ComApartment com;  // the engine and its threads live in the MTA

    while (!stopRequested_.load(std::memory_order_relaxed)) {
        service();
        WaitForSingleObject(wakeEvent_.get(), kServiceTickMs);
    }

    closeStreams();
    publish(EngineState::Off, {});
}

// One pass of the state machine. Everything it needs may be missing - no audio
// service, no default device, nothing else plugged in - and none of that is an
// error: it just means there is nothing to do yet.
void MirrorEngine::service() {
    if (stopRequested_.load(std::memory_order_relaxed)) return;

    if (!enabled_.load(std::memory_order_relaxed)) {
        closeStreams();
        publish(EngineState::Off, {});
        return;
    }

    if (!ensureEnumerator()) {
        publish(EngineState::Waiting, L"waiting for the Windows audio service");
        return;
    }

    // A different source or latency means rebuilding everything, because the
    // destinations are built around the source format. Switching a destination
    // on or off in the menu only needs the destinations resynced, which leaves
    // the devices already playing undisturbed.
    if (optionsChanged_.exchange(false, std::memory_order_relaxed)) {
        const MirrorOptions current = options();
        const bool rebuild = current.source != appliedOptions_.source ||
                             current.latencyMs != appliedOptions_.latencyMs ||
                             current.followDefault != appliedOptions_.followDefault;
        appliedOptions_ = current;
        sinkRetryAt_.clear();
        if (rebuild) {
            closeStreams();
        } else {
            devicesChanged_.store(true, std::memory_order_relaxed);
        }
    }

    const bool devicesChanged = devicesChanged_.exchange(false, std::memory_order_relaxed);
    if (devicesChanged) sinkRetryAt_.clear();  // a change is worth an immediate retry

    if (captureFailed_.load(std::memory_order_relaxed)) closeStreams();

    // Following the default device means noticing when it moves.
    if (devicesChanged && captureService_) {
        const MirrorOptions current = options();
        if (current.followDefault && (current.source.empty() || current.source == L"default")) {
            DeviceInfo preferred;
            if (ResolveRenderDevice(enumerator_.get(), L"default", &preferred) &&
                preferred.id != sourceInfo_.id) {
                closeStreams();
            }
        }
    }

    if (!captureService_ && !openSource()) {
        publish(EngineState::Waiting, sourceMessage_.empty()
                                          ? L"waiting for a playback device to mirror from"
                                          : sourceMessage_);
        return;
    }

    bool anyDead = false;
    {
        std::lock_guard<std::mutex> lock(sinksMutex_);
        for (const auto& sink : sinks_) {
            if (sink->dead()) anyDead = true;
        }
        if (sinks_.empty()) anyDead = true;  // keep looking while there is nowhere to play
    }
    if (devicesChanged || anyDead) syncSinks();

    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(sinksMutex_);
        count = sinks_.size();
    }
    if (count == 0) {
        publish(EngineState::Waiting, L"waiting for a second playback device");
    } else {
        publish(EngineState::Mirroring, {});
    }
}

bool MirrorEngine::ensureEnumerator() {
    if (enumerator_) return true;
    if (!CreateDeviceEnumerator(&enumerator_)) return false;

    const MirrorOptions current = options();
    notifier_.attach(new DeviceChangeNotifier(this, current.followDefault));
    const HRESULT hr = enumerator_->RegisterEndpointNotificationCallback(notifier_.get());
    if (FAILED(hr)) {
        LogVerbose("device change notifications unavailable: %s", HrText(hr).c_str());
    }
    return true;
}

bool MirrorEngine::openSource() {
    const MirrorOptions current = options();

    sourceMessage_.clear();
    if (!ResolveRenderDevice(enumerator_.get(), current.source, &sourceInfo_, &sourceMessage_)) {
        return false;
    }

    ComPtr<IMMDevice> device;
    if (!GetRenderDeviceById(enumerator_.get(), sourceInfo_.id, &device)) {
        sourceMessage_ = L"the device to mirror from disappeared";
        return false;
    }

    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  captureClient_.put_void());
    if (FAILED(hr)) {
        LogVerbose("cannot open the source device: %s", HrText(hr).c_str());
        captureClient_.reset();
        return false;
    }

    CoMem<WAVEFORMATEX> mixFormat;
    hr = captureClient_->GetMixFormat(mixFormat.put());
    if (FAILED(hr) || !DescribeFormat(mixFormat.get(), &sourceFormat_)) {
        LogVerbose("cannot use the audio format of the source device");
        captureClient_.reset();
        return false;
    }

    // A generous capture buffer: the loopback stream is polled, and a late poll
    // should not cost us any audio.
    const REFERENCE_TIME duration = 500 * kMsToRefTime;
    hr = captureClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                    duration, 0, mixFormat.get(), nullptr);
    if (SUCCEEDED(hr)) {
        hr = captureClient_->GetService(__uuidof(IAudioCaptureClient), captureService_.put_void());
    }
    if (SUCCEEDED(hr)) hr = captureClient_->Start();
    if (FAILED(hr)) {
        LogVerbose("cannot capture from \"%s\": %s", Utf8(sourceInfo_.name).c_str(),
                   HrText(hr).c_str());
        captureService_.reset();
        captureClient_.reset();
        return false;
    }

    captureFailed_.store(false, std::memory_order_relaxed);
    captureRunning_.store(true, std::memory_order_relaxed);
    ResetEvent(captureStopEvent_.get());
    captureThread_ = std::make_unique<std::thread>([this] { captureLoop(); });

    LogVerbose("capturing from %s [%s]", Utf8(sourceInfo_.name).c_str(),
               FormatSummary(sourceFormat_).c_str());
    return true;
}

void MirrorEngine::closeSource() {
    captureRunning_.store(false, std::memory_order_relaxed);
    if (captureStopEvent_) SetEvent(captureStopEvent_.get());
    if (captureThread_ && captureThread_->joinable()) captureThread_->join();
    captureThread_.reset();

    if (captureClient_) captureClient_->Stop();
    captureService_.reset();
    captureClient_.reset();
    captureFailed_.store(false, std::memory_order_relaxed);
    sourceInfo_ = DeviceInfo{};
}

// Brings the set of open destinations in line with what is plugged in and
// wanted. Devices that appear are added without disturbing the ones already
// playing; devices that vanish are dropped.
void MirrorEngine::syncSinks() {
    const MirrorOptions current = options();

    std::vector<DeviceInfo> devices;
    if (!ListRenderDevices(enumerator_.get(), &devices)) return;

    std::vector<DeviceInfo> wanted;
    for (const auto& info : devices) {
        if (info.id == sourceInfo_.id) continue;  // never mirror a device into itself

        bool include = current.include.empty();
        for (const auto& pattern : current.include) {
            if (ContainsNoCase(info.name, pattern)) include = true;
        }
        for (const auto& pattern : current.exclude) {
            if (ContainsNoCase(info.name, pattern)) include = false;
        }
        for (const auto& id : current.excludeIds) {
            if (info.id == id) include = false;
        }
        if (include) wanted.push_back(info);
    }

    // Drop the ones that are gone, unwanted, or have stopped working. Stopping
    // a stream joins its thread, so do that outside the lock.
    std::vector<std::unique_ptr<SinkStream>> doomed;
    {
        std::lock_guard<std::mutex> lock(sinksMutex_);
        for (size_t i = sinks_.size(); i > 0; --i) {
            auto& sink = sinks_[i - 1];
            const bool stillWanted =
                !sink->dead() && std::any_of(wanted.begin(), wanted.end(),
                                             [&](const DeviceInfo& device) {
                                                 return device.id == sink->info().id;
                                             });
            if (!stillWanted) {
                doomed.push_back(std::move(sink));
                sinks_.erase(sinks_.begin() + static_cast<ptrdiff_t>(i - 1));
            }
        }
    }
    for (auto& sink : doomed) {
        LogVerbose("dropping %s", Utf8(sink->info().name).c_str());
        sink->stop();
    }
    doomed.clear();

    // Add the ones we do not have yet. Opening a device can take a moment, so
    // it happens outside the lock as well.
    const ULONGLONG now = GetTickCount64();
    for (const auto& info : wanted) {
        bool alreadyOpen = false;
        {
            std::lock_guard<std::mutex> lock(sinksMutex_);
            for (const auto& sink : sinks_) {
                if (sink->info().id == info.id) alreadyOpen = true;
            }
        }
        if (alreadyOpen) continue;

        // A device held in exclusive mode by another program will not open;
        // do not hammer it every half second.
        const auto retry = sinkRetryAt_.find(info.id);
        if (retry != sinkRetryAt_.end() && now < retry->second) continue;

        ComPtr<IMMDevice> device;
        if (!GetRenderDeviceById(enumerator_.get(), info.id, &device)) continue;

        auto sink = std::make_unique<SinkStream>(std::move(device), info);
        if (!sink->open(sourceFormat_, current.latencyMs)) {
            sinkRetryAt_[info.id] = now + kOpenRetryMs;
            continue;
        }
        sinkRetryAt_.erase(info.id);
        LogVerbose("mirroring to %s [%s]", Utf8(info.name).c_str(),
                   FormatSummary(sink->format()).c_str());

        std::lock_guard<std::mutex> lock(sinksMutex_);
        sinks_.push_back(std::move(sink));
    }
}

void MirrorEngine::closeSinks() {
    std::vector<std::unique_ptr<SinkStream>> closing;
    {
        std::lock_guard<std::mutex> lock(sinksMutex_);
        closing.swap(sinks_);
    }
    for (auto& sink : closing) sink->stop();
}

void MirrorEngine::closeStreams() {
    closeSource();
    closeSinks();
}

void MirrorEngine::captureLoop() {
    ComApartment com;

    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    const DWORD pollMs = static_cast<DWORD>(std::max(1, options().latencyMs / 4));

    while (captureRunning_.load(std::memory_order_relaxed)) {
        UINT32 packetFrames = 0;
        HRESULT hr = captureService_->GetNextPacketSize(&packetFrames);
        if (FAILED(hr)) {
            captureFailed_.store(true, std::memory_order_relaxed);
            LogVerbose("capture stopped: %s", HrText(hr).c_str());
            break;
        }

        while (packetFrames > 0 && captureRunning_.load(std::memory_order_relaxed)) {
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
                std::lock_guard<std::mutex> lock(sinksMutex_);
                for (auto& sink : sinks_) {
                    sink->push(captureScratch_.data(), frames);
                }
            }

            captureService_->ReleaseBuffer(frames);
            if (FAILED(captureService_->GetNextPacketSize(&packetFrames))) break;
        }

        WaitForSingleObject(captureStopEvent_.get(), pollMs);
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (captureFailed_.load(std::memory_order_relaxed) && wakeEvent_) SetEvent(wakeEvent_.get());
}

void MirrorEngine::publish(EngineState state, std::wstring message) {
    EngineStatus fresh;
    fresh.state = state;
    fresh.message = std::move(message);
    if (state != EngineState::Off) {
        fresh.source = sourceInfo_.name;
        std::lock_guard<std::mutex> lock(sinksMutex_);
        for (const auto& sink : sinks_) fresh.sinks.push_back(sink->info().name);
    }

    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_ == fresh) return;
        status_ = std::move(fresh);
        callback = onStatusChanged_;
    }
    if (callback) callback();
}

}  // namespace ma

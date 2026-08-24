// The mirroring engine: one WASAPI loopback capture on the source device, and
// one shared-mode render stream per destination device.
//
// The engine is a service that keeps running whether or not any devices are
// present. It watches for endpoints appearing and disappearing and adds or
// drops destinations as they come and go, so starting it before the headphones
// are plugged in is normal rather than a failure.
#pragma once

#include <audioclient.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"
#include "devices.h"
#include "util.h"

namespace ma {

struct MirrorOptions {
    std::wstring source = L"default";       // "default", an index, a name, or an endpoint id
    std::vector<std::wstring> include;      // --to, empty means "every other device"
    std::vector<std::wstring> exclude;      // --exclude, matched against the name
    std::vector<std::wstring> excludeIds;   // endpoints switched off in the tray menu
    int latencyMs = 40;                     // buffered audio ahead of each destination
    bool followDefault = true;              // follow the Windows default device
};

enum class EngineState {
    Off,        // switched off by the user
    Waiting,    // switched on, but something is missing (see message)
    Mirroring,  // audio is flowing
};

struct EngineStatus {
    EngineState state = EngineState::Off;
    std::wstring source;
    std::vector<std::wstring> sinks;
    int latencyMs = 0;     // what the destinations are actually behind by
    std::wstring message;  // why it is waiting

    bool operator==(const EngineStatus& other) const {
        return state == other.state && source == other.source && sinks == other.sinks &&
               latencyMs == other.latencyMs && message == other.message;
    }
    bool operator!=(const EngineStatus& other) const { return !(*this == other); }
};

class SinkStream;
class DeviceChangeNotifier;

class MirrorEngine {
public:
    MirrorEngine();
    ~MirrorEngine();

    MirrorEngine(const MirrorEngine&) = delete;
    MirrorEngine& operator=(const MirrorEngine&) = delete;

    // Settings can be changed at any time; they take effect on the next pass.
    void setOptions(const MirrorOptions& options);
    MirrorOptions options() const;

    void setEnabled(bool enabled);
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    EngineStatus status() const;

    // Invoked on the service thread whenever the status changes. Keep it short
    // and do not call back into the engine from it; post to your UI instead.
    void setStatusCallback(std::function<void()> callback);

    void runForeground();   // service loop on this thread, until requestStop()
    void startBackground();  // service loop on its own thread
    void requestStop();     // safe from any thread, including a console handler

    // Called by the device-change notifier.
    void notifyDevicesChanged();

private:
    void serviceLoop();
    void service();
    bool ensureEnumerator();
    bool openSource();
    void closeSource();
    void syncSinks();
    void closeSinks();
    void closeStreams();
    void captureLoop();
    void publish(EngineState state, std::wstring message);

    mutable std::mutex mutex_;  // guards options_ and status_
    MirrorOptions options_;
    EngineStatus status_;
    std::function<void()> onStatusChanged_;

    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<DeviceChangeNotifier> notifier_;

    MirrorOptions appliedOptions_;  // what the open streams were built from
    std::wstring sourceMessage_;    // why the source could not be opened
    DeviceInfo sourceInfo_;
    ComPtr<IAudioClient> captureClient_;
    ComPtr<IAudioCaptureClient> captureService_;
    StreamFormat sourceFormat_;

    mutable std::mutex sinksMutex_;  // guards sinks_, held by the capture thread
    std::vector<std::unique_ptr<SinkStream>> sinks_;
    std::map<std::wstring, ULONGLONG> sinkRetryAt_;  // back off devices that will not open

    std::vector<float> captureScratch_;

    std::atomic<bool> enabled_{true};
    std::atomic<bool> devicesChanged_{false};
    std::atomic<bool> captureFailed_{false};
    std::atomic<bool> optionsChanged_{false};
    std::atomic<bool> captureRunning_{false};
    std::atomic<bool> stopRequested_{false};

    Handle wakeEvent_;         // wakes the service loop early
    Handle captureStopEvent_;  // ends the capture thread's poll wait
    std::unique_ptr<std::thread> captureThread_;
    std::unique_ptr<std::thread> serviceThread_;
};

}  // namespace ma

// The mirroring engine: one WASAPI loopback capture on the source device, and
// one shared-mode render stream per destination device.
#pragma once

#include <audioclient.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"
#include "devices.h"
#include "util.h"

namespace ma {

struct MirrorOptions {
    std::wstring source = L"default";       // "default", an index, or a name substring
    std::vector<std::wstring> include;      // --to, empty means "every other device"
    std::vector<std::wstring> exclude;      // --exclude
    int latencyMs = 40;                     // buffered audio ahead of each sink
    bool followDefault = true;              // restart when the default device changes
};

class SinkStream;
class DeviceChangeNotifier;

class MirrorEngine {
public:
    MirrorEngine();
    ~MirrorEngine();

    MirrorEngine(const MirrorEngine&) = delete;
    MirrorEngine& operator=(const MirrorEngine&) = delete;

    // Opens the source and every destination. Returns false if the source
    // could not be opened or nothing was left to mirror to.
    bool start(const MirrorOptions& options);

    // Blocks until stop() is called, restarting the streams when devices are
    // plugged in or removed.
    void run();

    // Safe to call from a console control handler.
    void requestStop();

private:
    bool openSource();
    bool openSinks();
    void closeStreams();
    void captureLoop();

    MirrorOptions options_;
    DeviceInfo sourceInfo_;

    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IAudioClient> captureClient_;
    ComPtr<IAudioCaptureClient> captureService_;
    StreamFormat sourceFormat_;

    std::vector<std::unique_ptr<SinkStream>> sinks_;
    std::vector<float> captureScratch_;

    ComPtr<DeviceChangeNotifier> notifier_;
    std::atomic<bool> devicesChanged_{false};
    std::atomic<bool> captureFailed_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};

    Handle stopEvent_;
    std::unique_ptr<std::thread> captureThread_;
};

}  // namespace ma

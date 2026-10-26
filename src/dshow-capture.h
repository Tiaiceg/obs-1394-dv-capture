/*
 * dshow-capture.h
 *
 * Minimal DirectShow capture engine for legacy IEEE-1394 (FireWire) DV
 * camcorders exposed through Windows as "Microsoft DV Camera and VCR".
 *
 * The video graph uses ICaptureGraphBuilder2::RenderStream(), which inserts
 * the system "DV Splitter" and "DV Video Decoder". The audio branch finds
 * and reuses that splitter, rendering its PCM output separately. Rendering
 * audio directly from the camera source cannot work because MSDV exposes
 * interleaved DV rather than a standalone audio pin.
 */

#pragma once

#include <windows.h>
#include <dshow.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "qedit-defs.h"

namespace dvcap {

// UTF-8 converted device description.
struct DvDevice {
    std::string friendlyName;  // shown in the UI
    std::string monikerPath;   // stable device identifier (UTF-8)
};

struct DvVideoFrame {
    int width = 0;
    int height = 0;
    bool topDown = false;  // true if scanline 0 is already at the top
    const uint8_t *data = nullptr;
    size_t size = 0;
    int64_t timestampNs = 0;  // stream time, rebased to 0
};

struct DvAudioFrame {
    int channels = 0;
    int samplesPerSec = 0;
    int bitsPerSample = 0;
    const uint8_t *data = nullptr;
    size_t size = 0;
    int64_t timestampNs = 0;  // stream time, rebased to 0
};

using VideoCallback = std::function<void(const DvVideoFrame &)>;
using AudioCallback = std::function<void(const DvAudioFrame &)>;
using LogCallback = std::function<void(const std::string &)>;

// Enumerate video input devices (DirectShow video input device category).
// Safe to call on any thread that has COM initialised; the function performs
// its own COM init/uninit pair.
std::vector<DvDevice> EnumerateDevices();

class DshowCapture {
public:
    DshowCapture();
    ~DshowCapture();

    DshowCapture(const DshowCapture &) = delete;
    DshowCapture &operator=(const DshowCapture &) = delete;

    // monikerPath: value from DvDevice::monikerPath.
    // Returns false if the device cannot be opened or the graph cannot be
    // built. Callbacks are invoked on DirectShow streaming threads and must
    // be cheap and thread-safe.
    bool Start(const std::string &monikerPathUtf8,
               bool captureAudio,
               VideoCallback videoCb,
               AudioCallback audioCb,
               LogCallback logCb = {});

    void Stop();

    bool running() const { return running_.load(); }

    // Last error, UTF-8.
    const std::string &lastError() const { return lastError_; }

private:
    struct CallbackImpl;

    void CaptureThread(std::wstring monikerPath, bool captureAudio);
    void SetError(const char *msg, HRESULT hr);
    void Log(const std::string &msg);
    void Teardown();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};

    // Graph objects (owned by the capture thread).
    IGraphBuilder *graph_ = nullptr;
    ICaptureGraphBuilder2 *captureGraph_ = nullptr;
    IMediaControl *control_ = nullptr;
    IMediaEventEx *mediaEvent_ = nullptr;
    IBaseFilter *sourceFilter_ = nullptr;
    IBaseFilter *dvSplitter_ = nullptr;
    IBaseFilter *videoGrabberFilter_ = nullptr;
    IBaseFilter *videoNullRenderer_ = nullptr;
    IBaseFilter *audioGrabberFilter_ = nullptr;
    IBaseFilter *audioNullRenderer_ = nullptr;
    ISampleGrabber *videoGrabber_ = nullptr;
    ISampleGrabber *audioGrabber_ = nullptr;
    CallbackImpl *videoCallback_ = nullptr;
    CallbackImpl *audioCallback_ = nullptr;

    VideoCallback userVideoCb_;
    AudioCallback userAudioCb_;
    LogCallback logCb_;
    std::string lastError_;
};

}  // namespace dvcap

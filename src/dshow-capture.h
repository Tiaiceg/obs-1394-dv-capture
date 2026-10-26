/*
 * dshow-capture.h
 *
 * Minimal DirectShow capture engine for legacy IEEE-1394 (FireWire) DV
 * camcorders exposed through Windows as "Microsoft DV Camera and VCR".
 *
 * The key difference from a generic DirectShow capture source is that the
 * filter graph is built with ICaptureGraphBuilder2::RenderStream(), which
 * automatically inserts the system "DV Splitter" and "DV Video Decoder"
 * filters, turning the raw DVSD (dvsd / DVCPRO) stream delivered over 1394
 * into uncompressed YUY2 video and PCM audio. Generic capture sources that
 * connect pins directly never get a decoded stream and show a black frame.
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
               AudioCallback audioCb);

    void Stop();

    bool running() const { return running_.load(); }

    // Last error, UTF-8.
    const std::string &lastError() const { return lastError_; }

private:
    struct CallbackImpl;

    void CaptureThread(std::wstring monikerPath, bool captureAudio);
    void SetError(const char *msg, HRESULT hr);
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
    std::string lastError_;
};

}  // namespace dvcap

/*
 * dshow-capture.cpp - DirectShow capture engine for IEEE-1394 DV camcorders.
 */

#include "dshow-capture.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <cctype>

// IID_PPV_ARG is missing from the MinGW-w64 headers (MSVC-only convenience
// macro), so spell the interface ID out explicitly.
#define PPV(T, ptr) IID_##T, reinterpret_cast<void **>(ptr)

namespace dvcap {

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

static std::wstring Utf8ToWide(const std::string &utf8) {
    if (utf8.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

static std::string WideToUtf8(const std::wstring &wide) {
    if (wide.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                static_cast<int>(wide.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                        static_cast<int>(wide.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

template <class T>
static void SafeRelease(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Device enumeration
// ---------------------------------------------------------------------------

// A real IEEE-1394 DV camcorder is exposed through AV/C with a moniker path
// like "@device:pnp:\\?\avc#panasonic&mod_0&camcorder&dv#...". Software
// virtual cameras (Corel, etc.) appear as "@device:sw:{...}". Recognise the
// former so it is listed (and selected) first.
static bool IsLikelyFirewireDv(const std::string &path) {
    std::string lower;
    lower.reserve(path.size());
    for (unsigned char c : path)
        lower.push_back(static_cast<char>(std::tolower(c)));
    static const char *markers[] = {"avc#", "camcorder", "&dv#", "#dv"};
    for (const char *m : markers) {
        if (lower.find(m) != std::string::npos)
            return true;
    }
    return false;
}

std::vector<DvDevice> EnumerateDevices() {
    std::vector<DvDevice> devices;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInited = SUCCEEDED(hr);
    if (!comInited && hr != RPC_E_CHANGED_MODE)
        return devices;

    ICreateDevEnum *devEnum = nullptr;
    IEnumMoniker *enumMoniker = nullptr;

    do {
        hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                              CLSCTX_INPROC_SERVER, PPV(ICreateDevEnum,
                                                                &devEnum));
        if (FAILED(hr))
            break;

        hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                            &enumMoniker, 0);
        if (FAILED(hr) || !enumMoniker)
            break;  // No video input devices installed.

        IMoniker *moniker = nullptr;
        while (enumMoniker->Next(1, &moniker, nullptr) == S_OK) {
            DvDevice dev;

            IPropertyBag *bag = nullptr;
            if (SUCCEEDED(moniker->BindToStorage(
                    nullptr, nullptr, PPV(IPropertyBag, &bag)))) {
                VARIANT var;
                VariantInit(&var);
                if (SUCCEEDED(bag->Read(L"FriendlyName", &var, nullptr)) &&
                    var.vt == VT_BSTR) {
                    dev.friendlyName =
                        WideToUtf8(var.bstrVal ? var.bstrVal : L"");
                }
                VariantClear(&var);
                bag->Release();
            }

            LPOLESTR displayName = nullptr;
            if (SUCCEEDED(moniker->GetDisplayName(nullptr, nullptr,
                                                  &displayName))) {
                if (displayName) {
                    dev.monikerPath = WideToUtf8(displayName);
                    CoTaskMemFree(displayName);
                }
            }

            if (!dev.monikerPath.empty()) {
                if (dev.friendlyName.empty())
                    dev.friendlyName = dev.monikerPath;
                devices.push_back(std::move(dev));
            }
            moniker->Release();
        }
    } while (false);

    SafeRelease(enumMoniker);
    SafeRelease(devEnum);
    if (comInited)
        CoUninitialize();

    // Put real 1394 DV camcorders first so the default selection is the
    // camcorder rather than a software virtual camera.
    std::stable_partition(devices.begin(), devices.end(),
                          [](const DvDevice &d) {
                              return IsLikelyFirewireDv(d.monikerPath);
                          });
    return devices;
}

// ---------------------------------------------------------------------------
// Sample grabber callback
// ---------------------------------------------------------------------------

struct DshowCapture::CallbackImpl : public ISampleGrabberCB {
    bool video = true;
    std::atomic<LONG> refCount{1};

    // Connected media format (populated after the graph is connected).
    bool haveFormat = false;
    int width = 0;
    int height = 0;
    bool topDown = false;
    int channels = 0;
    int samplesPerSec = 0;
    int bitsPerSample = 0;
    LONGLONG averageFrameTime = 0;  // 100-ns units, video fallback

    std::atomic<LONGLONG> firstTime{-1};
    LONGLONG fallbackTime = 0;

    VideoCallback *videoCb = nullptr;
    AudioCallback *audioCb = nullptr;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void **ppv) override {
        if (!ppv)
            return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_ISampleGrabberCB) {
            *ppv = static_cast<ISampleGrabberCB *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(
            InterlockedIncrement(reinterpret_cast<LONG *>(&refCount)));
    }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(reinterpret_cast<LONG *>(&refCount));
        if (r == 0)
            delete this;
        return static_cast<ULONG>(r);
    }

    STDMETHODIMP SampleCB(double /*sampleTime*/,
                          IMediaSample *sample) override {
        if (!sample || !haveFormat)
            return S_OK;

        BYTE *buffer = nullptr;
        sample->GetPointer(&buffer);
        LONG bufferLen = sample->GetActualDataLength();
        if (!buffer || bufferLen <= 0)
            return S_OK;

        REFERENCE_TIME tStart = 0;
        REFERENCE_TIME tEnd = 0;
        LONGLONG ts;
        if (SUCCEEDED(sample->GetTime(&tStart, &tEnd))) {
            ts = tStart;
        } else {
            ts = fallbackTime;
            if (video)
                fallbackTime += averageFrameTime;
            else if (samplesPerSec > 0 && channels > 0 && bitsPerSample > 0)
                fallbackTime += static_cast<LONGLONG>(
                    (static_cast<double>(bufferLen) * 10000000.0 /
                     (samplesPerSec * channels * (bitsPerSample / 8))));
        }

        LONGLONG first = firstTime.load();
        if (first < 0) {
            LONGLONG expected = -1;
            if (firstTime.compare_exchange_strong(expected, ts))
                first = ts;
            else if (expected >= 0)
                first = expected;
        }
        int64_t timestampNs = static_cast<int64_t>(ts - first) * 100;
        if (timestampNs < 0)
            timestampNs = 0;

        if (video) {
            if (videoCb && *videoCb) {
                DvVideoFrame frame;
                frame.width = width;
                frame.height = height;
                frame.topDown = topDown;
                frame.data = buffer;
                frame.size = static_cast<size_t>(bufferLen);
                frame.timestampNs = timestampNs;
                (*videoCb)(frame);
            }
        } else {
            if (audioCb && *audioCb) {
                DvAudioFrame frame;
                frame.channels = channels;
                frame.samplesPerSec = samplesPerSec;
                frame.bitsPerSample = bitsPerSample;
                frame.data = buffer;
                frame.size = static_cast<size_t>(bufferLen);
                frame.timestampNs = timestampNs;
                (*audioCb)(frame);
            }
        }
        return S_OK;
    }

    STDMETHODIMP BufferCB(double /*sampleTime*/, BYTE * /*pBuffer*/,
                          long /*bufferLen*/) override {
        return E_NOTIMPL;
    }
};

// ---------------------------------------------------------------------------
// DshowCapture
// ---------------------------------------------------------------------------

DshowCapture::DshowCapture() = default;

DshowCapture::~DshowCapture() {
    Stop();
}

void DshowCapture::SetError(const char *msg, HRESULT hr) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s (HRESULT 0x%08lX)", msg,
             static_cast<unsigned long>(hr));
    lastError_ = buf;
    Log(lastError_);
}

void DshowCapture::Log(const std::string &msg) {
    if (logCb_)
        logCb_(msg);
}

bool DshowCapture::Start(const std::string &monikerPathUtf8,
                         bool captureAudio,
                         VideoCallback videoCb,
                         AudioCallback audioCb,
                         LogCallback logCb) {
    if (running_.load())
        return false;
    lastError_.clear();
    userVideoCb_ = std::move(videoCb);
    userAudioCb_ = std::move(audioCb);
    logCb_ = std::move(logCb);
    stopRequested_ = false;
    thread_ = std::thread(&DshowCapture::CaptureThread, this,
                          Utf8ToWide(monikerPathUtf8), captureAudio);
    // Wait for the graph to come up so the caller can react to immediate
    // failures. A DV device can take several seconds to initialise.
    for (int i = 0; i < 300 && !running_.load() && lastError_.empty(); ++i)
        Sleep(100);
    const bool started = running_.load();
    if (!started && !lastError_.empty()) {
        // Definite failure: reap the capture thread so the object can be
        // safely destroyed/restarted.
        if (thread_.joinable())
            thread_.join();
    }
    return started;
}

void DshowCapture::Stop() {
    stopRequested_ = true;
    if (thread_.joinable())
        thread_.join();
}

static IBaseFilter *BindMonikerDevice(IEnumMoniker *enumMoniker,
                                      const std::wstring &targetPath,
                                      IMoniker **outMoniker) {
    IMoniker *moniker = nullptr;
    while (enumMoniker->Next(1, &moniker, nullptr) == S_OK) {
        LPOLESTR displayName = nullptr;
        bool match = false;
        if (SUCCEEDED(moniker->GetDisplayName(nullptr, nullptr,
                                              &displayName))) {
            if (displayName) {
                match = (targetPath == displayName);
                CoTaskMemFree(displayName);
            }
        }
        if (match) {
            IBaseFilter *filter = nullptr;
            if (SUCCEEDED(moniker->BindToObject(
                    nullptr, nullptr, PPV(IBaseFilter, &filter)))) {
                if (outMoniker)
                    *outMoniker = moniker;
                else
                    moniker->Release();
                return filter;
            }
        }
        moniker->Release();
    }
    return nullptr;
}

void DshowCapture::Teardown() {
    if (control_)
        control_->Stop();

    if (videoGrabber_)
        videoGrabber_->SetCallback(nullptr, 0);
    if (audioGrabber_)
        audioGrabber_->SetCallback(nullptr, 0);

    SafeRelease(videoCallback_);
    SafeRelease(audioCallback_);
    SafeRelease(videoGrabber_);
    SafeRelease(audioGrabber_);
    SafeRelease(videoGrabberFilter_);
    SafeRelease(videoNullRenderer_);
    SafeRelease(audioGrabberFilter_);
    SafeRelease(audioNullRenderer_);
    SafeRelease(dvVideoDecoder_);
    SafeRelease(dvSplitter_);
    SafeRelease(sourceFilter_);
    SafeRelease(mediaEvent_);
    SafeRelease(control_);
    SafeRelease(captureGraph_);
    SafeRelease(graph_);
}

void DshowCapture::CaptureThread(std::wstring monikerPath,
                                 bool captureAudio) {
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        SetError("COM initialization failed", comHr);
        running_ = false;
        return;
    }
    const bool comInited = SUCCEEDED(comHr);

    HANDLE stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    ICreateDevEnum *devEnum = nullptr;
    IEnumMoniker *enumMoniker = nullptr;
    IMoniker *sourceMoniker = nullptr;

    auto fail = [&](const char *msg, HRESULT code) {
        SetError(msg, code);
        running_ = false;
        SetEvent(stopEvent);
    };
    auto logResult = [&](const char *step, HRESULT result) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s (HRESULT 0x%08lX)", step,
                 static_cast<unsigned long>(result));
        Log(buf);
    };

    HRESULT hr;
    do {
        hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                              CLSCTX_INPROC_SERVER,
                              PPV(ICreateDevEnum, &devEnum));
        if (FAILED(hr)) {
            fail("Cannot create system device enumerator", hr);
            break;
        }
        hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                            &enumMoniker, 0);
        if (FAILED(hr) || !enumMoniker) {
            fail("No DirectShow video input devices found",
                 hr ? hr : E_FAIL);
            break;
        }

        sourceFilter_ =
            BindMonikerDevice(enumMoniker, monikerPath, &sourceMoniker);
        if (!sourceFilter_) {
            fail("Selected DV device not found (unplugged or in use?)",
                 E_FAIL);
            break;
        }

        hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                              PPV(IGraphBuilder, &graph_));
        if (FAILED(hr)) {
            fail("Cannot create DirectShow filter graph", hr);
            break;
        }
        hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr,
                              CLSCTX_INPROC_SERVER,
                              PPV(ICaptureGraphBuilder2,
                                          &captureGraph_));
        if (FAILED(hr)) {
            fail("Cannot create capture graph builder", hr);
            break;
        }
        captureGraph_->SetFiltergraph(graph_);

        hr = graph_->AddFilter(sourceFilter_, L"DV Camera Source");
        if (FAILED(hr)) {
            fail("Cannot add DV source filter to graph", hr);
            break;
        }

        // ----- Canonical DV graph: source -> DV Splitter -> decoder -----
        // This device can connect its source directly to the video decoder,
        // so a video-only graph contains no splitter. Create both filters
        // explicitly to expose the embedded audio as well as decoded video.
        hr = CoCreateInstance(CLSID_DVSplitter, nullptr,
                              CLSCTX_INPROC_SERVER,
                              PPV(IBaseFilter, &dvSplitter_));
        if (FAILED(hr)) {
            fail("Cannot create DV Splitter", hr);
            break;
        }
        hr = graph_->AddFilter(dvSplitter_, L"DV Splitter");
        if (FAILED(hr)) {
            fail("Cannot add DV Splitter to graph", hr);
            break;
        }
        hr = CoCreateInstance(CLSID_DVVideoCodec, nullptr,
                              CLSCTX_INPROC_SERVER,
                              PPV(IBaseFilter, &dvVideoDecoder_));
        if (FAILED(hr)) {
            fail("Cannot create DV Video Decoder", hr);
            break;
        }
        hr = graph_->AddFilter(dvVideoDecoder_, L"DV Video Decoder");
        if (FAILED(hr)) {
            fail("Cannot add DV Video Decoder to graph", hr);
            break;
        }

        // Microsoft DV graph order: source, no intermediate, splitter sink.
        // Passing splitter as the intermediate with a null sink only performs
        // a partial render and was the cause of the phase1rc0 regression.
        hr = captureGraph_->RenderStream(
            nullptr, &MEDIATYPE_Interleaved, sourceFilter_, nullptr,
            dvSplitter_);
        logResult("DV source -> DV Splitter RenderStream", hr);
        if (FAILED(hr)) {
            fail("Cannot connect DV source to DV Splitter", hr);
            break;
        }

        // ----- Video branch: DV Splitter -> decoder -> SampleGrabber -----
        hr = CoCreateInstance(CLSID_SampleGrabber, nullptr,
                              CLSCTX_INPROC_SERVER,
                              PPV(ISampleGrabber, &videoGrabber_));
        if (FAILED(hr)) {
            fail("Cannot create video SampleGrabber", hr);
            break;
        }
        AM_MEDIA_TYPE videoMt;
        ZeroMemory(&videoMt, sizeof(videoMt));
        videoMt.majortype = MEDIATYPE_Video;
        videoMt.subtype = MEDIASUBTYPE_YUY2;
        videoMt.formattype = FORMAT_VideoInfo;
        videoGrabber_->SetMediaType(&videoMt);
        videoGrabber_->SetBufferSamples(FALSE);

        videoCallback_ = new CallbackImpl();
        videoCallback_->video = true;
        videoCallback_->videoCb = &userVideoCb_;
        videoGrabber_->SetCallback(videoCallback_, 0);

        hr = videoGrabber_->QueryInterface(
            PPV(IBaseFilter, &videoGrabberFilter_));
        if (FAILED(hr)) {
            fail("Cannot query video grabber filter interface", hr);
            break;
        }
        graph_->AddFilter(videoGrabberFilter_, L"DV Video Grabber");

        hr = CoCreateInstance(CLSID_NullRenderer, nullptr,
                              CLSCTX_INPROC_SERVER,
                              PPV(IBaseFilter, &videoNullRenderer_));
        if (FAILED(hr)) {
            fail("Cannot create video renderer", hr);
            break;
        }
        graph_->AddFilter(videoNullRenderer_, L"DV Video Null");

        hr = captureGraph_->RenderStream(nullptr, nullptr, dvSplitter_,
                                         dvVideoDecoder_,
                                         videoGrabberFilter_);
        logResult("DV Splitter -> decoder -> video grabber RenderStream", hr);
        if (FAILED(hr)) {
            fail("Cannot build DV decoder and video grabber graph", hr);
            break;
        }
        hr = captureGraph_->RenderStream(nullptr, nullptr,
                                         videoGrabberFilter_, nullptr,
                                         videoNullRenderer_);
        logResult("video grabber -> NullRenderer RenderStream", hr);
        if (FAILED(hr)) {
            fail("Cannot connect DV video grabber to renderer", hr);
            break;
        }

        AM_MEDIA_TYPE connectedVideo;
        ZeroMemory(&connectedVideo, sizeof(connectedVideo));
        if (SUCCEEDED(videoGrabber_->GetConnectedMediaType(&connectedVideo))) {
            if (connectedVideo.formattype == FORMAT_VideoInfo &&
                connectedVideo.cbFormat >= sizeof(VIDEOINFOHEADER)) {
                VIDEOINFOHEADER *vih =
                    reinterpret_cast<VIDEOINFOHEADER *>(connectedVideo.pbFormat);
                // MAKEFOURCC('Y','U','Y','2') == 0x32595559.
                const DWORD kFourccYUY2 = 0x32595559u;
                if (vih->bmiHeader.biCompression == kFourccYUY2) {
                    videoCallback_->width = vih->bmiHeader.biWidth;
                    videoCallback_->height =
                        std::abs(vih->bmiHeader.biHeight);
                    // The real MSDV/qdvd DV path observed on the target
                    // camcorder delivers rows opposite to the usual DIB sign
                    // convention. Positive biHeight is top-down here; using
                    // the generic DIB rule produced an upside-down image.
                    videoCallback_->topDown = vih->bmiHeader.biHeight > 0;
                    videoCallback_->averageFrameTime =
                        vih->AvgTimePerFrame > 0
                            ? vih->AvgTimePerFrame
                            : static_cast<REFERENCE_TIME>(400000);
                    videoCallback_->haveFormat = true;
                    char formatLog[192];
                    snprintf(formatLog, sizeof(formatLog),
                             "video: connected %ldx%ld YUY2, biHeight=%ld, %s",
                             static_cast<long>(vih->bmiHeader.biWidth),
                             static_cast<long>(vih->bmiHeader.biHeight),
                             static_cast<long>(vih->bmiHeader.biHeight),
                             videoCallback_->topDown ? "top-down" :
                                                       "bottom-up");
                    Log(formatLog);
                }
            }
            if (connectedVideo.pbFormat)
                CoTaskMemFree(connectedVideo.pbFormat);
            if (connectedVideo.pUnk)
                connectedVideo.pUnk->Release();
        }
        if (!videoCallback_->haveFormat) {
            fail("DV video decoder produced an unknown format", E_FAIL);
            break;
        }

        // ----- Audio branch (optional): DV Splitter -> SampleGrabber -----
        bool audioOk = false;
        if (captureAudio && userAudioCb_) {
            do {
                hr = CoCreateInstance(CLSID_SampleGrabber, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      PPV(ISampleGrabber, &audioGrabber_));
                if (FAILED(hr)) {
                    logResult("audio: cannot create SampleGrabber", hr);
                    break;
                }

                AM_MEDIA_TYPE audioMt;
                ZeroMemory(&audioMt, sizeof(audioMt));
                audioMt.majortype = MEDIATYPE_Audio;
                audioMt.formattype = FORMAT_WaveFormatEx;
                audioGrabber_->SetMediaType(&audioMt);
                audioGrabber_->SetBufferSamples(FALSE);

                audioCallback_ = new CallbackImpl();
                audioCallback_->video = false;
                audioCallback_->audioCb = &userAudioCb_;
                audioGrabber_->SetCallback(audioCallback_, 0);

                hr = audioGrabber_->QueryInterface(
                    PPV(IBaseFilter, &audioGrabberFilter_));
                if (FAILED(hr)) {
                    logResult("audio: cannot query grabber filter", hr);
                    break;
                }
                hr = graph_->AddFilter(audioGrabberFilter_,
                                       L"DV Audio Grabber");
                if (FAILED(hr)) {
                    logResult("audio: cannot add SampleGrabber", hr);
                    break;
                }

                hr = CoCreateInstance(CLSID_NullRenderer, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      PPV(IBaseFilter,
                                                  &audioNullRenderer_));
                if (FAILED(hr)) {
                    logResult("audio: cannot create NullRenderer", hr);
                    break;
                }
                hr = graph_->AddFilter(audioNullRenderer_, L"DV Audio Null");
                if (FAILED(hr)) {
                    logResult("audio: cannot add NullRenderer", hr);
                    break;
                }

                // Do not request audio from sourceFilter_: MSDV only exposes
                // interleaved DV. The splitter owns the actual audio output.
                hr = captureGraph_->RenderStream(
                    nullptr, &MEDIATYPE_Audio, dvSplitter_,
                    audioGrabberFilter_, audioNullRenderer_);
                logResult("DV Splitter -> audio RenderStream", hr);
                if (FAILED(hr))
                    break;

                AM_MEDIA_TYPE connectedAudio;
                ZeroMemory(&connectedAudio, sizeof(connectedAudio));
                hr = audioGrabber_->GetConnectedMediaType(&connectedAudio);
                if (FAILED(hr)) {
                    logResult("audio: GetConnectedMediaType", hr);
                    break;
                }
                if (connectedAudio.formattype == FORMAT_WaveFormatEx &&
                    connectedAudio.cbFormat >= sizeof(WAVEFORMATEX)) {
                    WAVEFORMATEX *wf =
                        reinterpret_cast<WAVEFORMATEX *>(
                            connectedAudio.pbFormat);
                    // DV embedded audio is 16-bit PCM on this capture path.
                    if (wf->wBitsPerSample == 16 &&
                        (wf->nChannels == 1 || wf->nChannels == 2) &&
                        wf->nSamplesPerSec > 0) {
                        audioCallback_->channels =
                            static_cast<int>(wf->nChannels);
                        audioCallback_->samplesPerSec =
                            static_cast<int>(wf->nSamplesPerSec);
                        audioCallback_->bitsPerSample =
                            static_cast<int>(wf->wBitsPerSample);
                        audioCallback_->haveFormat = true;
                        audioOk = true;

                        char formatLog[160];
                        snprintf(formatLog, sizeof(formatLog),
                                 "audio: connected %lu Hz, %u channels, "
                                 "%u-bit PCM",
                                 static_cast<unsigned long>(
                                     wf->nSamplesPerSec),
                                 static_cast<unsigned>(wf->nChannels),
                                 static_cast<unsigned>(wf->wBitsPerSample));
                        Log(formatLog);
                    } else {
                        Log("audio: unsupported format (need mono/stereo "
                            "16-bit PCM)");
                    }
                } else {
                    Log("audio: connected format is not WAVEFORMATEX");
                }
                if (connectedAudio.pbFormat)
                    CoTaskMemFree(connectedAudio.pbFormat);
                if (connectedAudio.pUnk)
                    connectedAudio.pUnk->Release();
            } while (false);

            // Audio is best-effort, but never fail silently: the exact graph
            // error above is visible in the OBS log while video continues.
            if (!audioOk) {
                Log("audio: embedded DV audio capture unavailable");
                if (audioGrabber_)
                    audioGrabber_->SetCallback(nullptr, 0);
                if (audioGrabberFilter_)
                    graph_->RemoveFilter(audioGrabberFilter_);
                if (audioNullRenderer_)
                    graph_->RemoveFilter(audioNullRenderer_);
                SafeRelease(audioCallback_);
                SafeRelease(audioGrabber_);
                SafeRelease(audioGrabberFilter_);
                SafeRelease(audioNullRenderer_);
            }
        }

        hr = graph_->QueryInterface(PPV(IMediaControl, &control_));
        if (FAILED(hr)) {
            fail("Cannot query media control interface", hr);
            break;
        }
        hr = graph_->QueryInterface(PPV(IMediaEventEx, &mediaEvent_));
        if (FAILED(hr)) {
            fail("Cannot query media event interface", hr);
            break;
        }

        hr = control_->Run();
        if (FAILED(hr)) {
            fail("Failed to start the DirectShow graph", hr);
            break;
        }

        running_ = true;
        ResetEvent(stopEvent);

        // Pump graph events until stop is requested or the device vanishes.
        while (!stopRequested_) {
            long eventCode = 0;
            LONG_PTR p1 = 0, p2 = 0;
            while (mediaEvent_ &&
                   mediaEvent_->GetEvent(&eventCode, &p1, &p2, 0) == S_OK) {
                mediaEvent_->FreeEventParams(eventCode, p1, p2);
                if (eventCode == EC_DEVICE_LOST ||
                    eventCode == EC_ERRORABORT ||
                    eventCode == EC_ERRORABORTEX) {
                    SetError("DV device lost or stream aborted",
                             static_cast<HRESULT>(p2));
                    SetEvent(stopEvent);
                    stopRequested_ = true;
                }
            }
            WaitForSingleObject(stopEvent, 100);
        }
    } while (false);

    Teardown();

    SafeRelease(sourceMoniker);
    SafeRelease(enumMoniker);
    SafeRelease(devEnum);
    if (stopEvent)
        CloseHandle(stopEvent);
    if (comInited)
        CoUninitialize();
    running_ = false;
}

}  // namespace dvcap

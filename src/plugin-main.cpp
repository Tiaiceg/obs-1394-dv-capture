/*
 * plugin-main.cpp - OBS source wrapper around the DirectShow DV capture
 * engine. Registers an async video + audio source named
 * "DV/1394 FireWire Capture".
 */

#include <obs-module.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "dshow-capture.h"

#define PLUGIN_NAME "obs-1394-dv-capture"
#define SETTING_DEVICE "device_id"
#define SETTING_AUDIO "capture_audio"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

// Named Tr() instead of TEXT() to avoid clashing with the Windows TEXT()/T()
// macro from winnt.h.
const char *Tr(const char *key) {
    return obs_module_text(key);
}

struct DvSource {
    obs_source_t *source = nullptr;
    std::unique_ptr<dvcap::DshowCapture> capture;
    std::string devicePath;
    bool captureAudio = true;
    bool active = false;
    bool loggedFormat = false;

    // Ring of owned buffers so a frame handed to libobs stays valid until
    // it has been consumed. DirectShow callbacks and the OBS async video
    // queue run on different threads.
    std::vector<std::vector<uint8_t>> videoSlots{4};
    std::vector<std::vector<uint8_t>> audioSlots{6};
    std::atomic<unsigned> videoIndex{0};
    std::atomic<unsigned> audioIndex{0};

    explicit DvSource(obs_source_t *src) : source(src) {}

    void OnVideo(const dvcap::DvVideoFrame &frame) {
        const int rowBytes = frame.width * 2;  // YUY2: 4 bytes / 2 pixels
        if (rowBytes <= 0 || frame.height <= 0)
            return;
        const size_t planeSize =
            static_cast<size_t>(rowBytes) * static_cast<size_t>(frame.height);
        if (frame.size < planeSize)
            return;  // Truncated / non-YUY2 sample, ignore it.

        const unsigned slotIndex =
            videoIndex.fetch_add(1, std::memory_order_relaxed) %
            static_cast<unsigned>(videoSlots.size());
        auto &slot = videoSlots[slotIndex];
        if (slot.size() < planeSize)
            slot.resize(planeSize);

        if (frame.topDown) {
            memcpy(slot.data(), frame.data, planeSize);
        } else {
            // The DV decoder hands back bottom-up DIB rows; OBS expects
            // top-down, so flip while copying.
            for (int y = 0; y < frame.height; ++y) {
                const uint8_t *srcRow =
                    frame.data + (frame.height - 1 - y) * rowBytes;
                uint8_t *dstRow = slot.data() + y * rowBytes;
                memcpy(dstRow, srcRow, rowBytes);
            }
        }

        obs_source_frame out{};
        out.format = VIDEO_FORMAT_YUY2;
        out.width = static_cast<uint32_t>(frame.width);
        out.height = static_cast<uint32_t>(frame.height);
        out.linesize[0] = static_cast<uint32_t>(rowBytes);
        out.data[0] = slot.data();
        out.timestamp = frame.timestampNs;
        // DV standard definition is BT.601, limited range.
        out.full_range = false;
        video_format_get_parameters(VIDEO_CS_601, VIDEO_RANGE_PARTIAL,
                                    out.color_matrix, out.color_range_min,
                                    out.color_range_max);
        obs_source_output_video(source, &out);

        if (!loggedFormat) {
            loggedFormat = true;
            blog(LOG_INFO,
                 "[obs-1394-dv-capture] video: %dx%d YUY2 (BT.601, limited range)",
                 frame.width, frame.height);
        }
    }

    void OnAudio(const dvcap::DvAudioFrame &frame) {
        // Only 16-bit PCM is supported by the DV graph path; anything else
        // is skipped.
        if (frame.bitsPerSample != 16 || frame.channels < 1 ||
            frame.samplesPerSec <= 0)
            return;

        const unsigned slotIndex =
            audioIndex.fetch_add(1, std::memory_order_relaxed) %
            static_cast<unsigned>(audioSlots.size());
        auto &slot = audioSlots[slotIndex];
        if (slot.size() < frame.size)
            slot.resize(frame.size);
        memcpy(slot.data(), frame.data, frame.size);

        obs_source_audio out{};
        out.format = AUDIO_FORMAT_16BIT;
        out.samples_per_sec = static_cast<uint32_t>(frame.samplesPerSec);
        out.speakers = frame.channels >= 2 ? SPEAKERS_STEREO
                                          : SPEAKERS_MONO;
        out.data[0] = slot.data();
        out.frames = static_cast<uint32_t>(
            frame.size /
            (static_cast<size_t>(frame.bitsPerSample / 8) *
             static_cast<size_t>(frame.channels)));
        out.timestamp = frame.timestampNs;
        obs_source_output_audio(source, &out);
    }

    void StartCapture() {
        if (devicePath.empty()) {
            blog(LOG_WARNING,
                 "[obs-1394-dv-capture] cannot start: no DV device selected");
            return;
        }
        StopCapture();
        capture = std::make_unique<dvcap::DshowCapture>();

        DvSource *self = this;
        auto videoCb = [self](const dvcap::DvVideoFrame &f) {
            self->OnVideo(f);
        };
        dvcap::AudioCallback audioCb;
        if (captureAudio) {
            audioCb = [self](const dvcap::DvAudioFrame &f) {
                self->OnAudio(f);
            };
        }

        auto logCb = [](const std::string &message) {
            blog(LOG_INFO, "[" PLUGIN_NAME "] %s", message.c_str());
        };
        bool ok = capture->Start(devicePath, captureAudio, videoCb,
                                 audioCb, logCb);
        if (ok) {
            blog(LOG_INFO,
                 "[obs-1394-dv-capture] capture started for '%s' (audio=%s)",
                 devicePath.c_str(), captureAudio ? "on" : "off");
        } else {
            blog(LOG_WARNING,
                 "[obs-1394-dv-capture] failed to start DV capture on '%s': %s",
                 devicePath.c_str(),
                 capture ? capture->lastError().c_str() : "unknown error");
            capture.reset();
        }
    }

    void StopCapture() {
        if (capture) {
            capture->Stop();
            capture.reset();
        }
    }

    // OBS calls Activate when the source is shown (preview/program) and
    // Deactivate when it is no longer visible. The 1394 DV device is
    // exclusive, so release it while the source is inactive.
    void Activate() {
        active = true;
        StartCapture();
    }

    void Deactivate() {
        active = false;
        StopCapture();
    }
};

// ---------------------------------------------------------------------------
// OBS source callbacks
// ---------------------------------------------------------------------------

static const char *GetName(void *) {
    return Tr("DV1394Capture");
}

static void *Create(obs_data_t *settings, obs_source_t *source) {
    auto *s = new DvSource(source);

    s->devicePath = obs_data_get_string(settings, SETTING_DEVICE);
    s->captureAudio = obs_data_get_bool(settings, SETTING_AUDIO);

    // Fall back to the first available device if the stored setting is empty
    // (e.g. the source was created before the camcorder was plugged in).
    if (s->devicePath.empty()) {
        auto devices = dvcap::EnumerateDevices();
        if (!devices.empty())
            s->devicePath = devices.front().monikerPath;
    }

    // The actual capture is started in Activate().
    return s;
}

static void Destroy(void *data) {
    auto *s = static_cast<DvSource *>(data);
    s->StopCapture();
    delete s;
}

static void Activate(void *data) {
    static_cast<DvSource *>(data)->Activate();
}

static void Deactivate(void *data) {
    static_cast<DvSource *>(data)->Deactivate();
}

static void Update(void *data, obs_data_t *settings) {
    auto *s = static_cast<DvSource *>(data);

    std::string device = obs_data_get_string(settings, SETTING_DEVICE);
    bool wantAudio = obs_data_get_bool(settings, SETTING_AUDIO);

    if (device != s->devicePath || wantAudio != s->captureAudio) {
        s->devicePath = device;
        s->captureAudio = wantAudio;
        // Restart only while the source is actually visible; Activate will
        // pick up the new settings otherwise.
        if (s->active)
            s->StartCapture();
    }
}

static void FillDeviceList(obs_property_t *prop) {
    obs_property_list_clear(prop);
    for (const auto &dev : dvcap::EnumerateDevices()) {
        obs_property_list_add_string(prop, dev.friendlyName.c_str(),
                                     dev.monikerPath.c_str());
    }
}

static bool RefreshDevices(obs_properties_t *, obs_property_t *prop, void *) {
    FillDeviceList(prop);
    return true;
}

static obs_properties_t *GetProperties(void *) {
    obs_properties_t *props = obs_properties_create();

    obs_property_t *device = obs_properties_add_list(
        props, SETTING_DEVICE, Tr("Device"), OBS_COMBO_TYPE_LIST,
        OBS_COMBO_FORMAT_STRING);
    FillDeviceList(device);

    obs_properties_add_button(props, "refresh", Tr("RefreshDevices"),
                              RefreshDevices);

    obs_properties_add_bool(props, SETTING_AUDIO, Tr("CaptureAudio"));

    obs_properties_t *advanced = obs_properties_create();
    obs_properties_add_text(advanced, "hint",
                            Tr("Hint"), OBS_TEXT_INFO);
    obs_properties_add_group(props, "group_advanced", Tr("Advanced"),
                             OBS_GROUP_NORMAL, advanced);

    return props;
}

static void GetDefaults(obs_data_t *defaults) {
    obs_data_set_default_bool(defaults, SETTING_AUDIO, true);
    obs_data_set_default_string(defaults, "hint", Tr("HintText"));

    auto devices = dvcap::EnumerateDevices();
    if (!devices.empty()) {
        obs_data_set_default_string(defaults, SETTING_DEVICE,
                                    devices.front().monikerPath.c_str());
    }
}

static obs_source_info dv_source_info = [] {
    obs_source_info info{};
    info.id = "dv1394_capture";
    info.type = OBS_SOURCE_TYPE_INPUT;
    // Frames are pushed asynchronously via obs_source_output_video(), so
    // this is an ASYNC_VIDEO source (no get_width/get_height/video_render
    // callbacks are required), matching OBS's built-in video capture source.
    info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
                       OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name = GetName;
    info.create = Create;
    info.destroy = Destroy;
    info.activate = Activate;
    info.deactivate = Deactivate;
    info.update = Update;
    info.get_properties = GetProperties;
    info.get_defaults = GetDefaults;
    info.icon_type = OBS_ICON_TYPE_CAMERA;
    return info;
}();

}  // namespace

bool obs_module_load() {
    blog(LOG_INFO, "[obs-1394-dv-capture] loaded (IEEE-1394 DV capture source)");
    obs_register_source(&dv_source_info);
    return true;
}

void obs_module_unload() {}

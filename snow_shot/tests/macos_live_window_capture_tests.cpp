// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/presentation/pinned/livewindownative_p.h"
#include "snow_capture_macos.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

struct SnowMacCancellation {
    bool canceled = false;
};
struct SnowMacStream {};
struct SnowMacFrame {
    std::atomic<int> references{1};
    std::array<uint8_t, 8> pixels{64, 32, 16, 128, 64, 32, 16, 128};
};
namespace {
SnowMacStatus nextStatus = SNOW_MAC_OK;
uint32_t eventKind = 2;
int creates = 0, destroys = 0, cancellations = 0, readbacks = 0, frames = 0;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace
extern "C" {
SnowMacCancellation* snow_capture_macos_cancellation_create() {
    return new SnowMacCancellation;
}
void snow_capture_macos_cancellation_cancel(const SnowMacCancellation*) {
    ++cancellations;
}
void snow_capture_macos_cancellation_release(SnowMacCancellation* token) {
    delete token;
}
SnowMacStatus snow_capture_macos_live_window_stream_create(const SnowMacCaptureConfig* config,
                                                           SnowMacStream** output) {
    require(config->struct_size == sizeof(*config) && config->target_kind == 2 &&
                config->target_id == 42 && config->fps == 30 && !config->cursor_mode &&
                !config->dynamic_range && !config->output_width && !config->output_height &&
                config->cancellation,
            "The native adapter requests persistent window capture at native SDR resolution.");
    ++creates;
    *output = new SnowMacStream;
    return SNOW_MAC_OK;
}
SnowMacStatus snow_capture_macos_stream_next(SnowMacStream*, uint32_t timeout,
                                             SnowMacCaptureEvent* event) {
    require(timeout == 50, "Capture polling remains bounded.");
    if (nextStatus != SNOW_MAC_OK)
        return nextStatus;
    event->kind = eventKind;
    if (eventKind == 2) {
        event->frame = new SnowMacFrame;
        ++frames;
    }
    return SNOW_MAC_OK;
}
void snow_capture_macos_stream_destroy(SnowMacStream* stream) {
    if (stream)
        ++destroys;
    delete stream;
}
SnowMacStatus snow_capture_macos_frame_info(const SnowMacFrame*, SnowMacFrameInfo* info) {
    *info = {};
    info->width = 2;
    info->height = 1;
    info->alpha_mode = 2;
    info->desktop_x = 100;
    info->desktop_y = 200;
    info->desktop_width = 1;
    info->desktop_height = 0.5;
    return SNOW_MAC_OK;
}
const void* snow_capture_macos_frame_pixel_buffer(const SnowMacFrame* frame) {
    return frame;
}
SnowMacFrame* snow_capture_macos_frame_retain(const SnowMacFrame* frame) {
    auto* retained = const_cast<SnowMacFrame*>(frame);
    ++retained->references;
    return retained;
}
void snow_capture_macos_frame_release(SnowMacFrame* frame) {
    if (frame && --frame->references == 0) {
        --frames;
        delete frame;
    }
}
SnowMacStatus snow_capture_macos_frame_map_format(const SnowMacFrame* frame, uint32_t format,
                                                  uint32_t plane, SnowMacPlane* output) {
    require(format == 0 && plane == 0, "Image actions explicitly request RGBA pixels.");
    ++readbacks;
    *output = {frame->pixels.data(), frame->pixels.size(), 2, 1, 8};
    return SNOW_MAC_OK;
}
}
int main() {
    using namespace snow_shot::presentation;
    using namespace snow_shot::presentation::live_window_detail;
    try {
        auto capture = createNativeWindowCapture({42, 7, QRect(100, 200, 1, 1), {}});
        NativeCaptureFrame frame;
        for (const auto status : {SNOW_MAC_TIMEOUT, SNOW_MAC_INTERRUPTED, SNOW_MAC_INTERRUPTED}) {
            nextStatus = status;
            require(capture->receive(&frame) && !frame.pixels,
                    "Timeout and transient inactive samples keep the native stream alive.");
        }
        nextStatus = SNOW_MAC_OK;
        eventKind = 1;
        require(capture->receive(&frame) && !frame.pixels,
                "Configuration events do not masquerade as frames.");
        eventKind = 2;
        require(capture->receive(&frame) && frame.pixels && creates == 1 && destroys == 0,
                "The same native stream delivers a fresh frame after an interruption.");
        require(frame.desktopBounds == QRectF(100, 200, 1, 0.5) &&
                    frame.pixels->size() == QSize(2, 1) && frame.pixels->pixelBuffer() &&
                    readbacks == 0,
                "Receiving native geometry and GPU leases does not read CPU pixels.");
        const QImage first = frame.pixels->image();
        const QImage second = frame.pixels->image();
        require(!first.isNull() && second == first && readbacks == 1 &&
                    first.pixelColor(0, 0) == QColor(128, 64, 32, 128),
                "Image actions share one readback and convert premultiplied pixels correctly.");
        nextStatus = SNOW_MAC_TARGET_UNAVAILABLE;
        NativeCaptureFrame missing;
        require(!capture->receive(&missing), "A missing source remains a real capture failure.");
        capture->cancel();
        capture.reset();
        require(cancellations == 1 && destroys == 1 && frames == 1,
                "Stopping the stream preserves outstanding native frame leases.");
        frame = {};
        require(frames == 0, "The final native lease releases the underlying buffer.");
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../presentation/pinned/livewindownative_p.h"
#include "snow_capture_macos.h"

#include <QColorSpace>
#include <limits>
#include <mutex>

namespace snow_shot::presentation::live_window_detail {
namespace {
class MacWindowFrame final : public NativeWindowFrame {
  public:
    MacWindowFrame(SnowMacFrame* frame, SnowMacFrameInfo info) : m_frame(frame), m_info(info) {}
    ~MacWindowFrame() override {
        snow_capture_macos_frame_release(m_frame);
    }
    QSize size() const override {
        return QSize(int(m_info.width), int(m_info.height));
    }
    const void* pixelBuffer() const override {
        return snow_capture_macos_frame_pixel_buffer(m_frame);
    }
    QImage image() const override {
        std::call_once(m_imageOnce, [this] { m_image = readImage(); });
        return m_image;
    }

  private:
    QImage readImage() const {
        SnowMacPlane plane{};
        if (snow_capture_macos_frame_map_format(m_frame, 0, 0, &plane) != SNOW_MAC_OK ||
            !plane.data || plane.stride > size_t(std::numeric_limits<int>::max()) ||
            plane.len < plane.stride * plane.height)
            return {};
        auto* retained = snow_capture_macos_frame_retain(m_frame);
        QImage image(
            plane.data, int(m_info.width), int(m_info.height), qsizetype(plane.stride),
            m_info.alpha_mode == 2 ? QImage::Format_RGBA8888_Premultiplied
                                   : QImage::Format_RGBA8888,
            [](void* value) {
                snow_capture_macos_frame_release(static_cast<SnowMacFrame*>(value));
            },
            retained);
        image.setColorSpace(QColorSpace::SRgb);
        return image.convertToFormat(QImage::Format_RGBA8888);
    }

    SnowMacFrame* m_frame;
    SnowMacFrameInfo m_info;
    mutable std::once_flag m_imageOnce;
    mutable QImage m_image;
};

class MacWindowCapture final : public NativeWindowCapture {
  public:
    explicit MacWindowCapture(const LiveWindowTarget& target) : m_target(target) {}
    ~MacWindowCapture() override {
        snow_capture_macos_stream_destroy(m_stream);
        snow_capture_macos_cancellation_release(m_cancellation);
    }
    void cancel() override {
        snow_capture_macos_cancellation_cancel(m_cancellation);
    }
    bool receive(NativeCaptureFrame* output) override {
        if (!m_stream) {
            SnowMacCaptureConfig config{};
            config.struct_size = sizeof(config);
            config.cancellation = m_cancellation;
            config.target_kind = 2;
            config.target_id = uint32_t(m_target.nativeId);
            config.fps = 30;
            config.timeout_ms = 5000;
            if (snow_capture_macos_live_window_stream_create(&config, &m_stream) != SNOW_MAC_OK)
                return false;
        }
        SnowMacCaptureEvent event{};
        const auto status = snow_capture_macos_stream_next(m_stream, 50, &event);
        // Blank/suspended samples invalidate stale capture observations but do
        // not end the stream. Match the existing macOS CPU capture adapter;
        // the source monitor and request deadline govern unavailability.
        if (status == SNOW_MAC_TIMEOUT || status == SNOW_MAC_INTERRUPTED)
            return true;
        if (status != SNOW_MAC_OK)
            return false;
        if (event.kind != 2)
            return true;
        SnowMacFrameInfo info{};
        if (snow_capture_macos_frame_info(event.frame, &info) != SNOW_MAC_OK || !info.width ||
            !info.height || info.width > uint32_t(std::numeric_limits<int>::max() / 4) ||
            info.height > uint32_t(std::numeric_limits<int>::max()) || info.desktop_width <= 0 ||
            info.desktop_height <= 0) {
            snow_capture_macos_frame_release(event.frame);
            return false;
        }
        output->pixels = std::make_shared<MacWindowFrame>(event.frame, info);
        output->desktopBounds =
            QRectF(info.desktop_x, info.desktop_y, info.desktop_width, info.desktop_height);
        output->duplicate = info.duplicate != 0;
        return true;
    }

  private:
    LiveWindowTarget m_target;
    SnowMacCancellation* m_cancellation = snow_capture_macos_cancellation_create();
    SnowMacStream* m_stream = nullptr;
};
} // namespace
std::unique_ptr<NativeWindowCapture> createNativeWindowCapture(const LiveWindowTarget& target) {
    return std::make_unique<MacWindowCapture>(target);
}
} // namespace snow_shot::presentation::live_window_detail

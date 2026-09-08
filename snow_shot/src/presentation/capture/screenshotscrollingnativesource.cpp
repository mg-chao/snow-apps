#include "screenshotscrollingpipeline.h"
#include "snow_capture.h"

#include <limits>

namespace snow_shot::capture_detail {
namespace {
class NativeScrollingSource final : public ScrollingFrameSource {
  public:
    explicit NativeScrollingSource(SnowCaptureStream* stream) : m_stream(stream) {}
    ~NativeScrollingSource() override {
        stop();
        snow_capture_stream_destroy(m_stream);
    }
    ScrollingSourceEvent receive(int timeoutMilliseconds) override {
        SnowCaptureStreamEvent event{};
        ScrollingSourceEvent result;
        if (snow_capture_stream_receive(m_stream, static_cast<std::uint32_t>(timeoutMilliseconds),
                                        &event) == 0) {
            result.kind = ScrollingSourceEvent::Kind::Error;
            result.error = QString::fromUtf8(snow_capture_last_error_message());
            return result;
        }
        switch (event.kind) {
        case SNOW_CAPTURE_STREAM_EVENT_FRAME: {
            SnowCaptureStreamFrameInfo info{};
            if (!event.frame || snow_capture_stream_frame_info(event.frame, &info) == 0 ||
                !info.rgba_bytes || info.width == 0 || info.height == 0 ||
                info.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max() / 4) ||
                info.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
                info.stride_bytes != info.width * 4 ||
                info.rgba_len < static_cast<std::size_t>(info.stride_bytes) * info.height) {
                if (event.frame)
                    snow_capture_stream_frame_release(event.frame);
                result.kind = ScrollingSourceEvent::Kind::Error;
                result.error = QStringLiteral("capture stream returned an invalid frame");
                return result;
            }
            result.kind = ScrollingSourceEvent::Kind::Frame;
            result.frame.duplicate = info.is_duplicate != 0;
            result.frame.image = QImage(
                info.rgba_bytes, static_cast<int>(info.width), static_cast<int>(info.height),
                static_cast<int>(info.stride_bytes), QImage::Format_RGBA8888,
                [](void* frame) {
                    snow_capture_stream_frame_release(static_cast<SnowCaptureStreamFrame*>(frame));
                },
                event.frame);
            break;
        }
        case SNOW_CAPTURE_STREAM_EVENT_FRAMES_DROPPED:
            result.kind = ScrollingSourceEvent::Kind::Dropped;
            break;
        case SNOW_CAPTURE_STREAM_EVENT_ERROR:
            result.kind = ScrollingSourceEvent::Kind::Error;
            result.error = QString::fromUtf8(snow_capture_last_error_message());
            break;
        case SNOW_CAPTURE_STREAM_EVENT_ENDED:
            result.kind = ScrollingSourceEvent::Kind::Ended;
            break;
        default:
            break;
        }
        return result;
    }
    void setTargetFps(int fps) override {
        if (fps != m_targetFps &&
            snow_capture_stream_set_target_fps(m_stream, static_cast<std::uint32_t>(fps)) != 0) {
            m_targetFps = fps;
        }
    }
    ScrollingSourceStats stats() const override {
        SnowCaptureStreamStats stats{};
        if (snow_capture_stream_stats(m_stream, &stats) == 0)
            return {};
        return {stats.capture_latency_ns, stats.buffer_fill, stats.frames_dropped};
    }
    void stop() override {
        static_cast<void>(snow_capture_stream_stop(m_stream));
    }

  private:
    SnowCaptureStream* m_stream;
    int m_targetFps = 30;
};
} // namespace

ScrollingSourceFactory nativeScrollingSource(QRect selection, bool restoreOriginalColors) {
    return [selection, restoreOriginalColors]() -> std::unique_ptr<ScrollingFrameSource> {
        if (selection.isEmpty())
            return {};
        SnowCaptureStreamConfig config{};
        config.version = SNOW_CAPTURE_STREAM_CONFIG_VERSION;
        config.struct_size = sizeof(config);
        config.x = selection.x();
        config.y = selection.y();
        config.width = static_cast<std::uint32_t>(selection.width());
        config.height = static_cast<std::uint32_t>(selection.height());
        config.target_fps = 30;
        config.min_fps = 1;
        config.buffer_depth = 3;
        config.max_consecutive_errors = 30;
        config.capture_retry_count = 1;
        config.wgc_update_mode = SNOW_CAPTURE_WGC_UPDATE_MODE_COMPLETE_ONLY;
        // Auto tries DXGI first, then WGC and GDI on eligible capture failures.
        config.capture_backend = SNOW_CAPTURE_BACKEND_AUTO;
        config.pixel_format = SNOW_CAPTURE_PIXEL_FORMAT_RGBA8;
        config.adaptive_fps = 1;
        config.include_cursor = 0;
        config.restore_original_colors = restoreOriginalColors ? 1 : 0;
        auto* stream = snow_capture_stream_create_region(&config);
        if (!stream) {
            qWarning("Failed to create continuous scrolling stream: %s",
                     snow_capture_last_error_message());
            return {};
        }
        return std::make_unique<NativeScrollingSource>(stream);
    };
}
} // namespace snow_shot::capture_detail

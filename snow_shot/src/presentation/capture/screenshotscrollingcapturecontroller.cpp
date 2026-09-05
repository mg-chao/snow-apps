#include "snow_shot/presentation/screenshotscrollingcapturecontroller.h"

#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"

#if defined(Q_OS_WIN) || defined(_WIN32)
#include "snow_shot/platform/windows/windowchrome.h"
#endif

#include "adaptivescrollingcapturecadence.h"
#include "latestbridgemailbox.h"
#include "../pinned/screenshotpintoperfinstrumentation.h"

#include "snow_capture.h"
#include "snow_stitch_images.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

Q_LOGGING_CATEGORY(snowShotScrollingCaptureLog, "snowshot.capture.scrolling", QtWarningMsg)

namespace {
using ScrollClock = std::chrono::steady_clock;
using AdaptiveScrollCadence = snow_shot::capture_detail::AdaptiveScrollingCaptureCadence;

struct ScrollWorkerFrame {
    quint64 generation = 0;
    ScrollClock::duration processingDuration{};
    bool changed = false;
    bool fatalError = false;
    SnowStitchFrameEvent event = SNOW_STITCH_FRAME_EVENT_UNMATCHED;
    int addedRows = 0;
    QImage previewImage;
    QSize sourceSize;
    int replacedPreviewRows = 0;
    bool previewReplaced = false;
};

struct ScrollPreviewLayout {
    int targetHeight = 0;
    int patchHeight = 0;
    int replacedRows = 0;
    bool replaced = false;
    bool valid = false;
};

struct ScrollCaptureResult {
    quint64 generation = 0;
    bool wakeConsumer = false;
    bool streamPressure = false;
    bool fatalError = false;
    QString errorMessage;
};

class OwnedScrollFrame {
  public:
    OwnedScrollFrame() = default;
    explicit OwnedScrollFrame(SnowStitchFrameBuffer* frameValue) : frame(frameValue) {}
    ~OwnedScrollFrame() {
        if (frame != nullptr) {
            snow_stitch_frame_buffer_destroy(frame);
        }
    }
    OwnedScrollFrame(const OwnedScrollFrame&) = delete;
    OwnedScrollFrame& operator=(const OwnedScrollFrame&) = delete;
    OwnedScrollFrame(OwnedScrollFrame&& other) noexcept
        : frame(std::exchange(other.frame, nullptr)) {}
    OwnedScrollFrame& operator=(OwnedScrollFrame&& other) noexcept {
        if (this != &other) {
            if (frame != nullptr) {
                snow_stitch_frame_buffer_destroy(frame);
            }
            frame = std::exchange(other.frame, nullptr);
        }
        return *this;
    }
    SnowStitchFrameBuffer* release() {
        return std::exchange(frame, nullptr);
    }

  private:
    SnowStitchFrameBuffer* frame = nullptr;
};

using ScrollFrameMailbox =
    snow_shot::capture_detail::LatestBridgeMailbox<OwnedScrollFrame, quint64>;

void releaseStitchOwnedImage(void* image) {
    snow_stitch_owned_image_destroy(static_cast<SnowStitchOwnedImage*>(image));
}

const char* unmatchedReasonName(SnowStitchUnmatchedReason reason) {
    switch (reason) {
    case SNOW_STITCH_UNMATCHED_REASON_INSUFFICIENT_OVERLAP:
        return "insufficient-overlap";
    case SNOW_STITCH_UNMATCHED_REASON_LOW_INFORMATION:
        return "low-information";
    case SNOW_STITCH_UNMATCHED_REASON_AMBIGUOUS:
        return "ambiguous";
    case SNOW_STITCH_UNMATCHED_REASON_CONFLICTING_REFERENCES:
        return "conflicting-references";
    case SNOW_STITCH_UNMATCHED_REASON_FIXED_CONTENT_DOMINATED:
        return "fixed-content-dominated";
    case SNOW_STITCH_UNMATCHED_REASON_VERIFICATION_FAILED:
        return "verification-failed";
    case SNOW_STITCH_UNMATCHED_REASON_NONE:
    default:
        return "none";
    }
}

const char* limitingStageName(AdaptiveScrollCadence::LimitingStage stage) {
    switch (stage) {
    case AdaptiveScrollCadence::LimitingStage::Capture:
        return "capture";
    case AdaptiveScrollCadence::LimitingStage::Stitch:
        return "stitch";
    case AdaptiveScrollCadence::LimitingStage::Warmup:
    default:
        return "warmup";
    }
}

class ScreenshotScrollingCaptureProducer final : public QObject {
  public:
    using CaptureCompletedCallback = std::function<void(ScrollCaptureResult)>;

    explicit ScreenshotScrollingCaptureProducer(std::shared_ptr<ScrollFrameMailbox> mailbox,
                                                CaptureCompletedCallback callback)
        : m_mailbox(std::move(mailbox)), m_captureCompleted(std::move(callback)) {}

    ~ScreenshotScrollingCaptureProducer() override {
        stopStream();
        if (m_framePool != nullptr) {
            snow_stitch_frame_pool_destroy(m_framePool);
        }
    }

    void begin(quint64 generation, QRect selection, QRect physicalSelection,
               AdaptiveScrollCadence::Config cadenceConfig = {}) {
        m_generation = generation;
        m_selection = selection;
        m_physicalSelection = physicalSelection;
        m_cadence = AdaptiveScrollCadence(cadenceConfig);
        m_lastLoggedFps = -1;
        stopStream();
        if (m_framePool != nullptr) {
            snow_stitch_frame_pool_destroy(m_framePool);
            m_framePool = nullptr;
        }
        if (!ensureFramePool() || !startStream()) {
            ScrollCaptureResult result;
            result.generation = generation;
            result.fatalError = true;
            result.errorMessage = QString::fromUtf8(snow_capture_last_error_message());
            if (m_captureCompleted) {
                m_captureCompleted(std::move(result));
            }
            return;
        }
        m_active.store(true, std::memory_order_release);
        m_stopRequested.store(false, std::memory_order_release);
        const quint64 streamGeneration = m_generation;
        m_consumerThread = std::thread([this, streamGeneration]() {
            consumeStream(streamGeneration);
        });
    }

    void reset(quint64 generation) {
        m_generation = generation;
        stopStream();
        m_selection = {};
        m_physicalSelection = {};
        if (m_framePool != nullptr) {
            snow_stitch_frame_pool_destroy(m_framePool);
            m_framePool = nullptr;
        }
    }

    void recordStitch(quint64 generation, ScrollClock::duration duration) {
        if (!m_active || generation != m_generation) {
            return;
        }
        m_cadence.recordStitch(duration);
        SnowCaptureStreamStats stats{};
        if (m_stream != nullptr && snow_capture_stream_stats(m_stream, &stats) != 0) {
            if (stats.capture_latency_ns != 0) {
                const auto captureNanos = std::min(
                    stats.capture_latency_ns,
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
                m_cadence.recordCapture(
                    std::chrono::nanoseconds(static_cast<std::int64_t>(captureNanos)));
            }
            m_cadence.recordStreamPressure(stats.buffer_fill, stats.frames_dropped);
        }
        applyCadenceTarget();
        logCadenceIfChanged();
    }

    void recordStreamPressure(quint64 generation) {
        if (!m_active || generation != m_generation) {
            return;
        }
        SnowCaptureStreamStats stats{};
        std::uint32_t queueDepth = 2;
        std::uint64_t droppedFrames = 0;
        if (m_stream != nullptr && snow_capture_stream_stats(m_stream, &stats) != 0) {
            queueDepth = std::max(queueDepth, stats.buffer_fill);
            droppedFrames = stats.frames_dropped;
        }
        m_cadence.recordStreamPressure(queueDepth, droppedFrames);
        applyCadenceTarget();
        logCadenceIfChanged();
    }

  private:
    void logCadenceIfChanged() {
        const int fps = static_cast<int>(std::floor(m_cadence.fps()));
        if (fps == m_lastLoggedFps) {
            return;
        }
        m_lastLoggedFps = fps;
        qCDebug(snowShotScrollingCaptureLog,
                "Scrolling capture target rate changed: fps=%d limiting-stage=%s", fps,
                limitingStageName(m_cadence.limitingStage()));
    }

    void applyCadenceTarget() {
        const int targetFps = static_cast<int>(std::lround(m_cadence.fps()));
        if (m_stream != nullptr && targetFps != m_lastAppliedFps) {
            if (snow_capture_stream_set_target_fps(m_stream,
                                                   static_cast<std::uint32_t>(targetFps)) != 0) {
                m_lastAppliedFps = targetFps;
            }
        }
    }

    bool startStream() {
        if (m_physicalSelection.isEmpty()) {
            return false;
        }
        SnowCaptureStreamConfig config{};
        config.version = SNOW_CAPTURE_STREAM_CONFIG_VERSION;
        config.struct_size = sizeof(config);
        config.x = m_physicalSelection.x();
        config.y = m_physicalSelection.y();
        config.width = static_cast<std::uint32_t>(m_physicalSelection.width());
        config.height = static_cast<std::uint32_t>(m_physicalSelection.height());
        config.target_fps = 30;
        config.min_fps = 1;
        config.buffer_depth = 3;
        config.max_consecutive_errors = 30;
        config.capture_retry_count = 1;
        config.wgc_update_mode = SNOW_CAPTURE_WGC_UPDATE_MODE_COMPLETE_ONLY;
        config.capture_backend = SNOW_CAPTURE_BACKEND_WGC;
        config.pixel_format = SNOW_CAPTURE_PIXEL_FORMAT_RGBA8;
        config.adaptive_fps = 1;
        config.include_cursor = 0;
        m_stream = snow_capture_stream_create_region(&config);
        if (m_stream != nullptr) {
            m_lastAppliedFps = 30;
            return true;
        }

        qWarning("Direct scrolling stream unavailable; retrying with automatic backend: %s",
                 snow_capture_last_error_message());
        config.capture_backend = SNOW_CAPTURE_BACKEND_AUTO;
        m_stream = snow_capture_stream_create_region(&config);
        if (m_stream == nullptr) {
            qWarning("Failed to create continuous scrolling stream: %s",
                     snow_capture_last_error_message());
            return false;
        }
        m_lastAppliedFps = 30;
        return true;
    }

    bool ensureFramePool() {
        if (m_framePool != nullptr) {
            return true;
        }
        if (m_selection.isEmpty()) {
            return false;
        }
        m_framePool =
            snow_stitch_frame_pool_create(static_cast<std::uint32_t>(m_selection.width()),
                                          static_cast<std::uint32_t>(m_selection.height()), 6);
        if (m_framePool == nullptr) {
            qWarning("Failed to create scrolling frame pool: %s", snow_stitch_last_error_message());
        }
        return m_framePool != nullptr;
    }

    void stopStream() {
        m_active.store(false, std::memory_order_release);
        m_stopRequested.store(true, std::memory_order_release);
        if (m_stream != nullptr) {
            static_cast<void>(snow_capture_stream_stop(m_stream));
        }
        if (m_consumerThread.joinable()) {
            m_consumerThread.join();
        }
        if (m_stream != nullptr) {
            snow_capture_stream_destroy(m_stream);
            m_stream = nullptr;
        }
        m_lastAppliedFps = -1;
    }

    void consumeStream(quint64 generation) {
        while (!m_stopRequested.load(std::memory_order_acquire)) {
            SnowCaptureStreamEvent event{};
            if (m_stream == nullptr ||
                snow_capture_stream_receive(m_stream, 100, &event) == 0) {
                if (m_stopRequested.load(std::memory_order_acquire)) {
                    break;
                }
                emitCaptureError(generation, QString::fromUtf8(snow_capture_last_error_message()));
                break;
            }

            if (event.kind == SNOW_CAPTURE_STREAM_EVENT_TIMEOUT) {
                continue;
            }

            ScrollCaptureResult result;
            result.generation = generation;
            switch (event.kind) {
            case SNOW_CAPTURE_STREAM_EVENT_FRAME:
                consumeFrame(generation, event.frame, result);
                break;
            case SNOW_CAPTURE_STREAM_EVENT_FRAMES_DROPPED:
                result.streamPressure = true;
                break;
            case SNOW_CAPTURE_STREAM_EVENT_ERROR:
                result.fatalError = true;
                result.errorMessage = QString::fromUtf8(snow_capture_last_error_message());
                break;
            case SNOW_CAPTURE_STREAM_EVENT_ENDED:
                if (!m_stopRequested.load(std::memory_order_acquire)) {
                    result.fatalError = true;
                    result.errorMessage = QStringLiteral("capture stream ended unexpectedly");
                }
                break;
            default:
                break;
            }
            const bool fatalError = result.fatalError;
            if (m_captureCompleted &&
                (result.wakeConsumer || result.streamPressure || result.fatalError)) {
                m_captureCompleted(std::move(result));
            }
            if (fatalError) {
                break;
            }
        }
    }

    void consumeFrame(quint64 generation, SnowCaptureStreamFrame* frame,
                      ScrollCaptureResult& result) {
        if (frame == nullptr) {
            result.fatalError = true;
            result.errorMessage = QStringLiteral("capture stream returned a null frame");
            return;
        }
        SnowCaptureStreamFrameInfo info{};
        if (snow_capture_stream_frame_info(frame, &info) == 0) {
            result.fatalError = true;
            result.errorMessage = QString::fromUtf8(snow_capture_last_error_message());
            snow_capture_stream_frame_release(frame);
            return;
        }
        const size_t expected = static_cast<size_t>(m_selection.width()) *
                                static_cast<size_t>(m_selection.height()) * 4U;
        const bool valid = info.rgba_bytes != nullptr && info.width ==
                               static_cast<std::uint32_t>(m_selection.width()) &&
                           info.height == static_cast<std::uint32_t>(m_selection.height()) &&
                           info.stride_bytes == info.width * 4 && info.rgba_len >= expected;
        const bool mailboxHasCapacity = m_mailbox->hasPendingCapacity();
        if (!valid || info.is_duplicate != 0 || !mailboxHasCapacity) {
            result.streamPressure = !mailboxHasCapacity;
            snow_capture_stream_frame_release(frame);
            return;
        }

        SnowStitchFrameBuffer* stitchFrame = snow_stitch_frame_pool_acquire(m_framePool);
        SnowStitchMutableImageInfo input{};
        const bool validInput = stitchFrame != nullptr &&
                                snow_stitch_frame_buffer_info(stitchFrame, &input) != 0 &&
                                input.rgba_bytes != nullptr && input.rgba_len >= expected &&
                                input.width == info.width && input.height == info.height &&
                                input.stride_bytes == info.stride_bytes;
        if (validInput) {
            std::memcpy(input.rgba_bytes, info.rgba_bytes, expected);
            result.wakeConsumer = m_mailbox->publish(generation, OwnedScrollFrame(stitchFrame));
        } else if (stitchFrame != nullptr) {
            snow_stitch_frame_buffer_destroy(stitchFrame);
        }
        snow_capture_stream_frame_release(frame);
    }

    void emitCaptureError(quint64 generation, QString message) {
        if (!m_captureCompleted) {
            return;
        }
        ScrollCaptureResult result;
        result.generation = generation;
        result.fatalError = true;
        result.errorMessage = std::move(message);
        m_captureCompleted(std::move(result));
    }

    std::shared_ptr<ScrollFrameMailbox> m_mailbox;
    CaptureCompletedCallback m_captureCompleted;
    SnowCaptureStream* m_stream = nullptr;
    SnowStitchFramePool* m_framePool = nullptr;
    quint64 m_generation = 0;
    QRect m_selection;
    QRect m_physicalSelection;
    AdaptiveScrollCadence m_cadence;
    int m_lastLoggedFps = -1;
    int m_lastAppliedFps = -1;
    std::atomic_bool m_active = false;
    std::atomic_bool m_stopRequested = false;
    std::thread m_consumerThread;
};

class ScreenshotScrollingCaptureWorker final : public QObject {
  public:
    ~ScreenshotScrollingCaptureWorker() override {
        if (m_stitchSession != nullptr) {
            snow_stitch_session_destroy(m_stitchSession);
        }
    }

    void begin(quint64 generation, ScreenshotScrollingRecognitionMode mode) {
        m_generation = generation;
        if (m_mode != mode && m_stitchSession != nullptr) {
            snow_stitch_session_destroy(m_stitchSession);
            m_stitchSession = nullptr;
        }
        m_mode = mode;
        resetPreview();
        if (!ensureStitchSession()) {
            return;
        }
        static_cast<void>(snow_stitch_session_reset(m_stitchSession));
    }

    void reset(quint64 generation) {
        m_generation = generation;
        resetPreview();
        if (m_stitchSession != nullptr) {
            static_cast<void>(snow_stitch_session_reset(m_stitchSession));
        }
    }

    ScrollWorkerFrame process(quint64 generation, SnowStitchFrameBuffer* stitchFrame) {
        ScrollWorkerFrame result;
        result.generation = generation;
        if (generation != m_generation || stitchFrame == nullptr || !ensureStitchSession()) {
            if (stitchFrame != nullptr) {
                snow_stitch_frame_buffer_destroy(stitchFrame);
            }
            return result;
        }

        SnowStitchFrameOutcome outcome{};
        if (snow_stitch_session_push_owned(m_stitchSession, &stitchFrame, &outcome) == 0) {
            qWarning("Scrolling screenshot stitching failed: %s", snow_stitch_last_error_message());
            result.fatalError = true;
            return result;
        }

        result.event = outcome.event;
        result.addedRows = static_cast<int>(outcome.added_rows);
        result.changed = outcome.event == SNOW_STITCH_FRAME_EVENT_INITIAL ||
                         outcome.event == SNOW_STITCH_FRAME_EVENT_EXTENDED_BOTTOM ||
                         outcome.event == SNOW_STITCH_FRAME_EVENT_EXTENDED_TOP ||
                         outcome.event == SNOW_STITCH_FRAME_EVENT_EXTENDED_LEFT ||
                         outcome.event == SNOW_STITCH_FRAME_EVENT_EXTENDED_RIGHT;
        if (outcome.event == SNOW_STITCH_FRAME_EVENT_UNMATCHED) {
            qCDebug(snowShotScrollingCaptureLog,
                    "Scrolling screenshot frame was not verified: reason=%s "
                    "score=%.3f second=%.3f content=%.3f fixed=%.3f "
                    "inliers=%.3f features=%u references=%u",
                    unmatchedReasonName(outcome.unmatched_reason),
                    static_cast<double>(outcome.metrics.score),
                    static_cast<double>(outcome.metrics.second_score),
                    static_cast<double>(outcome.metrics.content_coverage),
                    static_cast<double>(outcome.metrics.fixed_coverage),
                    static_cast<double>(outcome.metrics.inlier_ratio),
                    outcome.metrics.feature_support, outcome.metrics.reference_count);
        }
        if (!result.changed) {
            return result;
        }

        const std::uint32_t outputExtent = m_mode == ScreenshotScrollingRecognitionMode::Horizontal
                                               ? outcome.output_width
                                               : outcome.output_height;
        if (outcome.output_width == 0 || outcome.output_height == 0 || outcome.delta_rows == 0 ||
            outcome.delta_rows > outputExtent ||
            outcome.delta_top > outputExtent - outcome.delta_rows) {
            qWarning("Failed to read scrolling screenshot output: %s",
                     snow_stitch_last_error_message());
            result.changed = false;
            result.fatalError = true;
            return result;
        }

        result.sourceSize =
            QSize(static_cast<int>(outcome.output_width), static_cast<int>(outcome.output_height));
        m_lastOutputSize = result.sourceSize;
        const ScrollPreviewLayout preview =
            previewLayout(static_cast<int>(outcome.delta_rows), result.sourceSize, outcome.event,
                          result.addedRows);
        if (!preview.valid) {
            result.changed = false;
            result.fatalError = true;
            return result;
        }
        result.previewImage =
            renderPreviewPatch(static_cast<int>(outcome.delta_top),
                               static_cast<int>(outcome.delta_rows), preview.patchHeight);
        if (result.previewImage.isNull()) {
            qWarning("Failed to render scrolling screenshot preview: %s",
                     snow_stitch_last_error_message());
            result.changed = false;
            result.fatalError = true;
            return result;
        }
        m_emittedPreviewHeight = preview.targetHeight;
        result.replacedPreviewRows = preview.replacedRows;
        result.previewReplaced = preview.replaced;
        return result;
    }

    ScreenshotScrollingSnapshot trimmedSnapshot(int trimTop, int trimBottom) const {
        if (m_stitchSession == nullptr || m_lastOutputSize.isEmpty()) {
            return {};
        }
        // The stitch-session finalize is the scrolling screenshot's export
        // work; the pin trace records it while a scrolling pin sample waits.
        SNOW_SHOT_PIN_PERF_SCOPE("export.scrolling_trimmed_snapshot");
        const int sourceExtent = m_mode == ScreenshotScrollingRecognitionMode::Horizontal
                                     ? m_lastOutputSize.width()
                                     : m_lastOutputSize.height();
        const int top = std::clamp(trimTop, 0, sourceExtent - 1);
        const int bottom = std::clamp(trimBottom, top + 1, sourceExtent);
        SnowStitchSnapshot* snapshot = snow_stitch_session_snapshot_axis(
            m_stitchSession, static_cast<std::uint32_t>(top), static_cast<std::uint32_t>(bottom));
        if (snapshot == nullptr) {
            return {};
        }
        SnowStitchImageInfo info{};
        if (snow_stitch_snapshot_info(snapshot, &info) == 0 || info.width == 0 ||
            info.height == 0 ||
            info.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            info.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            snow_stitch_snapshot_destroy(snapshot);
            return {};
        }
        return ScreenshotScrollingSnapshot::adoptNative(
            snapshot, QSize(static_cast<int>(info.width), static_cast<int>(info.height)));
    }

  private:
    void resetPreview() {
        m_emittedPreviewHeight = 0;
        m_lastOutputSize = {};
    }

    ScrollPreviewLayout previewLayout(int deltaRows, const QSize& outputSize,
                                      SnowStitchFrameEvent event, int addedRows) const {
        if (deltaRows <= 0 || outputSize.isEmpty()) {
            return {};
        }
        constexpr int previewCrossExtent = 128;
        const int outputCrossExtent = m_mode == ScreenshotScrollingRecognitionMode::Horizontal
                                          ? outputSize.height()
                                          : outputSize.width();
        const int outputExtent = m_mode == ScreenshotScrollingRecognitionMode::Horizontal
                                     ? outputSize.width()
                                     : outputSize.height();
        const qreal scale =
            static_cast<qreal>(previewCrossExtent) / static_cast<qreal>(outputCrossExtent);
        const int targetHeight = std::max(1, qCeil(static_cast<qreal>(outputExtent) * scale));
        if (event == SNOW_STITCH_FRAME_EVENT_INITIAL || m_emittedPreviewHeight == 0) {
            return {targetHeight, targetHeight, 0, true, true};
        }

        const bool append = event == SNOW_STITCH_FRAME_EVENT_EXTENDED_BOTTOM ||
                            event == SNOW_STITCH_FRAME_EVENT_EXTENDED_RIGHT;
        const bool prepend = event == SNOW_STITCH_FRAME_EVENT_EXTENDED_TOP ||
                             event == SNOW_STITCH_FRAME_EVENT_EXTENDED_LEFT;
        if (!append && !prepend) {
            return {};
        }

        // Refresh the splice overlap and absorb all scale rounding into this
        // small edge patch so the retained preview tiles never drift in height.
        const int overlapSourceRows = std::max(0, deltaRows - std::max(0, addedRows));
        int replacedRows = 0;
        if (overlapSourceRows > 0) {
            replacedRows = std::clamp(qRound(static_cast<qreal>(overlapSourceRows) * scale), 1,
                                      m_emittedPreviewHeight);
        }
        const int patchHeight = targetHeight - (m_emittedPreviewHeight - replacedRows);
        if (patchHeight <= 0) {
            return {};
        }
        return {targetHeight, patchHeight, replacedRows, false, true};
    }

    QImage renderPreviewPatch(int top, int rows, int targetHeight) const {
        if (m_stitchSession == nullptr || top < 0 || rows <= 0 || targetHeight <= 0) {
            return {};
        }
        constexpr std::uint32_t previewCrossExtent = 128;
        const std::uint32_t targetWidth = m_mode == ScreenshotScrollingRecognitionMode::Horizontal
                                              ? static_cast<std::uint32_t>(targetHeight)
                                              : previewCrossExtent;
        const std::uint32_t targetImageHeight =
            m_mode == ScreenshotScrollingRecognitionMode::Horizontal
                ? previewCrossExtent
                : static_cast<std::uint32_t>(targetHeight);
        SnowStitchOwnedImage* image = snow_stitch_session_render_scaled_axis(
            m_stitchSession, static_cast<std::uint32_t>(top), static_cast<std::uint32_t>(rows),
            targetWidth, targetImageHeight);
        if (image == nullptr) {
            return {};
        }
        SnowStitchImageInfo info{};
        if (snow_stitch_owned_image_info(image, &info) == 0 || info.rgba_bytes == nullptr ||
            info.width != targetWidth || info.height != targetImageHeight ||
            info.stride_bytes != info.width * 4) {
            snow_stitch_owned_image_destroy(image);
            return {};
        }
        QImage preview(info.rgba_bytes, static_cast<int>(info.width), static_cast<int>(info.height),
                       static_cast<int>(info.stride_bytes), QImage::Format_RGBA8888,
                       &releaseStitchOwnedImage, image);
        if (preview.isNull()) {
            snow_stitch_owned_image_destroy(image);
        }
        return preview;
    }

    bool ensureStitchSession() {
        if (m_stitchSession != nullptr) {
            return true;
        }
        SnowStitchConfig config{};
        if (snow_stitch_config_default(&config) == 0) {
            qWarning("Failed to initialize scrolling stitch config: %s",
                     snow_stitch_last_error_message());
            return false;
        }
        config.axis = m_mode == ScreenshotScrollingRecognitionMode::Horizontal
                          ? SNOW_STITCH_AXIS_HORIZONTAL
                          : SNOW_STITCH_AXIS_VERTICAL;
        m_stitchSession = snow_stitch_session_create(&config);
        if (m_stitchSession == nullptr) {
            qWarning("Failed to create scrolling stitch session: %s",
                     snow_stitch_last_error_message());
        }
        return m_stitchSession != nullptr;
    }

    SnowStitchSession* m_stitchSession = nullptr;
    QSize m_lastOutputSize;
    quint64 m_generation = 0;
    int m_emittedPreviewHeight = 0;
    ScreenshotScrollingRecognitionMode m_mode = ScreenshotScrollingRecognitionMode::Vertical;
};

QRect logicalSelectionRect(const ScreenshotGeometryMapper& geometry,
                           const CapturedDisplayModel& display, const QRect& canvasSelection) {
    const ScreenshotHalfOpenRect selection = ScreenshotHalfOpenRect::fromRect(canvasSelection);
    const QPointF topLeft = geometry.logicalPositionForCanvasPoint(display, selection.topLeft());
    const QPointF bottomRight =
        geometry.logicalPositionForCanvasPoint(display, selection.bottomRight());
    return ScreenshotHalfOpenRect::fromEdges(topLeft.x(), topLeft.y(), bottomRight.x(),
                                             bottomRight.y())
        .toAlignedQRect();
}
} // namespace

ScreenshotScrollingSnapshot ScreenshotScrollingSnapshot::adoptNative(void* snapshot, QSize size) {
    ScreenshotScrollingSnapshot result;
    if (snapshot == nullptr || !size.isValid() || size.isEmpty()) {
        return result;
    }
    result.m_snapshot = std::shared_ptr<void>(snapshot, [](void* value) {
        snow_stitch_snapshot_destroy(static_cast<SnowStitchSnapshot*>(value));
    });
    result.m_size = size;
    return result;
}

bool ScreenshotScrollingSnapshot::isValid() const {
    return m_snapshot != nullptr && m_size.isValid() && !m_size.isEmpty();
}

ScreenshotImageRowSource
ScreenshotScrollingSnapshot::rowSource(std::function<bool()> cancellationRequested) const {
    ScreenshotImageRowSource source;
    if (!isValid()) {
        return source;
    }
    source.size = m_size;
    source.cancellationRequested = std::move(cancellationRequested);
    const std::shared_ptr<void> snapshot = m_snapshot;
    source.readRows = [snapshot](int firstRow, int rowCount, qsizetype destinationStride,
                                 uchar* destination, qsizetype destinationSize) {
        if (firstRow < 0 || rowCount <= 0 || destinationStride <= 0 || destination == nullptr ||
            destinationSize <= 0) {
            return false;
        }
        return snow_stitch_snapshot_copy_rows(
                   static_cast<const SnowStitchSnapshot*>(snapshot.get()),
                   static_cast<std::uint32_t>(firstRow), static_cast<std::uint32_t>(rowCount),
                   static_cast<std::size_t>(destinationStride), destination,
                   static_cast<std::size_t>(destinationSize)) != 0;
    };
    return source;
}

QImage ScreenshotScrollingSnapshot::materialize() const {
    if (!isValid()) {
        return {};
    }
    SnowStitchOwnedImage* image =
        snow_stitch_snapshot_materialize(static_cast<const SnowStitchSnapshot*>(m_snapshot.get()));
    if (image == nullptr) {
        return {};
    }
    SnowStitchImageInfo info{};
    if (snow_stitch_owned_image_info(image, &info) == 0 || info.rgba_bytes == nullptr ||
        info.width != static_cast<std::uint32_t>(m_size.width()) ||
        info.height != static_cast<std::uint32_t>(m_size.height()) ||
        info.stride_bytes != info.width * 4) {
        snow_stitch_owned_image_destroy(image);
        return {};
    }
    QImage result(info.rgba_bytes, m_size.width(), m_size.height(),
                  static_cast<int>(info.stride_bytes), QImage::Format_RGBA8888,
                  &releaseStitchOwnedImage, image);
    if (result.isNull()) {
        snow_stitch_owned_image_destroy(image);
    }
    return result;
}

struct ScreenshotScrollingCaptureController::Impl {
    Impl(ScreenshotScrollingCaptureController& ownerValue,
         ScreenshotScrollingCaptureControllerContext contextValue)
        : owner(ownerValue), context(contextValue) {}

    ~Impl() {
        stop(false);
        shutdownWorker();
    }

    bool start(QRect selection, ScreenshotScrollingRecognitionMode requestedMode) {
        if (selection.width() < 1 || selection.height() < 1) {
            return false;
        }

        const CapturedDisplayModel* anchorDisplay = context.geometry.displayForCanvasPoint(
            context.displaySession, ScreenshotHalfOpenRect::fromRect(selection).center());
        if (anchorDisplay == nullptr) {
            anchorDisplay =
                context.geometry.displayForCanvasRect(context.displaySession, QRectF(selection));
        }
        ScreenshotOverlayWindow* anchorOverlay =
            context.displaySession.overlayForDisplay(anchorDisplay);
        if (anchorDisplay == nullptr || anchorOverlay == nullptr) {
            return false;
        }

        if (active) {
            stop(false);
        }
        ensureWorker();
        if (worker == nullptr || producer == nullptr) {
            return false;
        }

        if (!excludeScrollingWindowsFromCapture(anchorOverlay)) {
            return false;
        }

        canvasSelection = selection;
        mode = requestedMode;
        thumbnailHost = anchorOverlay;
        active = true;
        ++generation;
        pendingResultRequestId.reset();
        mailbox->reset(generation);

        context.overlayCoordinator.setScrollingCaptureMode(context.displaySession,
                                                           QRectF(canvasSelection), true);

        const QRect logicalSelection =
            logicalSelectionRect(context.geometry, *anchorDisplay, canvasSelection);
        thumbnailHost->beginScrollingThumbnail(
            logicalSelection.translated(-thumbnailHost->geometry().topLeft()), mode);

        const quint64 requestGeneration = generation;
        const QRect requestSelection = canvasSelection;
        const QRect requestPhysicalSelection =
            canvasSelection.translated(context.geometry.canvasOrigin());
        const AdaptiveScrollCadence::Config requestCadenceConfig = cadenceConfig;
        postCaptureTask([requestGeneration, requestSelection, requestPhysicalSelection,
                         requestCadenceConfig](ScreenshotScrollingCaptureProducer& target) mutable {
            target.begin(requestGeneration, requestSelection, requestPhysicalSelection,
                         requestCadenceConfig);
        });
        const ScreenshotScrollingRecognitionMode requestMode = mode;
        postWorkerTask([requestGeneration, requestMode](ScreenshotScrollingCaptureWorker& target) {
            target.begin(requestGeneration, requestMode);
        });
        return true;
    }

    bool switchMode(ScreenshotScrollingRecognitionMode requestedMode) {
        if (!active || mode == requestedMode || canvasSelection.isEmpty() ||
            thumbnailHost == nullptr) {
            return false;
        }

        const CapturedDisplayModel* anchorDisplay = context.geometry.displayForCanvasPoint(
            context.displaySession, ScreenshotHalfOpenRect::fromRect(canvasSelection).center());
        if (anchorDisplay == nullptr) {
            anchorDisplay = context.geometry.displayForCanvasRect(context.displaySession,
                                                                  QRectF(canvasSelection));
        }
        if (anchorDisplay == nullptr) {
            return false;
        }

        // A direction change invalidates the axis-specific capture and stitch state, but the
        // selection, overlay presentation, and window exclusion must remain active. Advancing
        // the generation drops work from the previous axis without briefly restoring the canvas.
        mode = requestedMode;
        ++generation;
        pendingResultRequestId.reset();
        mailbox->reset(generation);
        latestOutputSize = {};
        cachedSnapshot = {};
        cachedSnapshotTop = -1;
        cachedSnapshotBottom = -1;
        cachedSnapshotGeneration = 0;

        const quint64 requestGeneration = generation;
        const QRect requestSelection = canvasSelection;
        const QRect requestPhysicalSelection =
            canvasSelection.translated(context.geometry.canvasOrigin());
        const AdaptiveScrollCadence::Config requestCadenceConfig = cadenceConfig;
        const QRect logicalSelection =
            logicalSelectionRect(context.geometry, *anchorDisplay, canvasSelection);
        thumbnailHost->beginScrollingThumbnail(
            logicalSelection.translated(-thumbnailHost->geometry().topLeft()), mode);

        postCaptureTask([requestGeneration, requestSelection, requestPhysicalSelection,
                         requestCadenceConfig](ScreenshotScrollingCaptureProducer& target) mutable {
            target.begin(requestGeneration, requestSelection, requestPhysicalSelection,
                         requestCadenceConfig);
        });
        const ScreenshotScrollingRecognitionMode requestMode = mode;
        postWorkerTask([requestGeneration, requestMode](ScreenshotScrollingCaptureWorker& target) {
            target.begin(requestGeneration, requestMode);
        });
        return true;
    }

    void stop(bool restoreScreenshotPresentation) {
        const bool wasActive = active;
        active = false;
        ++generation;
        pendingResultRequestId.reset();
        mailbox->reset(generation);
        latestOutputSize = {};
        cachedSnapshot = {};
        cachedSnapshotTop = -1;
        cachedSnapshotBottom = -1;
        cachedSnapshotGeneration = 0;
        canvasSelection = {};

        if (worker != nullptr) {
            const quint64 resetGeneration = generation;
            postWorkerTask([resetGeneration](ScreenshotScrollingCaptureWorker& target) {
                target.reset(resetGeneration);
            });
        }
        if (producer != nullptr) {
            const quint64 resetGeneration = generation;
            postCaptureTask([resetGeneration](ScreenshotScrollingCaptureProducer& target) {
                target.reset(resetGeneration);
            });
        }

        if (wasActive) {
            context.overlayCoordinator.setScrollingCaptureMode(context.displaySession, QRectF(),
                                                               false);
            if (restoreScreenshotPresentation) {
                context.overlayCoordinator.applyDisplayModels(context.displaySession);
            }
        } else if (thumbnailHost != nullptr) {
            thumbnailHost->clearScrollingThumbnail();
        }
        restoreScrollingWindowsCaptureVisibility();
        thumbnailHost = nullptr;

        // Scrolling workers are session-scoped.  Tear them down after invalidating all
        // in-flight work so the stitch session and its native resources are released between
        // captures; the next start() recreates them on demand.
        shutdownWorker();
    }

    void detachPendingResultRequest() {
        if (pendingResultRequestId.has_value()) {
            detachedResultRequestIds.insert(*pendingResultRequestId);
            pendingResultRequestId.reset();
        }
    }

    bool excludeScrollingWindowsFromCapture(ScreenshotOverlayWindow* overlay) {
#if defined(Q_OS_WIN) || defined(_WIN32)
        ScreenshotToolbarWindow* const toolbar = context.overlayCoordinator.toolbar();
        if (QCoreApplication::arguments().contains(QStringLiteral("--e2e-allow-overlay-capture"))) {
            return overlay != nullptr && toolbar != nullptr;
        }
        if (overlay == nullptr || toolbar == nullptr ||
            !snow_shot::platform::windows::setWindowExcludedFromCapture(overlay, true)) {
            return false;
        }
        if (!snow_shot::platform::windows::setWindowExcludedFromCapture(toolbar, true)) {
            static_cast<void>(
                snow_shot::platform::windows::setWindowExcludedFromCapture(overlay, false));
            return false;
        }

        excludedOverlay = overlay;
        excludedToolbar = toolbar;
#else
        Q_UNUSED(overlay);
#endif
        return true;
    }

    void restoreScrollingWindowsCaptureVisibility() {
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (excludedToolbar != nullptr) {
            static_cast<void>(
                snow_shot::platform::windows::setWindowExcludedFromCapture(excludedToolbar, false));
        }
        if (excludedOverlay != nullptr) {
            static_cast<void>(
                snow_shot::platform::windows::setWindowExcludedFromCapture(excludedOverlay, false));
        }
#endif
        excludedToolbar = nullptr;
        excludedOverlay = nullptr;
    }

    void ensureWorker() {
        if (worker != nullptr && producer != nullptr) {
            return;
        }
        captureThread = new QThread(&owner);
        captureThread->setObjectName(QStringLiteral("snow-shot-scrolling-capture"));
        const QPointer<ScreenshotScrollingCaptureController> receiver(&owner);
        producer = new ScreenshotScrollingCaptureProducer(
            mailbox, [receiver](ScrollCaptureResult result) mutable {
                if (receiver.isNull()) {
                    return;
                }
                static_cast<void>(QMetaObject::invokeMethod(
                    receiver,
                    [receiver, result = std::move(result)]() mutable {
                        if (!receiver.isNull() && receiver->m_impl != nullptr) {
                            receiver->m_impl->handleCapture(std::move(result));
                        }
                    },
                    Qt::QueuedConnection));
            });
        producer->moveToThread(captureThread);
        QObject::connect(captureThread, &QThread::finished, producer, &QObject::deleteLater);
        captureThread->start();

        thread = new QThread(&owner);
        thread->setObjectName(QStringLiteral("snow-shot-scrolling-stitch"));
        worker = new ScreenshotScrollingCaptureWorker;
        worker->moveToThread(thread);
        QObject::connect(thread, &QThread::finished, worker, &QObject::deleteLater);
        thread->start();
    }

    void shutdownWorker() {
        if (captureThread != nullptr) {
            captureThread->quit();
            captureThread->wait();
            delete captureThread;
            captureThread = nullptr;
            producer = nullptr;
        }
        if (thread == nullptr) {
            worker = nullptr;
            return;
        }
        thread->quit();
        thread->wait();
        delete thread;
        thread = nullptr;
        worker = nullptr;
    }

    template <typename Task> void postCaptureTask(Task&& task) {
        if (producer == nullptr) {
            return;
        }
        const QPointer<ScreenshotScrollingCaptureProducer> target(producer);
        static_cast<void>(QMetaObject::invokeMethod(
            producer,
            [target, task = std::forward<Task>(task)]() mutable {
                if (!target.isNull()) {
                    task(*target);
                }
            },
            Qt::QueuedConnection));
    }

    template <typename Task> void postWorkerTask(Task&& task) {
        if (worker == nullptr) {
            return;
        }
        const QPointer<ScreenshotScrollingCaptureWorker> target(worker);
        static_cast<void>(QMetaObject::invokeMethod(
            worker,
            [target, task = std::forward<Task>(task)]() mutable {
                if (!target.isNull()) {
                    task(*target);
                }
            },
            Qt::QueuedConnection));
    }

    void handleCapture(ScrollCaptureResult result) {
        if (!active || result.generation != generation) {
            return;
        }
        if (result.fatalError) {
            qWarning("Scrolling capture stream failed: %s",
                     qUtf8Printable(result.errorMessage));
            ++generation;
            pendingResultRequestId.reset();
            mailbox->reset(generation);
            const quint64 resetGeneration = generation;
            postCaptureTask([resetGeneration](ScreenshotScrollingCaptureProducer& target) {
                target.reset(resetGeneration);
            });
            postWorkerTask([resetGeneration](ScreenshotScrollingCaptureWorker& target) {
                target.reset(resetGeneration);
            });
            return;
        }
        if (result.streamPressure) {
            postCaptureTask([feedbackGeneration = result.generation](
                                 ScreenshotScrollingCaptureProducer& target) {
                target.recordStreamPressure(feedbackGeneration);
            });
        }
        if (result.wakeConsumer) {
            scheduleStitchFrame();
        }
    }

    void scheduleStitchFrame() {
        if (!active || worker == nullptr) {
            return;
        }
        auto next = mailbox->take();
        if (!next.has_value()) {
            return;
        }
        const quint64 requestGeneration = next->generation;
        auto sharedFrame = std::make_shared<OwnedScrollFrame>(std::move(next->item));
        const QPointer<ScreenshotScrollingCaptureController> receiver(&owner);
        postWorkerTask([receiver, requestGeneration,
                        sharedFrame](ScreenshotScrollingCaptureWorker& target) {
            const ScrollClock::time_point startedAt = ScrollClock::now();
            ScrollWorkerFrame result = target.process(requestGeneration, sharedFrame->release());
            result.processingDuration = ScrollClock::now() - startedAt;
            if (receiver.isNull()) {
                return;
            }
            static_cast<void>(QMetaObject::invokeMethod(
                receiver,
                [receiver, result = std::move(result)]() mutable {
                    if (!receiver.isNull() && receiver->m_impl != nullptr) {
                        receiver->m_impl->handleFrame(std::move(result));
                    }
                },
                Qt::QueuedConnection));
        });
    }

    void handleFrame(ScrollWorkerFrame result) {
        const bool hasNext = mailbox->finish(result.generation);
        if (!active || result.generation != generation) {
            return;
        }
        const quint64 feedbackGeneration = result.generation;
        const ScrollClock::duration feedbackDuration = result.processingDuration;
        postCaptureTask(
            [feedbackGeneration, feedbackDuration](ScreenshotScrollingCaptureProducer& target) {
                target.recordStitch(feedbackGeneration, feedbackDuration);
            });
        if (result.fatalError) {
            ++generation;
            pendingResultRequestId.reset();
            mailbox->reset(generation);
            const quint64 resetGeneration = generation;
            postCaptureTask([resetGeneration](ScreenshotScrollingCaptureProducer& target) {
                target.reset(resetGeneration);
            });
            postWorkerTask([resetGeneration](ScreenshotScrollingCaptureWorker& target) {
                target.reset(resetGeneration);
            });
            return;
        }
        if (hasNext) {
            scheduleStitchFrame();
        }
        if (!result.changed || result.sourceSize.isEmpty()) {
            return;
        }

        if (thumbnailHost == nullptr) {
            return;
        }

        using Change = ScreenshotScrollingStitchChange;
        Change change = Change::Replaced;
        switch (result.event) {
        case SNOW_STITCH_FRAME_EVENT_INITIAL:
            change = Change::Initial;
            break;
        case SNOW_STITCH_FRAME_EVENT_EXTENDED_BOTTOM:
            change = Change::AppendedDown;
            break;
        case SNOW_STITCH_FRAME_EVENT_EXTENDED_TOP:
            change = Change::PrependedUp;
            break;
        case SNOW_STITCH_FRAME_EVENT_EXTENDED_RIGHT:
            change = Change::AppendedRight;
            break;
        case SNOW_STITCH_FRAME_EVENT_EXTENDED_LEFT:
            change = Change::PrependedLeft;
            break;
        case SNOW_STITCH_FRAME_EVENT_COVERED:
        case SNOW_STITCH_FRAME_EVENT_DUPLICATE:
        case SNOW_STITCH_FRAME_EVENT_UNMATCHED:
        default:
            change = Change::Replaced;
            break;
        }
        thumbnailHost->updateScrollingThumbnail(result.previewImage, result.sourceSize, change,
                                                result.addedRows, result.previewReplaced,
                                                result.replacedPreviewRows);
        latestOutputSize = result.sourceSize;
        cachedSnapshot = {};
        cachedSnapshotTop = -1;
        cachedSnapshotBottom = -1;
        cachedSnapshotGeneration = 0;
    }

    QSize trimmedSize() const {
        if (!active || thumbnailHost == nullptr || latestOutputSize.isEmpty()) {
            return {};
        }
        const ScreenshotScrollingTrimRange trim = thumbnailHost->scrollingThumbnailTrim();
        if (!trim.isValid()) {
            return {};
        }
        const int extent = mode == ScreenshotScrollingRecognitionMode::Horizontal
                               ? latestOutputSize.width()
                               : latestOutputSize.height();
        const int top = std::clamp(trim.top, 0, extent - 1);
        const int bottom = std::clamp(trim.bottom, top + 1, extent);
        return mode == ScreenshotScrollingRecognitionMode::Horizontal
                   ? QSize(bottom - top, latestOutputSize.height())
                   : QSize(latestOutputSize.width(), bottom - top);
    }

    bool
    requestTrimmedSnapshot(ScreenshotScrollingCaptureController::SnapshotResultCallback callback) {
        if (!active || worker == nullptr || thumbnailHost == nullptr ||
            latestOutputSize.isEmpty() || !callback || pendingResultRequestId.has_value()) {
            return false;
        }
        const ScreenshotScrollingTrimRange trim = thumbnailHost->scrollingThumbnailTrim();
        if (!trim.isValid()) {
            return false;
        }
        if (cachedSnapshot.isValid() && cachedSnapshotGeneration == generation &&
            cachedSnapshotTop == trim.top && cachedSnapshotBottom == trim.bottom) {
            SNOW_SHOT_PIN_PERF_COUNTER("scrolling.snapshot_cache_hit", 1);
            const QPointer<ScreenshotScrollingCaptureController> receiver(&owner);
            QTimer::singleShot(0, &owner, [receiver, cached = cachedSnapshot,
                                            callback = std::move(callback)]() mutable {
                if (!receiver.isNull() && receiver->m_impl != nullptr &&
                    receiver->m_impl->active) {
                    callback(std::move(cached));
                }
            });
            return true;
        }
        const quint64 requestGeneration = generation;
        const quint64 requestId = ++nextResultRequestId;
        pendingResultRequestId = requestId;
        const QPointer<ScreenshotScrollingCaptureWorker> target(worker);
        const QPointer<ScreenshotScrollingCaptureController> receiver(&owner);
        const bool invoked = QMetaObject::invokeMethod(
            worker,
            [target, receiver, requestId, requestGeneration, trim,
             callback = std::move(callback)]() mutable {
                ScreenshotScrollingSnapshot result;
                if (!target.isNull()) {
                    result = target->trimmedSnapshot(trim.top, trim.bottom);
                }
                if (receiver.isNull()) {
                    return;
                }
                static_cast<void>(QMetaObject::invokeMethod(
                    receiver,
                    [receiver, requestId, requestGeneration, trim, result = std::move(result),
                     callback = std::move(callback)]() mutable {
                        if (receiver.isNull() || receiver->m_impl == nullptr ||
                            (receiver->m_impl->pendingResultRequestId != requestId &&
                             !receiver->m_impl->detachedResultRequestIds.contains(requestId))) {
                            return;
                        }
                        const bool detached =
                            receiver->m_impl->detachedResultRequestIds.contains(requestId);
                        if (detached) {
                            receiver->m_impl->detachedResultRequestIds.remove(requestId);
                        } else {
                            receiver->m_impl->pendingResultRequestId.reset();
                        }
                        if (!detached && (!receiver->m_impl->active ||
                                          receiver->m_impl->generation != requestGeneration)) {
                            return;
                        }
                        if (result.isValid()) {
                            receiver->m_impl->cachedSnapshot = result;
                            receiver->m_impl->cachedSnapshotTop = trim.top;
                            receiver->m_impl->cachedSnapshotBottom = trim.bottom;
                            receiver->m_impl->cachedSnapshotGeneration = requestGeneration;
                        }
                        callback(std::move(result));
                    },
                    Qt::QueuedConnection));
            },
            Qt::QueuedConnection);
        if (!invoked && pendingResultRequestId == requestId) {
            pendingResultRequestId.reset();
        }
        return invoked;
    }

    ScreenshotScrollingCaptureController& owner;
    ScreenshotScrollingCaptureControllerContext context;
    AdaptiveScrollCadence::Config cadenceConfig;
    std::shared_ptr<ScrollFrameMailbox> mailbox = std::make_shared<ScrollFrameMailbox>();
    QThread* captureThread = nullptr;
    ScreenshotScrollingCaptureProducer* producer = nullptr;
    QThread* thread = nullptr;
    ScreenshotScrollingCaptureWorker* worker = nullptr;
    QPointer<ScreenshotOverlayWindow> thumbnailHost;
    QPointer<ScreenshotOverlayWindow> excludedOverlay;
    QPointer<ScreenshotToolbarWindow> excludedToolbar;
    QSize latestOutputSize;
    ScreenshotScrollingSnapshot cachedSnapshot;
    int cachedSnapshotTop = -1;
    int cachedSnapshotBottom = -1;
    quint64 cachedSnapshotGeneration = 0;
    std::optional<quint64> pendingResultRequestId;
    QSet<quint64> detachedResultRequestIds;
    QRect canvasSelection;
    quint64 generation = 0;
    quint64 nextResultRequestId = 0;
    bool active = false;
    ScreenshotScrollingRecognitionMode mode = ScreenshotScrollingRecognitionMode::Vertical;
};

ScreenshotScrollingCaptureController::ScreenshotScrollingCaptureController(
    ScreenshotScrollingCaptureControllerContext context, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, context)) {}

ScreenshotScrollingCaptureController::~ScreenshotScrollingCaptureController() = default;

bool ScreenshotScrollingCaptureController::start(const QRect& canvasSelection,
                                                 ScreenshotScrollingRecognitionMode mode) {
    return m_impl->start(canvasSelection, mode);
}

bool ScreenshotScrollingCaptureController::setRecognitionMode(
    ScreenshotScrollingRecognitionMode mode) {
    if (m_impl->mode == mode) {
        return false;
    }
    if (!m_impl->active) {
        m_impl->mode = mode;
        return true;
    }
    return m_impl->switchMode(mode);
}

ScreenshotScrollingRecognitionMode ScreenshotScrollingCaptureController::recognitionMode() const {
    return m_impl->mode;
}

void ScreenshotScrollingCaptureController::stop(bool restoreScreenshotPresentation) {
    m_impl->stop(restoreScreenshotPresentation);
}

bool ScreenshotScrollingCaptureController::active() const {
    return m_impl->active;
}

QSize ScreenshotScrollingCaptureController::trimmedSize() const {
    return m_impl->trimmedSize();
}

bool ScreenshotScrollingCaptureController::requestTrimmedSnapshot(SnapshotResultCallback callback) {
    return m_impl->requestTrimmedSnapshot(std::move(callback));
}

void ScreenshotScrollingCaptureController::detachPendingResultRequest() {
    m_impl->detachPendingResultRequest();
}

QRect ScreenshotScrollingCaptureController::canvasSelection() const {
    return m_impl->canvasSelection;
}

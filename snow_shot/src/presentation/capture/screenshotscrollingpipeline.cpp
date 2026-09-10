#include "screenshotscrollingpipeline.h"
#include "screenshotscrollingdiagnostics.h"
#include "latestbridgemailbox.h"
#include "../pinned/screenshotpintoperfinstrumentation.h"
#include "snow_stitch_images.h"

#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <optional>
#include <thread>
#include <utility>

Q_LOGGING_CATEGORY(snowShotScrollingCaptureLog, "snowshot.capture.scrolling", QtWarningMsg)

namespace snow_shot::capture_detail {
namespace {
using AdaptiveScrollCadence = AdaptiveScrollingCaptureCadence;
using ScrollWorkerFrame = ScrollingPipelineFrame;
#if defined(SNOW_SHOT_SCROLLING_PERF_DETAIL)
class PerfResultReader final {
  public:
    explicit PerfResultReader(scrolling_perf::FrameTrace trace) : m_trace(std::move(trace)) {}
    ~PerfResultReader() {
        SnowStitchPerfSnapshot snapshot{};
        if (m_trace && snow_stitch_perf_read_thread(&snapshot, sizeof(snapshot)) != 0) {
            std::copy(std::begin(snapshot.elapsed_ns), std::end(snapshot.elapsed_ns),
                      m_trace->rustElapsed.begin());
            std::copy(std::begin(snapshot.calls), std::end(snapshot.calls),
                      m_trace->rustCalls.begin());
        }
    }

  private:
    scrolling_perf::FrameTrace m_trace;
};
#endif
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
            SNOW_SCROLL_TRACE(trace, trace->disposition = "mailbox_cancelled");
            snow_stitch_frame_buffer_destroy(frame);
        }
    }
    OwnedScrollFrame(const OwnedScrollFrame&) = delete;
    OwnedScrollFrame& operator=(const OwnedScrollFrame&) = delete;
    OwnedScrollFrame(OwnedScrollFrame&& other) noexcept
        : trace(std::move(other.trace)), frame(std::exchange(other.frame, nullptr)) {}
    OwnedScrollFrame& operator=(OwnedScrollFrame&& other) noexcept {
        if (this != &other) {
            if (frame != nullptr) {
                SNOW_SCROLL_TRACE(trace, trace->disposition = "mailbox_cancelled");
                snow_stitch_frame_buffer_destroy(frame);
            }
            trace = std::move(other.trace);
            frame = std::exchange(other.frame, nullptr);
        }
        return *this;
    }
    scrolling_perf::FrameTrace trace;

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

    ScrollWorkerFrame process(quint64 generation, SnowStitchFrameBuffer* stitchFrame,
                              scrolling_perf::FrameTrace trace) {
        ScrollWorkerFrame result;
        result.generation = generation;
        result.trace = std::move(trace);
#if defined(SNOW_SHOT_SCROLLING_PERF_DETAIL)
        snow_stitch_perf_reset_thread();
        PerfResultReader perfReader(result.trace);
#endif
        if (generation != m_generation || stitchFrame == nullptr || !ensureStitchSession()) {
            if (stitchFrame != nullptr) {
                snow_stitch_frame_buffer_destroy(stitchFrame);
            }
            return result;
        }

        SnowStitchFrameOutcome outcome{};
        int pushed = 0;
        {
            SNOW_SCROLL_SCOPE(result.trace, Stitch);
            pushed = snow_stitch_session_push_owned(m_stitchSession, &stitchFrame, &outcome);
        }
        if (pushed == 0) {
            qWarning("Scrolling screenshot stitching failed: %s", snow_stitch_last_error_message());
            result.fatalError = true;
            return result;
        }

        result.event = outcome.event;
        result.sourceSize =
            QSize(static_cast<int>(outcome.output_width), static_cast<int>(outcome.output_height));
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

        m_lastOutputSize = result.sourceSize;
        ScrollPreviewLayout preview;
        {
            SNOW_SCROLL_SCOPE(result.trace, PreviewLayout);
            preview = previewLayout(static_cast<int>(outcome.delta_rows), result.sourceSize,
                                    outcome.event, result.addedRows);
        }
        if (!preview.valid) {
            result.changed = false;
            result.fatalError = true;
            return result;
        }
        result.previewImage = renderPreviewPatch(static_cast<int>(outcome.delta_top),
                                                 static_cast<int>(outcome.delta_rows),
                                                 preview.patchHeight, result.trace);
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

    QImage renderPreviewPatch(int top, int rows, int targetHeight,
                              scrolling_perf::FrameTrace trace) const {
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
        SnowStitchOwnedImage* image = nullptr;
        {
            SNOW_SCROLL_SCOPE(trace, PreviewRender);
            image = snow_stitch_session_render_scaled_axis(
                m_stitchSession, static_cast<std::uint32_t>(top), static_cast<std::uint32_t>(rows),
                targetWidth, targetImageHeight);
        }
        SNOW_SCROLL_SCOPE(trace, PreviewWrap);
        Q_UNUSED(trace);
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

class ScreenshotScrollingCaptureProducer final : public QObject {
  public:
    using Callback = std::function<void(ScrollCaptureResult)>;
    ScreenshotScrollingCaptureProducer(std::shared_ptr<ScrollFrameMailbox> mailbox,
                                       Callback callback)
        : m_mailbox(std::move(mailbox)), m_callback(std::move(callback)) {}
    ~ScreenshotScrollingCaptureProducer() override {
        reset(m_generation);
    }

    void begin(quint64 generation, QSize viewport, ScrollingSourceFactory factory,
               AdaptiveScrollCadence::Config config) {
        reset(generation);
        m_viewport = viewport;
        m_cadence = AdaptiveScrollCadence(config);
        m_pool = snow_stitch_frame_pool_create(static_cast<std::uint32_t>(viewport.width()),
                                               static_cast<std::uint32_t>(viewport.height()), 6);
        logScrollingEvent("scrolling.source_initializing", generation,
                          {{QStringLiteral("width"), viewport.width()},
                           {QStringLiteral("height"), viewport.height()}});
        m_diagnostics = {};
        m_source = factory();
        if (!m_pool || !m_source) {
            m_callback({generation, false, false, true,
                        QStringLiteral("could not initialize scrolling frame source or pool")});
            return;
        }
        logScrollingEvent("scrolling.source_ready", generation, m_diagnostics.fields());
        m_active.store(true);
        m_consumer = std::thread([this, generation]() { consume(generation); });
    }
    void reset(quint64 generation) {
        m_active.store(false);
        if (m_source)
            m_source->stop();
        if (m_consumer.joinable())
            m_consumer.join();
        m_source.reset();
        if (m_pool)
            snow_stitch_frame_pool_destroy(std::exchange(m_pool, nullptr));
        m_generation = generation;
    }
    void pause(quint64 generation) {
        if (m_generation == generation)
            reset(generation);
    }
    void recordStitch(quint64 generation, ScrollClock::duration duration) {
        if (!m_active.load() || generation != m_generation)
            return;
        m_cadence.recordStitch(duration);
        const auto stats = m_source->stats();
        if (stats.captureLatencyNs) {
            m_cadence.recordCapture(std::chrono::nanoseconds(stats.captureLatencyNs));
        }
        m_cadence.recordStreamPressure(stats.bufferedFrames, stats.droppedFrames);
        m_source->setTargetFps(static_cast<int>(std::lround(m_cadence.fps())));
    }
    void recordStreamPressure(quint64 generation) {
        if (!m_active.load() || generation != m_generation)
            return;
        const auto stats = m_source->stats();
        m_cadence.recordStreamPressure(std::max(2U, stats.bufferedFrames), stats.droppedFrames);
        m_source->setTargetFps(static_cast<int>(std::lround(m_cadence.fps())));
    }

  private:
    void consume(quint64 generation) {
        while (m_active.load()) {
            auto event = m_source->receive(100);
            if (m_diagnostics.reportDue(ScrollClock::now())) {
                logScrollingEvent("scrolling.capture_progress", generation, m_diagnostics.fields(),
                                  m_diagnostics.accepted == 0 ? QtWarningMsg : QtInfoMsg);
            }
            if (!m_active.load()) {
                SNOW_SCROLL_TRACE(event.frame.trace,
                                  event.frame.trace->disposition = "source_cancelled");
                break;
            }
            ScrollCaptureResult result;
            result.generation = generation;
            switch (event.kind) {
            case ScrollingSourceEvent::Kind::Frame:
                consumeFrame(generation, std::move(event.frame), result);
                break;
            case ScrollingSourceEvent::Kind::Dropped:
                ++m_diagnostics.droppedEvents;
                result.streamPressure = true;
                break;
            case ScrollingSourceEvent::Kind::Error:
            case ScrollingSourceEvent::Kind::Ended:
                result.fatalError = true;
                result.errorMessage = event.error.isEmpty()
                                          ? QStringLiteral("scrolling source ended unexpectedly")
                                          : event.error;
                break;
            case ScrollingSourceEvent::Kind::Timeout:
                ++m_diagnostics.timeouts;
                continue;
            }
            const bool fatal = result.fatalError;
            if (result.wakeConsumer || result.streamPressure || fatal)
                m_callback(std::move(result));
            if (fatal)
                break;
        }
        logScrollingEvent("scrolling.capture_summary", generation, m_diagnostics.fields());
    }
    void consumeFrame(quint64 generation, ScrollingSourceFrame source,
                      ScrollCaptureResult& result) {
        if (++m_diagnostics.received == 1) {
            logScrollingEvent(
                "scrolling.first_frame", generation,
                {{QStringLiteral("width"), source.image.width()},
                 {QStringLiteral("height"), source.image.height()},
                 {QStringLiteral("pixel_format"), static_cast<int>(source.image.format())},
                 {QStringLiteral("stride_bytes"), source.image.bytesPerLine()},
                 {QStringLiteral("duplicate"), source.duplicate}});
        }
        const auto trace = source.trace;
        SNOW_SCROLL_TRACE(trace, trace->record(scrolling_perf::Stage::SourceQueueWait,
                                               scrolling_perf::now() - trace->publishedAt));
        const auto expected = static_cast<std::size_t>(m_viewport.width()) *
                              static_cast<std::size_t>(m_viewport.height()) * 4U;
        const bool valid = !source.image.isNull() && source.image.size() == m_viewport &&
                           source.image.format() == QImage::Format_RGBA8888 &&
                           source.image.bytesPerLine() == m_viewport.width() * 4;
        const bool capacity = m_mailbox->hasPendingCapacity();
        SNOW_SCROLL_TRACE(trace, trace->queueDepth = m_mailbox->pendingDepth());
        if (!valid || source.duplicate || !capacity) {
            if (!valid) {
                if (++m_diagnostics.invalid == 1) {
                    logScrollingEvent(
                        "scrolling.frame_rejected", generation,
                        {{QStringLiteral("width"), source.image.width()},
                         {QStringLiteral("height"), source.image.height()},
                         {QStringLiteral("expected_width"), m_viewport.width()},
                         {QStringLiteral("expected_height"), m_viewport.height()},
                         {QStringLiteral("pixel_format"), static_cast<int>(source.image.format())},
                         {QStringLiteral("stride_bytes"), source.image.bytesPerLine()}},
                        QtWarningMsg);
                }
            } else if (source.duplicate) {
                ++m_diagnostics.duplicates;
            } else {
                ++m_diagnostics.mailboxDropped;
            }
            result.streamPressure = !capacity;
            SNOW_SCROLL_TRACE(trace, trace->disposition = !valid             ? "invalid"
                                                          : source.duplicate ? "duplicate"
                                                                             : "mailbox_dropped");
            return;
        }
        SnowStitchFrameBuffer* frame = nullptr;
        SnowStitchMutableImageInfo input{};
        {
            SNOW_SCROLL_SCOPE(trace, PoolAcquire);
            frame = snow_stitch_frame_pool_acquire(m_pool);
        }
        const bool acquired = frame && snow_stitch_frame_buffer_info(frame, &input) != 0 &&
                              input.rgba_bytes && input.rgba_len >= expected &&
                              input.width == static_cast<std::uint32_t>(m_viewport.width()) &&
                              input.height == static_cast<std::uint32_t>(m_viewport.height()) &&
                              input.stride_bytes == input.width * 4;
        if (!acquired) {
            ++m_diagnostics.poolUnavailable;
            if (frame)
                snow_stitch_frame_buffer_destroy(frame);
            SNOW_SCROLL_TRACE(trace, trace->disposition = "pool_unavailable");
            return;
        }
        {
            SNOW_SCROLL_SCOPE(trace, RgbaCopy);
            std::memcpy(input.rgba_bytes, source.image.constBits(), expected);
        }
        ++m_diagnostics.accepted;
        OwnedScrollFrame owned(frame);
        owned.trace = trace;
        SNOW_SCROLL_TRACE(trace, trace->publishedAt = scrolling_perf::now());
        SNOW_SCROLL_TRACE(trace, trace->disposition = "queued");
        {
            SNOW_SCROLL_SCOPE(trace, MailboxAdmission);
            result.wakeConsumer = m_mailbox->publish(generation, std::move(owned));
        }
    }
    std::shared_ptr<ScrollFrameMailbox> m_mailbox;
    Callback m_callback;
    std::unique_ptr<ScrollingFrameSource> m_source;
    SnowStitchFramePool* m_pool = nullptr;
    QSize m_viewport;
    ScrollingCaptureDiagnostics m_diagnostics;
    quint64 m_generation = 0;
    AdaptiveScrollCadence m_cadence;
    std::atomic_bool m_active = false;
    std::thread m_consumer;
};

ScreenshotScrollingStitchChange stitchChange(int event) {
    using Change = ScreenshotScrollingStitchChange;
    switch (event) {
    case SNOW_STITCH_FRAME_EVENT_INITIAL:
        return Change::Initial;
    case SNOW_STITCH_FRAME_EVENT_EXTENDED_BOTTOM:
        return Change::AppendedDown;
    case SNOW_STITCH_FRAME_EVENT_EXTENDED_TOP:
        return Change::PrependedUp;
    case SNOW_STITCH_FRAME_EVENT_EXTENDED_RIGHT:
        return Change::AppendedRight;
    case SNOW_STITCH_FRAME_EVENT_EXTENDED_LEFT:
        return Change::PrependedLeft;
    default:
        return Change::Replaced;
    }
}
} // namespace

struct ScreenshotScrollingPipeline::Impl {
    Impl(ScreenshotScrollingPipeline& ownerValue, FrameCallback frames, ErrorCallback errors)
        : owner(ownerValue), frameCallback(std::move(frames)), errorCallback(std::move(errors)) {
        const QPointer<ScreenshotScrollingPipeline> receiver(&owner);
        producer =
            new ScreenshotScrollingCaptureProducer(mailbox, [receiver](ScrollCaptureResult result) {
                if (!receiver)
                    return;
                QMetaObject::invokeMethod(
                    receiver,
                    [receiver, result = std::move(result)]() mutable {
                        if (receiver)
                            receiver->m_impl->handleCapture(std::move(result));
                    },
                    Qt::QueuedConnection);
            });
        producer->moveToThread(&captureThread);
        QObject::connect(&captureThread, &QThread::finished, producer, &QObject::deleteLater);
        worker = new ScreenshotScrollingCaptureWorker;
        worker->moveToThread(&stitchThread);
        QObject::connect(&stitchThread, &QThread::finished, worker, &QObject::deleteLater);
        captureThread.setObjectName(QStringLiteral("snow-shot-scrolling-capture"));
        stitchThread.setObjectName(QStringLiteral("snow-shot-scrolling-stitch"));
        captureThread.start();
        stitchThread.start();
    }
    ~Impl() {
        active = false;
        QMetaObject::invokeMethod(
            producer, [this]() { producer->reset(generation); }, Qt::BlockingQueuedConnection);
        captureThread.quit();
        captureThread.wait();
        stitchThread.quit();
        stitchThread.wait();
    }
    void handleCapture(ScrollCaptureResult result) {
        if (!active || result.generation != generation)
            return;
        if (result.fatalError) {
            if (errorCallback)
                errorCallback(generation, std::move(result.errorMessage));
            return;
        }
        if (result.streamPressure) {
            QMetaObject::invokeMethod(
                producer, [this, value = generation]() { producer->recordStreamPressure(value); },
                Qt::QueuedConnection);
        }
        if (result.wakeConsumer)
            schedule();
    }
    void schedule() {
        if (!active)
            return;
        auto next = mailbox->take();
        if (!next)
            return;
        busy = true;
        auto frame = std::make_shared<OwnedScrollFrame>(std::move(next->item));
        auto trace = frame->trace;
        SNOW_SCROLL_TRACE(trace, trace->dispatchedAt = scrolling_perf::now());
        SNOW_SCROLL_TRACE(trace, trace->record(scrolling_perf::Stage::MailboxWait,
                                               trace->dispatchedAt - trace->publishedAt));
        const QPointer<ScreenshotScrollingPipeline> receiver(&owner);
        QMetaObject::invokeMethod(
            worker,
            [receiver, target = worker, value = next->generation, frame, trace]() mutable {
                SNOW_SCROLL_TRACE(trace,
                                  trace->record(scrolling_perf::Stage::WorkerDispatch,
                                                scrolling_perf::now() - trace->dispatchedAt));
                const auto started = ScrollClock::now();
                auto result = target->process(value, frame->release(), trace);
                result.processingDuration = ScrollClock::now() - started;
                result.change = stitchChange(result.event);
                SNOW_SCROLL_TRACE(trace, trace->completedAt = scrolling_perf::now());
                if (!receiver)
                    return;
                QMetaObject::invokeMethod(
                    receiver,
                    [receiver, result = std::move(result)]() mutable {
                        if (receiver)
                            receiver->m_impl->handleFrame(std::move(result));
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }
    void handleFrame(ScrollingPipelineFrame result) {
        const bool next = mailbox->finish(result.generation);
        if (result.generation != generation) {
            SNOW_SCROLL_TRACE(result.trace, result.trace->disposition = "stale_generation");
            return;
        }
        busy = false;
        if (!active) {
            SNOW_SCROLL_TRACE(result.trace, result.trace->disposition = "cancelled");
            return;
        }
        if (++processedFrames == 1 || result.fatalError) {
            logScrollingEvent("scrolling.stitch_result", generation,
                              {{QStringLiteral("count"), processedFrames},
                               {QStringLiteral("code"), result.event},
                               {QStringLiteral("changed"), result.changed},
                               {QStringLiteral("fatal"), result.fatalError},
                               {QStringLiteral("width"), result.sourceSize.width()},
                               {QStringLiteral("height"), result.sourceSize.height()}},
                              result.fatalError ? QtWarningMsg : QtInfoMsg);
        }
        auto trace = result.trace;
        Q_UNUSED(trace);
        SNOW_SCROLL_TRACE(trace, trace->record(scrolling_perf::Stage::ReturnQueueWait,
                                               scrolling_perf::now() - trace->completedAt));
        SNOW_SCROLL_TRACE(trace, trace->disposition = result.fatalError ? "error" : "processed");
        SNOW_SCROLL_TRACE(trace, trace->event = result.event);
        SNOW_SCROLL_TRACE(trace, trace->outputHeight = result.sourceSize.height());
        QMetaObject::invokeMethod(
            producer,
            [this, value = generation, duration = result.processingDuration]() {
                producer->recordStitch(value, duration);
            },
            Qt::QueuedConnection);
        if (next && !result.fatalError)
            schedule();
        if (frameCallback)
            frameCallback(std::move(result));
    }

    ScreenshotScrollingPipeline& owner;
    FrameCallback frameCallback;
    ErrorCallback errorCallback;
    std::shared_ptr<ScrollFrameMailbox> mailbox = std::make_shared<ScrollFrameMailbox>();
    QThread captureThread;
    QThread stitchThread;
    ScreenshotScrollingCaptureProducer* producer = nullptr;
    ScreenshotScrollingCaptureWorker* worker = nullptr;
    quint64 generation = 0;
    bool active = false;
    bool busy = false;
    qint64 processedFrames = 0;
};

ScreenshotScrollingPipeline::ScreenshotScrollingPipeline(FrameCallback frames, ErrorCallback errors,
                                                         QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, std::move(frames), std::move(errors))) {
}
ScreenshotScrollingPipeline::~ScreenshotScrollingPipeline() = default;

void ScreenshotScrollingPipeline::begin(quint64 generation, QSize viewport,
                                        ScreenshotScrollingRecognitionMode mode,
                                        ScrollingSourceFactory source,
                                        AdaptiveScrollingCaptureCadence::Config cadence) {
    reset(generation);
    QMetaObject::invokeMethod(
        m_impl->worker,
        [target = m_impl->worker, generation, mode]() { target->begin(generation, mode); },
        Qt::QueuedConnection);
    resume(generation, viewport, std::move(source), cadence);
}
void ScreenshotScrollingPipeline::reset(quint64 generation) {
    m_impl->active = false;
    m_impl->generation = generation;
    m_impl->busy = false;
    m_impl->processedFrames = 0;
    m_impl->mailbox->reset(generation);
    QMetaObject::invokeMethod(
        m_impl->producer, [target = m_impl->producer, generation]() { target->reset(generation); },
        Qt::QueuedConnection);
    QMetaObject::invokeMethod(
        m_impl->worker, [target = m_impl->worker, generation]() { target->reset(generation); },
        Qt::QueuedConnection);
}
void ScreenshotScrollingPipeline::pause(quint64 generation) {
    m_impl->active = false;
    m_impl->mailbox->reset(generation);
    QMetaObject::invokeMethod(
        m_impl->producer, [target = m_impl->producer, generation]() { target->pause(generation); },
        Qt::QueuedConnection);
}
void ScreenshotScrollingPipeline::resume(quint64 generation, QSize viewport,
                                         ScrollingSourceFactory source,
                                         AdaptiveScrollingCaptureCadence::Config cadence) {
    m_impl->active = true;
    m_impl->generation = generation;
    QMetaObject::invokeMethod(
        m_impl->producer,
        [target = m_impl->producer, generation, viewport, source = std::move(source), cadence]() {
            target->begin(generation, viewport, source, cadence);
        },
        Qt::QueuedConnection);
}
bool ScreenshotScrollingPipeline::idle() const {
    return !m_impl->busy && m_impl->mailbox->pendingDepth() == 0;
}
void ScreenshotScrollingPipeline::finishInput(std::function<void()> callback) {
    const QPointer<ScreenshotScrollingPipeline> receiver(this);
    QMetaObject::invokeMethod(
        m_impl->producer,
        [target = m_impl->producer, value = m_impl->generation, receiver,
         callback = std::move(callback)]() mutable {
            target->pause(value);
            if (receiver)
                QMetaObject::invokeMethod(
                    receiver,
                    [receiver, callback = std::move(callback)]() {
                        if (receiver)
                            callback();
                    },
                    Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
bool ScreenshotScrollingPipeline::requestSnapshot(int top, int bottom, QObject* receiver,
                                                  SnapshotCallback callback) {
    const QPointer<QObject> guarded(receiver);
    return QMetaObject::invokeMethod(
        m_impl->worker,
        [target = m_impl->worker, top, bottom, guarded, callback = std::move(callback)]() mutable {
            auto result = target->trimmedSnapshot(top, bottom);
            if (!guarded)
                return;
            QMetaObject::invokeMethod(
                guarded,
                [guarded, callback = std::move(callback), result = std::move(result)]() mutable {
                    if (guarded)
                        callback(std::move(result));
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
} // namespace snow_shot::capture_detail

namespace {
void releaseSnapshotImage(void* image) {
    snow_stitch_owned_image_destroy(static_cast<SnowStitchOwnedImage*>(image));
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
                  &releaseSnapshotImage, image);
    if (result.isNull()) {
        snow_stitch_owned_image_destroy(image);
    }
    return result;
}

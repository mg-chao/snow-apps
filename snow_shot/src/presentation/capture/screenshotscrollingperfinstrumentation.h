#pragma once

#include <QtGlobal>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

namespace snow_shot::capture_detail::scrolling_perf {
enum class Stage : std::size_t {
    PacingWait,
    SchedulingLateness,
    ViewportPreparation,
    SourceQueueWait,
    PoolAcquire,
    RgbaCopy,
    MailboxAdmission,
    MailboxWait,
    WorkerDispatch,
    Stitch,
    PreviewLayout,
    PreviewRender,
    PreviewWrap,
    ReturnQueueWait,
    ThumbnailUpdate,
    ThumbnailPaint,
    CaptureToPaint,
    MotionToPaint,
    ThumbnailTiles,
    ThumbnailMetrics,
    ThumbnailTilePaint,
    Count
};
inline constexpr std::array<const char*, static_cast<std::size_t>(Stage::Count)> stageNames{
    "source.pacing_wait",
    "source.scheduling_lateness",
    "source.viewport_preparation",
    "source.queue_wait",
    "input.pool_acquire",
    "input.rgba_copy",
    "input.mailbox_admission",
    "worker.mailbox_wait",
    "worker.dispatch",
    "stitch.push_owned",
    "preview.layout",
    "preview.render",
    "preview.wrap",
    "thumbnail.return_queue_wait",
    "thumbnail.update",
    "thumbnail.paint",
    "latency.capture_to_paint",
    "latency.motion_to_paint",
    "thumbnail.tiles",
    "thumbnail.metrics",
    "thumbnail.tile_paint"};

#if defined(SNOW_SHOT_SCROLLING_PERF_INSTRUMENTATION)
using Clock = std::chrono::steady_clock;
inline qint64 now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}
struct FrameRecord {
    std::uint64_t id = 0;
    int offset = 0;
    qint64 motionAt = 0;
    qint64 capturedAt = 0;
    qint64 publishedAt = 0;
    qint64 dispatchedAt = 0;
    qint64 completedAt = 0;
    qint64 paintedAt = 0;
    int captureFps = 30;
    std::size_t queueDepth = 0;
    const char* disposition = "sampled";
    int event = -1;
    int outputHeight = 0;
    std::array<qint64, static_cast<std::size_t>(Stage::Count)> elapsed{};
    std::array<bool, static_cast<std::size_t>(Stage::Count)> available{};
    std::array<std::uint64_t, 16> rustElapsed{};
    std::array<std::uint64_t, 16> rustCalls{};
    void record(Stage stage, qint64 duration) {
        const auto index = static_cast<std::size_t>(stage);
        elapsed[index] += duration;
        available[index] = true;
    }
};
using FrameTrace = std::shared_ptr<FrameRecord>;
#if defined(SNOW_SHOT_SCROLLING_PERF_DETAIL)
inline thread_local FrameTrace currentTrace;
class TraceContext final {
  public:
    explicit TraceContext(FrameTrace trace)
        : m_previous(std::exchange(currentTrace, std::move(trace))) {}
    ~TraceContext() {
        currentTrace = std::move(m_previous);
    }

  private:
    FrameTrace m_previous;
};
#endif
class Scope final {
  public:
    Scope(FrameTrace trace, Stage stage)
        : m_trace(std::move(trace)), m_stage(stage), m_start(now()) {}
    ~Scope() {
        if (m_trace) {
            m_trace->record(m_stage, now() - m_start);
        }
    }

  private:
    FrameTrace m_trace;
    Stage m_stage;
    qint64 m_start;
};
#else
struct FrameTrace {};
#endif
} // namespace snow_shot::capture_detail::scrolling_perf

#define SNOW_SCROLL_CONCAT_IMPL(a, b) a##b
#define SNOW_SCROLL_CONCAT(a, b) SNOW_SCROLL_CONCAT_IMPL(a, b)
#if defined(SNOW_SHOT_SCROLLING_PERF_INSTRUMENTATION)
#define SNOW_SCROLL_SCOPE(trace, stage)                                                            \
    ::snow_shot::capture_detail::scrolling_perf::Scope SNOW_SCROLL_CONCAT(scrollScope, __LINE__)(  \
        trace, ::snow_shot::capture_detail::scrolling_perf::Stage::stage)
#define SNOW_SCROLL_TRACE(trace, expression)                                                       \
    do {                                                                                           \
        if (trace) {                                                                               \
            expression;                                                                            \
        }                                                                                          \
    } while (false)
#else
#define SNOW_SCROLL_SCOPE(trace, stage) ((void)0)
#define SNOW_SCROLL_TRACE(trace, expression) ((void)0)
#endif

#if defined(SNOW_SHOT_SCROLLING_PERF_DETAIL)
#define SNOW_SCROLL_DETAIL_SCOPE(stage)                                                            \
    SNOW_SCROLL_SCOPE(::snow_shot::capture_detail::scrolling_perf::currentTrace, stage)
#else
#define SNOW_SCROLL_DETAIL_SCOPE(stage) ((void)0)
#endif

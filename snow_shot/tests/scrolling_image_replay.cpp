#include "scrolling_image_replay.h"

#include <QJsonArray>
#include <QBuffer>
#include <QImageReader>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
#include <thread>

namespace snow_shot::scrolling_benchmark {
namespace {
namespace perf = capture_detail::scrolling_perf;
constexpr qint64 secondNs = 1'000'000'000;
constexpr std::array<const char*, 16> rustStages{"rust.frame_freeze",
                                                 "rust.duplicate_check",
                                                 "rust.reference_preparation",
                                                 "rust.grayscale",
                                                 "rust.similarity_maps",
                                                 "rust.feature_extraction",
                                                 "rust.descriptor_matching",
                                                 "rust.candidate_scoring",
                                                 "rust.refinement",
                                                 "rust.region_update",
                                                 "rust.canvas_composition",
                                                 "rust.reference_synthesis",
                                                 "rust.preview_scaling",
                                                 "rust.push_total",
                                                 "rust.initialization",
                                                 "rust.reserved"};

class ReplaySource final : public ScrollingFrameSource {
  public:
    explicit ReplaySource(ReplayState& state) : m_state(state), m_thread([this]() { produce(); }) {}
    ~ReplaySource() override {
        stop();
    }
    ScrollingSourceEvent receive(int timeoutMilliseconds) override {
        std::unique_lock lock(m_state.mutex);
        m_state.wake.wait_for(lock, std::chrono::milliseconds(timeoutMilliseconds), [this]() {
            return m_state.stopped.load() || !m_state.queue.empty();
        });
        if (m_state.stopped.load() || m_state.queue.empty())
            return {};
        auto event = std::move(m_state.queue.front());
        m_state.queue.pop_front();
        return event;
    }
    void setTargetFps(int fps) override {
        m_state.targetFps.store(std::clamp(fps, 1, 30));
        m_state.wake.notify_all();
    }
    ScrollingSourceStats stats() const override {
        std::lock_guard lock(m_state.mutex);
        return {m_state.captureLatencyNs.load(), static_cast<std::uint32_t>(m_state.queue.size()),
                m_state.dropped.load()};
    }
    void stop() override {
        m_state.stopped.store(true);
        m_state.wake.notify_all();
        if (m_thread.joinable())
            m_thread.join();
        std::lock_guard lock(m_state.mutex);
        for (auto& event : m_state.queue) {
            if (event.frame.trace)
                event.frame.trace->disposition = "source_cancelled";
        }
        m_state.queue.clear();
    }

  private:
    void produce() {
        try {
            sample(perf::now(), 0);
            std::unique_lock lock(m_state.mutex);
            m_state.wake.wait(lock, [this]() {
                return m_state.stopped.load() || m_state.motionStart.load() != 0;
            });
            qint64 pacingWait = 0;
            while (!m_state.stopped.load()) {
                const auto beforeWait = perf::now();
                const auto start = m_state.motionStart.load();
                const int fps = m_state.targetFps.load();
                const auto deadline =
                    start + ReplaySchedule::nextCaptureTime(beforeWait - start, fps);
                const auto point = perf::Clock::time_point(std::chrono::nanoseconds(deadline));
                m_state.wake.wait_until(lock, point, [this, fps]() {
                    return m_state.stopped.load() || m_state.targetFps.load() != fps;
                });
                pacingWait += perf::now() - beforeWait;
                if (m_state.stopped.load())
                    break;
                if (m_state.targetFps.load() != fps)
                    continue;
                lock.unlock();
                sample(deadline, pacingWait);
                pacingWait = 0;
                lock.lock();
            }
        } catch (const std::exception& error) {
            std::lock_guard lock(m_state.mutex);
            ScrollingSourceEvent event;
            event.kind = ScrollingSourceEvent::Kind::Error;
            event.error = QString::fromUtf8(error.what());
            m_state.queue.push_back(std::move(event));
            m_state.wake.notify_all();
        }
    }
    void sample(qint64 deadline, qint64 pacingWait) {
        auto trace = std::make_shared<perf::FrameRecord>();
        trace->id = ++m_sequence;
        trace->capturedAt = perf::now();
        const auto start = m_state.motionStart.load();
        trace->offset = start == 0 ? 0 : m_state.schedule.offsetAt(trace->capturedAt - start);
        trace->motionAt = start == 0 ? trace->capturedAt
                                     : start + m_state.schedule.motionTimeForOffset(trace->offset);
        trace->captureFps = m_state.targetFps.load();
        trace->record(perf::Stage::PacingWait, pacingWait);
        trace->record(perf::Stage::SchedulingLateness,
                      std::max(qint64{0}, trace->capturedAt - deadline));
        ScrollingSourceEvent event;
        event.kind = ScrollingSourceEvent::Kind::Frame;
        event.frame.trace = trace;
        {
            perf::Scope scope(trace, perf::Stage::ViewportPreparation);
            event.frame.image =
                m_state.image.copy(0, trace->offset, m_state.image.width(), m_state.viewportHeight);
            if (event.frame.image.isNull())
                throw std::runtime_error("viewport allocation failed");
        }
        m_state.captureLatencyNs.store(static_cast<quint64>(perf::now() - trace->capturedAt));
        trace->publishedAt = perf::now();
        m_state.publish(std::move(event));
    }
    ReplayState& m_state;
    std::uint64_t m_sequence = 0;
    std::thread m_thread;
};
} // namespace

ReplaySchedule::ReplaySchedule(int sourceHeight, int viewportHeight, int stepPixels,
                               double scrollFps, int maximumSteps)
    : m_step(stepPixels), m_fps(scrollFps) {
    if (viewportHeight <= 0 || sourceHeight < viewportHeight || stepPixels <= 0 ||
        !std::isfinite(scrollFps) || scrollFps <= 0 || maximumSteps < -1) {
        throw std::invalid_argument("invalid scrolling replay dimensions, rate, or step count");
    }
    const auto extent = static_cast<qint64>(sourceHeight) - viewportHeight;
    m_movements = static_cast<int>((extent + stepPixels - 1) / stepPixels);
    if (maximumSteps >= 0)
        m_movements = std::min(m_movements, maximumSteps);
    m_finalOffset =
        static_cast<int>(std::min(extent, static_cast<qint64>(m_movements) * stepPixels));
    const long double duration = static_cast<long double>(m_movements) * secondNs / scrollFps;
    if (duration > static_cast<long double>(std::numeric_limits<qint64>::max() / 4)) {
        throw std::invalid_argument("scrolling replay duration exceeds the clock range");
    }
}
int ReplaySchedule::offsetAt(qint64 elapsedNs) const {
    if (elapsedNs <= 0)
        return 0;
    if (elapsedNs >= durationNs())
        return m_finalOffset;
    const auto steps =
        static_cast<qint64>(std::floor(static_cast<long double>(elapsedNs) * m_fps / secondNs));
    return static_cast<int>(std::min(static_cast<qint64>(m_finalOffset), steps * m_step));
}
qint64 ReplaySchedule::motionTimeForOffset(int offset) const {
    const auto steps =
        (static_cast<qint64>(std::clamp(offset, 0, m_finalOffset)) + m_step - 1) / m_step;
    return static_cast<qint64>(std::ceil(static_cast<long double>(steps) * secondNs / m_fps));
}
qint64 ReplaySchedule::durationNs() const {
    return motionTimeForOffset(m_finalOffset);
}
qint64 ReplaySchedule::nextCaptureTime(qint64 elapsedNs, int fps) {
    const auto index =
        std::floor(static_cast<long double>(std::max(qint64{0}, elapsedNs)) * fps / secondNs) + 1;
    return static_cast<qint64>(std::ceil(index * secondNs / fps));
}
ReplayState::ReplayState(QImage value, int height, int step, double fps, int maximumSteps)
    : image(std::move(value)), viewportHeight(height),
      schedule(image.height(), height, step, fps, maximumSteps) {
    records.reserve(static_cast<std::size_t>(schedule.movements()) + 128U);
}
void ReplayState::startMotion(qint64 nowNs) {
    motionStart.store(nowNs);
    wake.notify_all();
}
qint64 ReplayState::endTime() const {
    return motionStart.load() + schedule.durationNs();
}
ScrollingSourceFactory ReplayState::factory() {
    return [this]() { return std::make_unique<ReplaySource>(*this); };
}

void ReplayState::publish(ScrollingSourceEvent event) {
    std::lock_guard lock(mutex);
    records.push_back(event.frame.trace);
    if (queue.size() == 3) {
        queue.front().frame.trace->disposition = "source_dropped";
        queue.pop_front();
        dropped.fetch_add(1);
    }
    queue.push_back(std::move(event));
    maximumQueueDepth = std::max(maximumQueueDepth, queue.size());
    wake.notify_all();
}

ReplayCompletion replayCompletion(qint64 now, qint64 deadline, bool finalObserved,
                                  bool inputStopped, bool idle) {
    if (now - deadline > 30'000'000'000LL)
        return ReplayCompletion::TimedOut;
    if (finalObserved && inputStopped && idle)
        return ReplayCompletion::Complete;
    return ReplayCompletion::Pending;
}

QJsonObject distribution(std::vector<qint64> samples) {
    if (samples.empty())
        return {{QStringLiteral("count"), 0}};
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&samples](double rank) {
        const auto index =
            static_cast<std::size_t>(std::ceil(rank * static_cast<double>(samples.size()))) - 1;
        return samples[index];
    };
    const double total = std::accumulate(samples.begin(), samples.end(), 0.0);
    return {{QStringLiteral("count"), static_cast<qint64>(samples.size())},
            {QStringLiteral("total_ns"), total},
            {QStringLiteral("mean_ns"), total / static_cast<double>(samples.size())},
            {QStringLiteral("p50_ns"), percentile(0.50)},
            {QStringLiteral("p95_ns"), percentile(0.95)},
            {QStringLiteral("p99_ns"), percentile(0.99)},
            {QStringLiteral("max_ns"), samples.back()}};
}

QJsonObject replayReport(const ReplayState& state, qint64 finishedAt) {
    QJsonArray frames;
    QJsonObject dispositions;
    QJsonObject outcomes;
    std::array<std::vector<qint64>, static_cast<std::size_t>(perf::Stage::Count)> stages;
    std::array<std::vector<qint64>, 16> rust;
    std::array<quint64, 16> calls{};
    std::set<int> sampledOffsets;
    qint64 painted = 0;
    std::size_t mailboxDepth = 0;
    for (const auto& trace : state.records) {
        sampledOffsets.insert(trace->offset);
        mailboxDepth = std::max(mailboxDepth, trace->queueDepth);
        const auto disposition = QString::fromLatin1(trace->disposition);
        dispositions[disposition] = dispositions[disposition].toInteger() + 1;
        if (trace->event >= 0) {
            const auto outcome = QString::number(trace->event);
            outcomes[outcome] = outcomes[outcome].toInteger() + 1;
        }
        if (trace->paintedAt != 0)
            ++painted;
        QJsonObject timings;
        QJsonObject rustCallCounts;
        for (std::size_t i = 0; i < stages.size(); ++i) {
            if (!trace->available[i])
                continue;
            timings[QString::fromLatin1(perf::stageNames[i])] = trace->elapsed[i];
            stages[i].push_back(trace->elapsed[i]);
        }
        for (std::size_t i = 0; i < rust.size(); ++i) {
            if (trace->rustCalls[i] == 0)
                continue;
            timings[QString::fromLatin1(rustStages[i])] =
                static_cast<qint64>(trace->rustElapsed[i]);
            rust[i].push_back(static_cast<qint64>(trace->rustElapsed[i]));
            calls[i] += trace->rustCalls[i];
            rustCallCounts[QString::fromLatin1(rustStages[i])] =
                static_cast<qint64>(trace->rustCalls[i]);
        }
        frames.append(QJsonObject{
            {QStringLiteral("id"), static_cast<qint64>(trace->id)},
            {QStringLiteral("offset_y"), trace->offset},
            {QStringLiteral("disposition"), disposition},
            {QStringLiteral("capture_fps"), trace->captureFps},
            {QStringLiteral("mailbox_depth"), static_cast<qint64>(trace->queueDepth)},
            {QStringLiteral("stitch_event"), trace->event},
            {QStringLiteral("output_height"), trace->outputHeight},
            {QStringLiteral("captured_at_ns"),
             trace->capturedAt - state.records.front()->capturedAt},
            {QStringLiteral("motion_at_ns"), trace->motionAt - state.records.front()->capturedAt},
            {QStringLiteral("painted_at_ns"),
             trace->paintedAt == 0
                 ? QJsonValue(QJsonValue::Null)
                 : QJsonValue(trace->paintedAt - state.records.front()->capturedAt)},
            {QStringLiteral("timings_ns"), timings},
            {QStringLiteral("rust_calls"), rustCallCounts}});
    }
    QJsonObject summaries;
    for (std::size_t i = 0; i < stages.size(); ++i)
        summaries[QString::fromLatin1(perf::stageNames[i])] = distribution(std::move(stages[i]));
    for (std::size_t i = 0; i < rust.size(); ++i) {
        if (rust[i].empty())
            continue;
        auto summary = distribution(std::move(rust[i]));
        summary[QStringLiteral("calls")] = static_cast<qint64>(calls[i]);
        summaries[QString::fromLatin1(rustStages[i])] = summary;
    }
    const auto start = state.motionStart.load();
    const double elapsed = start == 0 ? 0 : static_cast<double>(finishedAt - start) / secondNs;
    return {{QStringLiteral("frames"), frames},
            {QStringLiteral("stages"), summaries},
            {QStringLiteral("dispositions"), dispositions},
            {QStringLiteral("stitch_outcomes"), outcomes},
            {QStringLiteral("source_dropped_frames"), static_cast<qint64>(state.dropped.load())},
            {QStringLiteral("sampled_frames"), static_cast<qint64>(state.records.size())},
            {QStringLiteral("painted_frames"), painted},
            {QStringLiteral("unsampled_positions"),
             state.schedule.movements() + 1 - static_cast<qint64>(sampledOffsets.size())},
            {QStringLiteral("source_queue_peak"), static_cast<qint64>(state.maximumQueueDepth)},
            {QStringLiteral("mailbox_queue_peak"), static_cast<qint64>(mailboxDepth)},
            {QStringLiteral("elapsed_scroll_and_drain_seconds"), elapsed},
            {QStringLiteral("achieved_capture_fps"),
             elapsed > 0 && !state.records.empty()
                 ? static_cast<double>(state.records.size() - 1) / elapsed
                 : 0},
            {QStringLiteral("achieved_thumbnail_fps"),
             elapsed > 0 ? static_cast<double>(std::max(qint64{0}, painted - 1)) / elapsed : 0}};
}

quint64 imageChecksum(const QImage& image) {
    quint64 result = 1469598103934665603ULL;
    for (qsizetype i = 0; i < image.sizeInBytes(); ++i) {
        result ^= image.constBits()[i];
        result *= 1099511628211ULL;
    }
    return result;
}

bool readablePng(const QByteArray& encoded) {
    QBuffer buffer;
    buffer.setData(encoded);
    if (!buffer.open(QIODevice::ReadOnly))
        return false;
    QImageReader reader(&buffer);
    if (!reader.canRead() || reader.format() != QByteArrayLiteral("png") || reader.size().isEmpty())
        return false;
    // A scaled validation read avoids the native codec's fatal malformed-input
    // path without retaining a second full-size image. Timed as initialization.
    const int previousLimit = QImageReader::allocationLimit();
    QImageReader::setAllocationLimit(std::max(previousLimit, 1024));
    reader.setScaledSize(QSize(1, 1));
    const QImage validated = reader.read();
    QImageReader::setAllocationLimit(previousLimit);
    return !validated.isNull() && reader.error() == QImageReader::UnknownError;
}
} // namespace snow_shot::scrolling_benchmark

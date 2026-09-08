#pragma once

#include "presentation/capture/screenshotscrollingpipeline.h"

#include <QJsonObject>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace snow_shot::scrolling_benchmark {
using namespace capture_detail;

class ReplaySchedule final {
  public:
    ReplaySchedule(int sourceHeight, int viewportHeight, int stepPixels, double scrollFps,
                   int maximumSteps = -1);
    [[nodiscard]] int offsetAt(qint64 elapsedNs) const;
    [[nodiscard]] qint64 motionTimeForOffset(int offset) const;
    [[nodiscard]] qint64 durationNs() const;
    [[nodiscard]] int movements() const {
        return m_movements;
    }
    [[nodiscard]] int finalOffset() const {
        return m_finalOffset;
    }
    [[nodiscard]] static qint64 nextCaptureTime(qint64 elapsedNs, int fps);

  private:
    int m_step;
    double m_fps;
    int m_movements;
    int m_finalOffset;
};

struct ReplayState final {
    ReplayState(QImage image, int viewportHeight, int stepPixels, double scrollFps,
                int maximumSteps = -1);
    void startMotion(qint64 nowNs);
    [[nodiscard]] qint64 endTime() const;
    [[nodiscard]] ScrollingSourceFactory factory();
    void publish(ScrollingSourceEvent event);

    QImage image;
    int viewportHeight;
    ReplaySchedule schedule;
    std::atomic<qint64> motionStart{0};
    std::atomic<int> targetFps{30};
    std::atomic<quint64> captureLatencyNs{0};
    std::atomic<quint64> dropped{0};
    std::atomic_bool stopped{false};
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<ScrollingSourceEvent> queue;
    std::vector<scrolling_perf::FrameTrace> records;
    std::size_t maximumQueueDepth = 0;
};

enum class ReplayCompletion { Pending, Complete, TimedOut };
[[nodiscard]] ReplayCompletion replayCompletion(qint64 now, qint64 deadline, bool finalObserved,
                                                bool inputStopped, bool idle);
[[nodiscard]] QJsonObject distribution(std::vector<qint64> samples);
[[nodiscard]] QJsonObject replayReport(const ReplayState& state, qint64 finishedAt);
[[nodiscard]] quint64 imageChecksum(const QImage& image);
[[nodiscard]] bool readablePng(const QByteArray& encoded);
} // namespace snow_shot::scrolling_benchmark

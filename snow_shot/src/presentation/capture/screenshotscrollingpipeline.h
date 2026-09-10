#pragma once

#include "adaptivescrollingcapturecadence.h"
#include "screenshotscrollingperfinstrumentation.h"
#include "snow_shot/presentation/screenshotscrollingsnapshot.h"
#include "snow_shot/presentation/screenshotscrollingtypes.h"

#include <QImage>
#include <QObject>
#include <QRect>
#include <QString>

#include <chrono>
#include <functional>
#include <memory>

namespace snow_shot::capture_detail {
using ScrollClock = std::chrono::steady_clock;

struct ScrollingSourceFrame {
    QImage image;
    bool duplicate = false;
    scrolling_perf::FrameTrace trace;
};

struct ScrollingSourceEvent {
    enum class Kind { Frame, Timeout, Dropped, Error, Ended };
    Kind kind = Kind::Timeout;
    ScrollingSourceFrame frame;
    QString error;
};

struct ScrollingSourceStats {
    std::uint64_t captureLatencyNs = 0;
    std::uint32_t bufferedFrames = 0;
    std::uint64_t droppedFrames = 0;
};

// receive() runs on the consumer thread; control/statistics methods run on the
// producer's Qt thread. stop() must wake a blocked receive().
class ScrollingFrameSource {
  public:
    virtual ~ScrollingFrameSource() = default;
    virtual ScrollingSourceEvent receive(int timeoutMilliseconds) = 0;
    virtual void setTargetFps(int fps) = 0;
    [[nodiscard]] virtual ScrollingSourceStats stats() const = 0;
    virtual void stop() = 0;
};

using ScrollingSourceFactory = std::function<std::unique_ptr<ScrollingFrameSource>()>;
[[nodiscard]] ScrollingSourceFactory
nativeScrollingSource(QRect physicalSelection, bool restoreOriginalColors, quint64 generation = 0);

struct ScrollingPipelineFrame {
    quint64 generation = 0;
    ScrollClock::duration processingDuration{};
    bool changed = false;
    bool fatalError = false;
    int event = -1;
    ScreenshotScrollingStitchChange change = ScreenshotScrollingStitchChange::Replaced;
    int addedRows = 0;
    QImage previewImage;
    QSize sourceSize;
    int replacedPreviewRows = 0;
    bool previewReplaced = false;
    scrolling_perf::FrameTrace trace;
};

class ScreenshotScrollingPipeline final : public QObject {
  public:
    using FrameCallback = std::function<void(ScrollingPipelineFrame)>;
    using ErrorCallback = std::function<void(quint64, QString)>;
    using SnapshotCallback = std::function<void(ScreenshotScrollingSnapshot)>;

    ScreenshotScrollingPipeline(FrameCallback frameCallback, ErrorCallback errorCallback,
                                QObject* parent = nullptr);
    ~ScreenshotScrollingPipeline() override;
    void begin(quint64 generation, QSize viewport, ScreenshotScrollingRecognitionMode mode,
               ScrollingSourceFactory source, AdaptiveScrollingCaptureCadence::Config cadence = {});
    void reset(quint64 generation);
    void pause(quint64 generation);
    void resume(quint64 generation, QSize viewport, ScrollingSourceFactory source,
                AdaptiveScrollingCaptureCadence::Config cadence = {});
    [[nodiscard]] bool idle() const;
    void finishInput(std::function<void()> callback);
    [[nodiscard]] bool requestSnapshot(int top, int bottom, QObject* receiver,
                                       SnapshotCallback callback);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::capture_detail

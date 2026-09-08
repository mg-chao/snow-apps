#include "scrolling_image_replay.h"
#include "snow_shot/presentation/screenshotscrollingthumbnailwidget.h"
#include "snowimageqtcodec.h"
#include "snow_stitch_images.h"

#include <QApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QPainter>
#include <QTemporaryFile>
#include <QTimer>

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace snow_shot::scrolling_benchmark;
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        throw std::runtime_error(message);
    }
}
void scheduleTests() {
    ReplayState queueState(QImage(16, 32, QImage::Format_RGBA8888), 16, 1, 30);
    for (int i = 0; i < 5; ++i) {
        ScrollingSourceEvent event;
        event.kind = ScrollingSourceEvent::Kind::Frame;
        event.frame.trace = std::make_shared<scrolling_perf::FrameRecord>();
        event.frame.trace->id = static_cast<std::uint64_t>(i);
        queueState.publish(std::move(event));
    }
    require(queueState.queue.size() == 3 && queueState.maximumQueueDepth == 3 &&
                queueState.dropped.load() == 2 && queueState.queue.front().frame.trace->id == 2,
            "source overload must discard oldest frames and remain bounded");
    ReplaySchedule full(21944, 1600, 25, 30);
    require(full.movements() == 814 && full.finalOffset() == 20344, "full traversal geometry");
    require(full.offsetAt(33'333'333) == 0 && full.offsetAt(33'333'334) == 25,
            "fractional 30 Hz deadline");
    require(full.offsetAt(1'000'000'000) == 750, "clock based position");
    require(full.offsetAt(full.durationNs() - 1) == 20325, "last full movement");
    require(full.offsetAt(full.durationNs()) == 20344, "last partial movement");
    require(full.offsetAt(full.durationNs() + 1'000'000) == 20344, "hold final position");
    require(full.motionTimeForOffset(20344) == 27'133'333'334LL, "absolute end deadline");
    ReplaySchedule fractional(21944, 1600, 25, 29.97);
    for (int offset = 25; offset < fractional.finalOffset(); offset += 25) {
        const auto deadline = fractional.motionTimeForOffset(offset);
        require(fractional.offsetAt(deadline - 1) == offset - 25 &&
                    fractional.offsetAt(deadline) == offset,
                "fractional scroll periods must not accumulate rounding drift");
    }
    require(ReplaySchedule::nextCaptureTime(1'100'000'000, 2) == 1'500'000'000,
            "adaptive sampling skips past capture deadlines");
    require(ReplaySchedule(1600, 1600, 25, 30).movements() == 0, "single viewport");
    require(ReplaySchedule(21944, 1600, 25, 30, 2).finalOffset() == 50, "bounded smoke replay");
    int rejected = 0;
    for (const auto value : {0, -1}) {
        try {
            ReplaySchedule invalid(1600, 1600, value, 30);
        } catch (const std::invalid_argument&) {
            ++rejected;
        }
    }
    try {
        ReplaySchedule invalid(1599, 1600, 25, 30);
    } catch (const std::invalid_argument&) {
        ++rejected;
    }
    try {
        ReplaySchedule invalid(1600, 1600, 25, 0);
    } catch (const std::invalid_argument&) {
        ++rejected;
    }
    require(rejected == 4, "invalid inputs rejected");
    for (double rate : {std::numeric_limits<double>::quiet_NaN(),
                        std::numeric_limits<double>::infinity(), -1.0}) {
        bool failed = false;
        try {
            ReplaySchedule invalid(1600, 1600, 25, rate);
        } catch (const std::invalid_argument&) {
            failed = true;
        }
        require(failed, "nonfinite and negative rates rejected");
    }
    require(replayCompletion(30'000'000'001LL, 0, false, false, true) == ReplayCompletion::TimedOut,
            "initialization timeout");
    require(replayCompletion(40'000'000'001LL, 10'000'000'000LL, true, true, false) ==
                ReplayCompletion::TimedOut,
            "drain timeout");
    require(replayCompletion(11, 10, true, true, false) == ReplayCompletion::Pending &&
                replayCompletion(11, 10, true, false, true) == ReplayCompletion::Pending &&
                replayCompletion(11, 10, false, true, true) == ReplayCompletion::Pending &&
                replayCompletion(11, 10, true, true, true) == ReplayCompletion::Complete,
            "drain waits for final position, stopped input, and pending work");
    const auto stats = distribution({1, 2, 3, 4, 100});
    require(stats[QStringLiteral("p50_ns")].toInteger() == 3 &&
                stats[QStringLiteral("p95_ns")].toInteger() == 100 &&
                stats[QStringLiteral("total_ns")].toInteger() == 110,
            "timing distribution");
    require(distribution({})[QStringLiteral("count")].toInteger() == 0, "unavailable timing");
    require(distribution({0})[QStringLiteral("count")].toInteger() == 1,
            "measured zero is distinct from unavailable");
    require(!readablePng(QByteArray("not a PNG")), "wrong-format PNG rejected");
    QImage validationImage(32, 32, QImage::Format_RGBA8888);
    validationImage.fill(Qt::red);
    QByteArray png = snow_shot::image_codec::encodePng(validationImage);
    require(readablePng(png), "valid PNG preflight");
    png.truncate(40);
    require(!readablePng(png), "truncated PNG payload rejected");
    require(!readablePng(QByteArray::fromHex("89504e470d0a1a0a")), "truncated PNG header rejected");
    require(snow_shot::image_codec::decodeFile(QStringLiteral("missing-scrolling-fixture.png"),
                                               snow::image::Format::png)
                .isNull(),
            "missing PNG rejected");
}

struct ManualState {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<ScrollingSourceEvent> queue;
    bool stopped = false;
    std::size_t receiveCalls = 0;
    void push(QImage image) {
        ScrollingSourceEvent event;
        event.kind = ScrollingSourceEvent::Kind::Frame;
        event.frame.image = std::move(image);
        std::lock_guard lock(mutex);
        queue.push_back(std::move(event));
        wake.notify_all();
    }
};
class ManualSource final : public ScrollingFrameSource {
  public:
    explicit ManualSource(std::shared_ptr<ManualState> state) : m_state(std::move(state)) {}
    ScrollingSourceEvent receive(int milliseconds) override {
        std::unique_lock lock(m_state->mutex);
        ++m_state->receiveCalls;
        m_state->wake.notify_all();
        m_state->wake.wait_for(lock, std::chrono::milliseconds(milliseconds),
                               [this]() { return m_state->stopped || !m_state->queue.empty(); });
        if (m_state->queue.empty())
            return {};
        auto result = std::move(m_state->queue.front());
        m_state->queue.pop_front();
        return result;
    }
    void setTargetFps(int fps) override {
        require(fps >= 1 && fps <= 30, "capture FPS bounds");
    }
    ScrollingSourceStats stats() const override {
        return {};
    }
    void stop() override {
        std::lock_guard lock(m_state->mutex);
        m_state->stopped = true;
        m_state->wake.notify_all();
    }

  private:
    std::shared_ptr<ManualState> m_state;
};

QImage fixture() {
    QImage image(640, 640, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    QPainter painter(&image);
    quint32 random = 1234567;
    for (int y = 0; y < 640; y += 7) {
        for (int x = 0; x < 640; x += 7) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            painter.fillRect(x, y, 5, 5, QColor::fromRgb(random | 0xff000000U));
        }
    }
    return image;
}

void pipelineTest(ScreenshotScrollingRecognitionMode mode) {
    const bool horizontal = mode == ScreenshotScrollingRecognitionMode::Horizontal;
    const QImage source = fixture();
    const QSize viewport(400, 400);
    const auto frameAt = [&](int offset) {
        return source.copy(horizontal ? offset : 0, horizontal ? 0 : offset, 400, 400);
    };
    QWidget parent;
    ScreenshotScrollingThumbnailWidget thumbnail(parent);
    thumbnail.setRecognitionMode(mode);
    auto state = std::make_shared<ManualState>();
    state->push(frameAt(0));
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(15000);
    int received = 0;
    QString error;
    QImage snapshot;
    QImage expectedTiles;
    std::unique_ptr<ScreenshotScrollingPipeline> pipeline;
    pipeline = std::make_unique<ScreenshotScrollingPipeline>(
        [&](ScrollingPipelineFrame result) {
            require(result.generation == 2, "stale generation delivered");
            require(!result.fatalError, "stitch worker error");
            require(result.changed, "synthetic movement was not accepted");
            const QSize expected(horizontal ? 400 + received * 25 : 400,
                                 horizontal ? 400 : 400 + received * 25);
            require(result.sourceSize == expected, "stitched extent differs from known movement");
            if (received == 0) {
                require(result.previewReplaced && result.replacedPreviewRows == 0,
                        "initial preview must replace the tiles");
                expectedTiles = result.previewImage.copy();
            } else {
                require(!result.previewReplaced && result.replacedPreviewRows > 0,
                        "extended preview must refresh the overlap");
                const int retained = (horizontal ? expectedTiles.width() : expectedTiles.height()) -
                                     result.replacedPreviewRows;
                const int added =
                    horizontal ? result.previewImage.width() : result.previewImage.height();
                QImage joined(horizontal ? retained + added : 128,
                              horizontal ? 128 : retained + added, QImage::Format_RGBA8888);
                QPainter painter(&joined);
                painter.setCompositionMode(QPainter::CompositionMode_Source);
                painter.drawImage(QPoint(0, 0),
                                  expectedTiles.copy(0, 0, horizontal ? retained : 128,
                                                     horizontal ? 128 : retained));
                painter.drawImage(horizontal ? QPoint(retained, 0) : QPoint(0, retained),
                                  result.previewImage);
                painter.end();
                expectedTiles = std::move(joined);
            }
            thumbnail.setStitchedImage(result.previewImage, result.sourceSize, result.change,
                                       result.addedRows, result.previewReplaced,
                                       result.replacedPreviewRows);
            require(thumbnail.previewImageForTesting() == expectedTiles,
                    "preview tiles differ from the accepted edge patches");
            ++received;
            if (received < 3) {
                if (received == 1) {
                    pipeline->pause(2);
                    state = std::make_shared<ManualState>();
                    pipeline->resume(2, viewport,
                                     [state]() { return std::make_unique<ManualSource>(state); });
                }
                state->push(frameAt(received * 25));
            } else {
                require(pipeline->requestSnapshot(0, 450, &loop,
                                                  [&](ScreenshotScrollingSnapshot value) {
                                                      snapshot = value.materialize();
                                                      pipeline->finishInput([&]() {
                                                          require(pipeline->idle(),
                                                                  "final drain is not idle");
                                                          loop.quit();
                                                      });
                                                  }),
                        "snapshot request rejected");
            }
        },
        [&](quint64, QString value) {
            error = std::move(value);
            loop.quit();
        });
    // Reset before dispatching the old producer's events, then start a fresh generation.
    auto stale = std::make_shared<ManualState>();
    stale->push(frameAt(0));
    pipeline->begin(1, viewport, mode, [stale]() { return std::make_unique<ManualSource>(stale); });
    pipeline->reset(2);
    pipeline->begin(2, viewport, mode, [state]() { return std::make_unique<ManualSource>(state); });
    loop.exec();
    pipeline.reset();
    require(error.isEmpty() && received == 3, "pipeline did not complete");
    const auto expected = source.copy(0, 0, horizontal ? 450 : 400, horizontal ? 400 : 450);
    require(snapshot == expected, "snapshot pixel content changed");
    const QSize previewSize(horizontal ? 144 : 128, horizontal ? 128 : 144);
    require(thumbnail.previewImageForTesting().size() == previewSize, "preview scale drift");
    QImage painted(thumbnail.size(), QImage::Format_ARGB32_Premultiplied);
    painted.fill(Qt::transparent);
    const auto blank = imageChecksum(painted);
    thumbnail.render(&painted);
    require(imageChecksum(painted) != blank, "offscreen thumbnail paint is blank");
    require(!parent.isVisible(), "offscreen parent became visible");
}

void overloadTest() {
    auto state = std::make_shared<ManualState>();
    const QImage frame = fixture().copy(0, 0, 400, 400);
    std::vector<scrolling_perf::FrameTrace> records;
    for (int index = 0; index < 8; ++index) {
        ScrollingSourceEvent event;
        event.kind = ScrollingSourceEvent::Kind::Frame;
        event.frame.image = frame;
        event.frame.trace = std::make_shared<scrolling_perf::FrameRecord>();
        event.frame.trace->id = static_cast<std::uint64_t>(index + 1);
        event.frame.trace->publishedAt = scrolling_perf::now();
        records.push_back(event.frame.trace);
        state->queue.push_back(std::move(event));
    }
    QEventLoop loop;
    int delivered = 0;
    QString error;
    ScreenshotScrollingPipeline pipeline(
        [&](ScrollingPipelineFrame result) {
            ++delivered;
            require(result.trace == records[static_cast<std::size_t>(delivered - 1)],
                    "result correlation changed");
            require(!result.fatalError && result.sourceSize == QSize(400, 400),
                    "unchanged frames must retain stitched dimensions");
            require(result.event == (delivered == 1 ? SNOW_STITCH_FRAME_EVENT_INITIAL
                                                    : SNOW_STITCH_FRAME_EVENT_DUPLICATE),
                    "duplicate outcome changed");
            require(
                result.trace->available[static_cast<std::size_t>(scrolling_perf::Stage::Stitch)],
                "early-return stitching duration missing");
#if defined(SNOW_SHOT_SCROLLING_PERF_DETAIL)
            require(result.trace->rustCalls[SNOW_STITCH_PERF_PUSH_TOTAL] == 1,
                    "Rust snapshot did not correlate with this push");
#else
            require(result.trace->rustCalls[13] == 0, "disabled Rust profiling recorded a stage");
#endif
            if (delivered == 2)
                loop.quit();
        },
        [&](quint64, QString value) {
            error = std::move(value);
            loop.quit();
        });
    pipeline.begin(1, QSize(400, 400), ScreenshotScrollingRecognitionMode::Vertical,
                   [state]() { return std::make_unique<ManualSource>(state); });
    // Keep delivery paused until the consumer has attempted every queued input.
    {
        std::unique_lock lock(state->mutex);
        require(state->wake.wait_for(lock, std::chrono::seconds(5),
                                     [&]() { return state->receiveCalls >= 9; }),
                "overload source did not finish admission");
    }
    bool inputStopped = false;
    pipeline.finishInput([&]() { inputStopped = true; });
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
    watchdog.start(5000);
    loop.exec();
    require(error.isEmpty() && delivered == 2 && inputStopped && pipeline.idle(),
            "overloaded pipeline failed to drain");
    for (std::size_t index = 2; index < records.size(); ++index)
        require(std::string(records[index]->disposition) == "mailbox_dropped",
                "bounded mailbox drop was not attributed");
}

void replaySourceTest() {
    const QImage image = fixture();
    ReplayState state(image, 400, 25, 30);
    auto source = state.factory()();
    const auto first = source->receive(5000);
    require(first.kind == ScrollingSourceEvent::Kind::Frame && first.frame.trace->offset == 0 &&
                first.frame.image == image.copy(0, 0, 640, 400),
            "initial replay crop");
    require(source->receive(1).kind == ScrollingSourceEvent::Kind::Timeout,
            "motion must wait for initial thumbnail");
    source->setTargetFps(2);
    state.startMotion(scrolling_perf::now() - state.schedule.durationNs());
    const auto final = source->receive(5000);
    require(final.kind == ScrollingSourceEvent::Kind::Frame && final.frame.trace->offset == 240 &&
                final.frame.trace->captureFps == 2 &&
                final.frame.image == image.copy(0, 240, 640, 400),
            "late adaptive sample must capture the current final position");
    source->stop();
    require(source->receive(1).kind == ScrollingSourceEvent::Kind::Timeout,
            "cancelled source delivered another frame");
    const auto report = replayReport(state, scrolling_perf::now());
    require(report[QStringLiteral("sampled_frames")].toInteger() == 2 &&
                report[QStringLiteral("unsampled_positions")].toInteger() == 9,
            "adaptive skip accounting");
}

void sourceFailureTest() {
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    bool failed = false;
    ScreenshotScrollingPipeline pipeline(
        [](ScrollingPipelineFrame) { require(false, "failed source delivered a frame"); },
        [&](quint64 generation, QString error) {
            failed = generation == 9 && !error.isEmpty();
            loop.quit();
        });
    pipeline.begin(9, QSize(400, 400), ScreenshotScrollingRecognitionMode::Vertical,
                   []() -> std::unique_ptr<ScrollingFrameSource> { return {}; });
    timeout.start(5000);
    loop.exec();
    require(failed, "source initialization failure was not delivered");
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    try {
        scheduleTests();
        std::cerr << "schedule and input validation passed\n";
        pipelineTest(ScreenshotScrollingRecognitionMode::Vertical);
        std::cerr << "vertical pipeline passed\n";
        pipelineTest(ScreenshotScrollingRecognitionMode::Horizontal);
        overloadTest();
        replaySourceTest();
        sourceFailureTest();
        std::cout << "scrolling image replay tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

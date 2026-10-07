#include "scrolling_image_replay.h"
#include "snow_shot/presentation/screenshotscrollingthumbnailwidget.h"
#include "snowimageqtcodec.h"
#include "snow_stitch_images.h"
#include "../src/presentation/capture/scrollingsnapshotrequest.h"

#include <QApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QMouseEvent>
#include <QPainter>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include "snow_shot/diagnostics/diagnostics.h"
#include <condition_variable>
#include <mutex>
#include <QTimer>
#include <QColorSpace>

#include <iostream>
#include <array>
#include <atomic>
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
bool snapshotMatchesSrgbPixels(const QImage& snapshot, QImage expected) {
    expected.setColorSpace(QColorSpace::SRgb);
    return snapshot.colorSpace() == QColorSpace(QColorSpace::SRgb) && snapshot == expected;
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
                    // A release immediately followed by another press must invalidate
                    // the queued resume before it can recreate a capture source.
                    pipeline->resume(2, viewport, []() -> std::unique_ptr<ScrollingFrameSource> {
                        require(false, "superseded resume created a source during dragging");
                        return {};
                    });
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
    require(snapshotMatchesSrgbPixels(snapshot, expected),
            "snapshot pixels or sRGB interpretation changed");
    const QSize previewSize(horizontal ? 144 : 128, horizontal ? 128 : 144);
    require(thumbnail.previewImageForTesting().size() == previewSize, "preview scale drift");
    QImage painted(thumbnail.size(), QImage::Format_ARGB32_Premultiplied);
    painted.fill(Qt::transparent);
    const auto blank = imageChecksum(painted);
    thumbnail.render(&painted);
    require(imageChecksum(painted) != blank, "offscreen thumbnail paint is blank");
    require(!parent.isVisible(), "offscreen parent became visible");
}

void pauseWithDispatchedFramePreservesPreview() {
    auto state = std::make_shared<ManualState>();
    const QImage frame = fixture().copy(0, 0, 400, 400);
    state->push(frame);
    QEventLoop loop;
    QWidget host;
    ScreenshotScrollingThumbnailWidget thumbnail(host);
    int delivered = 0;
    QString error;
    QImage snapshot;
    ScreenshotScrollingPipeline pipeline(
        [&](ScrollingPipelineFrame result) {
            ++delivered;
            thumbnail.setStitchedImage(result.previewImage, result.sourceSize, result.change,
                                       result.addedRows, result.previewReplaced,
                                       result.replacedPreviewRows);
        },
        [&](quint64, QString value) {
            error = value;
            loop.quit();
        });
    pipeline.begin(19, frame.size(), ScreenshotScrollingRecognitionMode::Vertical,
                   [state] { return std::make_unique<ManualSource>(state); });
    {
        std::unique_lock lock(state->mutex);
        require(state->wake.wait_for(lock, std::chrono::seconds(5),
                                     [&] { return state->receiveCalls >= 2; }),
                "source must publish a frame before the pause race");
    }
    // sendPostedEvents processes the capture notification already queued at entry,
    // dispatching the worker. Its newly posted result stays queued until below.
    QCoreApplication::sendPostedEvents(&pipeline, QEvent::MetaCall);
    require(delivered == 0 && !pipeline.idle(), "frame must be in flight at pause");
    bool acknowledged = false;
    pipeline.pause(19, [&] {
        {
            std::lock_guard lock(state->mutex);
            require(state->stopped, "pause acknowledgment must follow source shutdown");
        }
        require(delivered == 1 && pipeline.idle(),
                "pause acknowledgment must follow the committed frame GUI delivery");
        acknowledged = true;
        require(pipeline.requestSnapshot(30, 370, &loop,
                                         [&](ScreenshotScrollingSnapshot value) {
                                             snapshot = value.materialize();
                                             loop.quit();
                                         }),
                "paused snapshot must remain available");
    });
    {
        std::unique_lock lock(state->mutex);
        require(state->wake.wait_for(lock, std::chrono::seconds(5), [&] { return state->stopped; }),
                "pause must stop the native source");
    }
    state->push(fixture().copy(0, 25, 400, 400));
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);
    loop.exec();
    require(error.isEmpty() && acknowledged && delivered == 1 &&
                !thumbnail.previewImageForTesting().isNull(),
            "committed frame must update the preview while paused");
    require(snapshotMatchesSrgbPixels(snapshot, frame.copy(0, 30, 400, 340)),
            "pause must preserve the trimmed result and reject later source frames");
}

void viewportPreviewTest(ScreenshotScrollingRecognitionMode mode) {
    const bool horizontal = mode == ScreenshotScrollingRecognitionMode::Horizontal;
    const QImage frame = fixture();
    auto state = std::make_shared<ManualState>();
    state->push(frame);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QString error;
    bool ready = false;
    ScreenshotScrollingPipeline* target = nullptr;
    ScreenshotScrollingPipeline pipeline(
        [&](ScrollingPipelineFrame result) {
            require(result.changed && !result.fatalError,
                    "viewport fixture must initialize the stitcher");
            target->pause(43, [&] {
                require(target->idle(), "acknowledged hover pause must be idle");
                {
                    std::lock_guard lock(state->mutex);
                    require(state->stopped, "hover pause must join the native source");
                }
                ready = true;
                loop.quit();
            });
        },
        [&](quint64, QString value) {
            error = std::move(value);
            loop.quit();
        });
    target = &pipeline;
    pipeline.begin(43, frame.size(), mode,
                   [state] { return std::make_unique<ManualSource>(state); });
    timeout.start(5000);
    loop.exec();
    require(ready && error.isEmpty(), "viewport fixture did not pause");

    // These 400-pixel windows span unaligned 256-pixel tile boundaries and both image edges.
    for (const int start : {-200, -73, 0, 113, 240, 440, -500, 700}) {
        bool delivered = false;
        QImage preview;
        require(pipeline.requestViewportPreview(start, start + 400, &loop,
                                                [&](QImage value) {
                                                    delivered = true;
                                                    preview = std::move(value);
                                                    loop.quit();
                                                }),
                "valid viewport request must be accepted while paused");
        timeout.start(5000);
        loop.exec();
        QImage expected(horizontal ? QSize(400, 640) : QSize(640, 400), QImage::Format_RGBA8888);
        expected.fill(Qt::black);
        QPainter painter(&expected);
        painter.drawImage(horizontal ? QPoint(-start, 0) : QPoint(0, -start), frame);
        painter.end();
        require(
            delivered && preview == expected && preview.format() == QImage::Format_RGBA8888,
            "viewport extraction must preserve source pixels and pad out-of-bounds parts black");
        require(preview.sizeInBytes() == expected.width() * expected.height() * 4,
                "viewport output allocation must be bounded by the selected range");
    }

    require(!pipeline.requestViewportPreview(2, 1, &loop, [](QImage) {}) &&
                !pipeline.requestViewportPreview(2, 2, &loop, [](QImage) {}) &&
                !pipeline.requestViewportPreview(std::numeric_limits<int>::min(),
                                                 std::numeric_limits<int>::max(), &loop,
                                                 [](QImage) {}) &&
                !pipeline.requestViewportPreview(0, 400, nullptr, [](QImage) {}) &&
                !pipeline.requestViewportPreview(0, 400, &loop, {}),
            "malformed viewport requests must be rejected");
    int destroyedReceiverDeliveries = 0;
    auto receiver = std::make_unique<QObject>();
    require(pipeline.requestViewportPreview(0, 400, receiver.get(),
                                            [&](QImage) { ++destroyedReceiverDeliveries; }),
            "live viewport receiver request must be accepted");
    receiver.reset();
    bool canceledDelivered = false;
    bool pauseAcknowledged = false;
    require(pipeline.requestViewportPreview(
                0, 400, &loop,
                [&](QImage value) {
                    require(value.isNull(),
                            "superseded control request must deliver an empty image");
                    canceledDelivered = true;
                }),
            "cancelable viewport request must be accepted");
    pipeline.pause(43, [&] {
        require(canceledDelivered && destroyedReceiverDeliveries == 0,
                "pause barrier must follow canceled delivery and suppress destroyed receivers");
        pauseAcknowledged = true;
        loop.quit();
    });
    timeout.start(5000);
    loop.exec();
    require(pauseAcknowledged, "second hover pause did not acknowledge");

    bool oldPauseDelivered = false;
    bool resetCompleted = false;
    bool resetImageDelivered = false;
    pipeline.pause(43, [&] { oldPauseDelivered = true; });
    require(pipeline.requestViewportPreview(
                0, 400, &loop,
                [&](QImage value) {
                    require(value.isNull(), "session reset must cancel stale preview pixels");
                    resetImageDelivered = true;
                }),
            "session-cancelable viewport request must be accepted");
    pipeline.reset(44);
    pipeline.pause(44, [&] {
        require(!oldPauseDelivered && resetImageDelivered,
                "new session must cancel old acknowledgment and finish empty viewport delivery");
        resetCompleted = true;
        loop.quit();
    });
    timeout.start(5000);
    loop.exec();
    require(resetCompleted && pipeline.idle(), "session reset pause did not reach quiescence");
}

void snapshotRequestLifetime() {
    ScrollingSnapshotRequest request;
    QObject receiver;
    int completed = 0;
    int cancelled = 0;
    const auto queue = [&]() {
        auto completion =
            request.begin([&](ScreenshotScrollingSnapshot) { ++completed; }, [&] { ++cancelled; });
        require(static_cast<bool>(completion), "snapshot must be accepted");
        QMetaObject::invokeMethod(
            &receiver, [completion] { completion({}); }, Qt::QueuedConnection);
        return completion;
    };
    auto detached = queue();
    require(request.pending(), "cached delivery must register as pending before dispatch");
    require(!request.begin([](ScreenshotScrollingSnapshot) {}),
            "a pending cached delivery must reject a second request");
    request.detach();
    request.cancel(); // capture teardown must not revoke a detached export
    QCoreApplication::sendPostedEvents(&receiver, QEvent::MetaCall);
    require(completed == 1 && cancelled == 0,
            "detached cached delivery must survive capture teardown without cancellation");
    detached({});
    require(completed == 1, "a snapshot completion must run exactly once");

    queue();
    request.cancel();
    request.cancel();
    require(cancelled == 1 && !request.pending(),
            "discarding a pending snapshot must release its consumer exactly once");
    queue(); // a new capture may start before the old completion arrives
    QCoreApplication::sendPostedEvents(&receiver, QEvent::MetaCall);
    require(completed == 2 && cancelled == 1 && !request.pending(),
            "cancel must discard only its own request and permit the next capture");

    auto oldExport = request.begin([&](ScreenshotScrollingSnapshot) { ++completed; });
    request.detach();
    auto nextCapture = request.begin([&](ScreenshotScrollingSnapshot) { ++completed; });
    oldExport({});
    require(completed == 3 && request.pending(),
            "finishing an old export must not release the next capture's pending request");
    request.cancel();
    nextCapture({});
    require(completed == 3, "the next capture still owns cancellation of its request");
}

void snapshotCancellationLifetime() {
    ScrollingSnapshotRequest request;
    int completed = 0;
    int cancelled = 0;
    // A cancelled queued delivery must release the source and captured upload options
    // immediately, even when its completion remains queued in the capture pipeline.
    auto source = std::make_shared<int>(0);
    const std::weak_ptr<int> retained = source;
    auto abandoned = request.begin([source](ScreenshotScrollingSnapshot) {}, [&] { ++cancelled; });
    source.reset();
    require(!retained.expired(), "pending snapshot must retain its consumer");
    request.cancel();
    require(retained.expired() && cancelled == 1,
            "cancellation must release the snapshot consumer before stale delivery");
    abandoned({});
    require(cancelled == 1, "stale snapshot delivery must not notify again");

    ScrollingSnapshotRequest::Completion next;
    auto previous = request.begin(
        [&](ScreenshotScrollingSnapshot) { ++completed; },
        [&] {
            ++cancelled;
            require(!request.pending(), "cancel must release ownership before notifying");
            next = request.begin([&](ScreenshotScrollingSnapshot) { ++completed; });
        });
    request.cancel();
    require(next && request.pending(), "a cancelled consumer may request another snapshot");
    previous({});
    require(completed == 0 && request.pending(), "stale delivery must preserve a newer request");
    next({});
    request.cancel();
    require(completed == 1 && cancelled == 2,
            "successful delivery must suppress subsequent cancellation notification");

    ScrollingSnapshotRequest::Completion detached;
    {
        ScrollingSnapshotRequest capture;
        detached =
            capture.begin([&](ScreenshotScrollingSnapshot) { ++completed; }, [&] { ++cancelled; });
        capture.detach();
        auto pending =
            capture.begin([&](ScreenshotScrollingSnapshot) { ++completed; }, [&] { ++cancelled; });
        require(static_cast<bool>(pending), "detached export must release capture ownership");
    }
    require(cancelled == 3, "capture destruction must notify the remaining pending consumer");
    detached({});
    require(completed == 2 && cancelled == 3,
            "capture destruction must preserve a detached export's completion");
}

void acceptedSnapshotsSurviveTeardown() {
    for (const auto mode : {ScreenshotScrollingRecognitionMode::Vertical,
                            ScreenshotScrollingRecognitionMode::Horizontal}) {
        auto state = std::make_shared<ManualState>();
        const QImage frame = fixture().copy(0, 0, 400, 400);
        state->push(frame);
        QEventLoop loop;
        bool ready = false;
        auto pipeline = std::make_unique<ScreenshotScrollingPipeline>(
            [&](ScrollingPipelineFrame result) {
                ready = result.changed && !result.fatalError;
                loop.quit();
            },
            [&](quint64, QString) { loop.quit(); });
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(5000);
        pipeline->begin(1, frame.size(), mode,
                        [state] { return std::make_unique<ManualSource>(state); });
        loop.exec();
        require(ready, "snapshot teardown fixture must have a stitched frame");
        int completed = 0;
        constexpr int requests = 256;
        for (int index = 0; index < requests; ++index) {
            require(pipeline->requestSnapshot(
                        30, 370, &loop,
                        [&](ScreenshotScrollingSnapshot snapshot) {
                            const auto expected =
                                mode == ScreenshotScrollingRecognitionMode::Horizontal
                                    ? frame.copy(30, 0, 340, 400)
                                    : frame.copy(0, 30, 400, 340);
                            require(snapshotMatchesSrgbPixels(snapshot.materialize(), expected),
                                    "detached snapshot must retain its pixels");
                            ++completed;
                        }),
                    "snapshot request must be accepted before teardown");
        }
        // Match export detachment: reset and destroy the workers before the UI
        // dispatches any result. Accepted snapshots belong to their receivers.
        pipeline->reset(2);
        pipeline.reset();
        QCoreApplication::sendPostedEvents(&loop, QEvent::MetaCall);
        require(completed == requests,
                "teardown must deliver every accepted snapshot exactly once");
    }
}

void nativeFrameLeasesRemainBoundedAndSurviveStitching() {
    auto state = std::make_shared<ManualState>();
    std::array<std::atomic_bool, 8> released{};
    QImage pixels = fixture().copy(0, 0, 400, 400);
    // Match the native source's external-buffer image ownership.
    for (auto& flag : released) {
        QImage frame(
            pixels.constBits(), pixels.width(), pixels.height(), pixels.bytesPerLine(),
            pixels.format(),
            [](void* value) { static_cast<std::atomic_bool*>(value)->store(true); }, &flag);
        state->push(std::move(frame));
    }
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    ScreenshotScrollingSnapshot snapshot;
    std::unique_ptr<ScreenshotScrollingPipeline> pipeline;
    pipeline = std::make_unique<ScreenshotScrollingPipeline>(
        [&](ScrollingPipelineFrame result) {
            require(!result.fatalError && result.changed, "native input must stitch successfully");
            require(!released.back().load(), "stitching must retain its immutable native input");
            require(!result.trace ||
                        !result.trace
                             ->available[static_cast<std::size_t>(scrolling_perf::Stage::RgbaCopy)],
                    "native input admission must not copy RGBA pixels");
            require(pipeline->requestSnapshot(0, 400, &loop,
                                              [&](ScreenshotScrollingSnapshot value) {
                                                  snapshot = std::move(value);
                                                  loop.quit();
                                              }),
                    "native snapshot request must be accepted");
        },
        [&](quint64, QString) { require(false, "native input source failed"); });
    pipeline->begin(1, pixels.size(), ScreenshotScrollingRecognitionMode::Vertical,
                    [state]() { return std::make_unique<ManualSource>(state); });
    {
        std::unique_lock lock(state->mutex);
        require(state->wake.wait_for(lock, std::chrono::seconds(5),
                                     [&]() { return state->receiveCalls >= 9; }),
                "capture must finish admitting every native frame");
    }
    for (std::size_t index = 0; index + 1 < released.size(); ++index)
        require(released[index].load(),
                "mailbox replacement must release superseded native inputs");
    require(!released.back().load(), "the newest mailbox input must keep its pixels alive");
    timeout.start(5000);
    loop.exec();
    require(snapshot.isValid(), "native input must produce a snapshot");
    pipeline.reset();
    require(released.back().load(), "teardown must release the final native input lease");
    require(snapshotMatchesSrgbPixels(snapshot.materialize(), pixels),
            "canvas snapshot must remain independent of released native inputs");
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
    bool inputStopped = false;
    QString error;
    ScreenshotScrollingPipeline pipeline(
        [&](ScrollingPipelineFrame result) {
            ++delivered;
            require(result.trace == records.back(), "overload must process the newest frame");
            require(!result.fatalError && result.sourceSize == QSize(400, 400),
                    "unchanged frames must retain stitched dimensions");
            require(result.event == SNOW_STITCH_FRAME_EVENT_INITIAL,
                    "the newest frame must initialize stitching");
            require(
                result.trace->available[static_cast<std::size_t>(scrolling_perf::Stage::Stitch)],
                "early-return stitching duration missing");
            require(
                !result.trace
                        ->available[static_cast<std::size_t>(scrolling_perf::Stage::RgbaCopy)] &&
                    !result.trace
                         ->available[static_cast<std::size_t>(scrolling_perf::Stage::PoolAcquire)],
                "mailbox inputs must reach stitching without an intermediate pool or pixel copy");
#if defined(SNOW_SHOT_SCROLLING_PERF_DETAIL)
            require(result.trace->rustCalls[SNOW_STITCH_PERF_PUSH_TOTAL] == 1,
                    "Rust snapshot did not correlate with this push");
#else
            require(result.trace->rustCalls[13] == 0, "disabled Rust profiling recorded a stage");
#endif
            if (inputStopped)
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
    pipeline.finishInput([&]() {
        inputStopped = true;
        if (delivered == 1)
            loop.quit();
    });
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
    watchdog.start(5000);
    loop.exec();
    require(error.isEmpty() && delivered == 1 && inputStopped && pipeline.idle(),
            "overloaded pipeline failed to drain");
    for (std::size_t index = 0; index + 1 < records.size(); ++index)
        require(std::string(records[index]->disposition) == "mailbox_replaced",
                "superseded pending frame must be attributed as replaced");
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

void interruptedThumbnailDragTest() {
    for (const auto mode : {ScreenshotScrollingRecognitionMode::Vertical,
                            ScreenshotScrollingRecognitionMode::Horizontal}) {
        for (const auto interruption :
             {QEvent::Hide, QEvent::WindowDeactivate, QEvent::UngrabMouse, QEvent::MouseMove}) {
            QWidget parent;
            ScreenshotScrollingThumbnailWidget thumbnail(parent);
            thumbnail.setRecognitionMode(mode);
            const bool horizontal = mode == ScreenshotScrollingRecognitionMode::Horizontal;
            const QSize size = horizontal ? QSize(256, 128) : QSize(128, 256);
            QImage image(size, QImage::Format_RGBA8888);
            image.fill(Qt::white);
            thumbnail.setStitchedImage(image, size, ScreenshotScrollingStitchChange::Initial, 0);
            parent.show();
            QApplication::processEvents();
            const auto sendMouse = [&](QEvent::Type type, int position, Qt::MouseButton button,
                                       Qt::MouseButtons buttons) {
                const QPointF point = horizontal ? QPointF(position, 64) : QPointF(64, position);
                QMouseEvent event(type, point, thumbnail.mapToGlobal(point.toPoint()), button,
                                  buttons, Qt::NoModifier);
                QApplication::sendEvent(&thumbnail, &event);
            };
            sendMouse(QEvent::MouseButtonPress, 0, Qt::LeftButton, Qt::LeftButton);
            sendMouse(QEvent::MouseMove, 30, Qt::NoButton, Qt::LeftButton);
            require(thumbnail.trimTop() == 30, "trim handle must respond to an active drag");
            if (interruption == QEvent::Hide) {
                thumbnail.hide();
                require(QWidget::mouseGrabber() != &thumbnail,
                        "a hidden thumbnail must immediately release mouse capture");
                thumbnail.show();
            } else if (interruption != QEvent::MouseMove) {
                QEvent event(interruption);
                QApplication::sendEvent(&thumbnail, &event);
            }
            sendMouse(QEvent::MouseMove, 90, Qt::NoButton, Qt::NoButton);
            require(thumbnail.trimTop() == 30,
                    "interrupted drags must not crop more content on subsequent mouse movement");
            sendMouse(QEvent::MouseButtonPress, 30, Qt::LeftButton, Qt::LeftButton);
            sendMouse(QEvent::MouseMove, 50, Qt::NoButton, Qt::LeftButton);
            sendMouse(QEvent::MouseButtonRelease, 50, Qt::LeftButton, Qt::NoButton);
            require(thumbnail.trimTop() == 50 && QWidget::mouseGrabber() != &thumbnail,
                    "a fresh trim drag must work after interruption");
        }
    }
}

void pipelineDiagnosticsTest() {
    using namespace snow_shot::diagnostics;
    class Source final : public ScrollingFrameSource {
      public:
        ScrollingSourceEvent receive(int timeoutMilliseconds) override {
            ScrollingSourceEvent event;
            if (step == 0) {
                ++step;
                return event;
            }
            if (step <= 3) {
                event.kind = ScrollingSourceEvent::Kind::Frame;
                event.frame.image =
                    QImage(32, 32, step == 1 ? QImage::Format_RGB32 : QImage::Format_RGBA8888);
                event.frame.image.fill(Qt::white);
                event.frame.duplicate = step == 2;
                ++step;
                return event;
            }
            std::unique_lock lock(mutex);
            wake.wait_for(lock, std::chrono::milliseconds(timeoutMilliseconds),
                          [this] { return stopped; });
            return event;
        }
        void setTargetFps(int) override {}
        ScrollingSourceStats stats() const override {
            return {};
        }
        void stop() override {
            std::lock_guard lock(mutex);
            stopped = true;
            wake.notify_all();
        }

      private:
        int step = 0;
        bool stopped = false;
        std::mutex mutex;
        std::condition_variable wake;
    };
    QTemporaryDir directory;
    DiagnosticsOptions options;
    options.directories = {QDir(directory.path()).canonicalPath()};
    options.enableCrashCapture = false;
    options.installMessageHandler = false;
    options.mirrorToConsole = false;
    auto& service = DiagnosticsService::instance();
    require(service.initialize(options), "initialize pipeline diagnostics");
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    bool preview = false;
    bool failed = false;
    {
        ScreenshotScrollingPipeline pipeline(
            [&](ScrollingPipelineFrame frame) {
                preview = frame.changed && !frame.previewImage.isNull();
                loop.quit();
            },
            [&](quint64, QString) {
                failed = true;
                loop.quit();
            });
        pipeline.begin(71, QSize(32, 32), ScreenshotScrollingRecognitionMode::Vertical,
                       [] { return std::make_unique<Source>(); });
        timeout.start(5000);
        loop.exec();
    }
    require(preview && !failed,
            "diagnostics must preserve first preview delivery after rejected frames");
    require(service.flush(), "pipeline diagnostics flush");
    QFile file(service.status().currentFile);
    require(file.open(QIODevice::ReadOnly), "read pipeline diagnostics");
    QJsonObject summary;
    int rejected = 0;
    int firstFrames = 0;
    for (const auto& line : file.readAll().split('\n')) {
        const auto record = QJsonDocument::fromJson(line).object();
        const auto fields = record.value(QStringLiteral("fields")).toObject();
        if (fields.value(QStringLiteral("operation")) != QStringLiteral("71"))
            continue;
        const auto event = record.value(QStringLiteral("event")).toString();
        if (event == QStringLiteral("scrolling.capture_summary"))
            summary = fields;
        if (event == QStringLiteral("scrolling.frame_rejected"))
            ++rejected;
        if (event == QStringLiteral("scrolling.first_frame"))
            ++firstFrames;
    }
    require(summary.value(QStringLiteral("frames_received")).toInt() == 3 &&
                summary.value(QStringLiteral("frames_accepted")).toInt() == 1 &&
                summary.value(QStringLiteral("invalid_frames")).toInt() == 1 &&
                summary.value(QStringLiteral("duplicate_frames")).toInt() == 1 &&
                summary.value(QStringLiteral("receive_timeouts")).toInt() >= 1,
            "capture summary must distinguish invalid, duplicate, accepted and missing frames");
    require(rejected == 1 && firstFrames == 1, "frame milestones must be emitted once per stream");
    file.close();
    service.shutdown();
}

#ifdef Q_OS_MACOS
void viewportReconfigurationTest() {
    for (auto mode : {ScreenshotScrollingRecognitionMode::Vertical,
                      ScreenshotScrollingRecognitionMode::Horizontal}) {
        auto source = std::make_shared<ManualState>();
        source->push(fixture().copy(0, 0, 200, 200));
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        bool failed = false;
        ScreenshotScrollingPipeline pipeline(
            [](ScrollingPipelineFrame) {
                require(false, "changed-scale frame reached the stitcher");
            },
            [&](quint64 generation, QString error) {
                failed = generation == 7 && !error.isEmpty();
                loop.quit();
            });
        pipeline.begin(7, QSize(400, 400), mode,
                       [source] { return std::make_unique<ManualSource>(source); });
        timeout.start(5000);
        loop.exec();
        require(failed, "scale-only reconfiguration must stop the scrolling session");
    }
}
#endif

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
        if (application.arguments().contains(QStringLiteral("--thumbnail-drag-only"))) {
            interruptedThumbnailDragTest();
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--snapshot-teardown-only"))) {
            snapshotRequestLifetime();
            snapshotCancellationLifetime();
            acceptedSnapshotsSurviveTeardown();
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--hover-preview-only"))) {
            pauseWithDispatchedFramePreservesPreview();
            viewportPreviewTest(ScreenshotScrollingRecognitionMode::Vertical);
            viewportPreviewTest(ScreenshotScrollingRecognitionMode::Horizontal);
            return 0;
        }
        interruptedThumbnailDragTest();
        scheduleTests();
        std::cerr << "schedule and input validation passed\n";
        pipelineTest(ScreenshotScrollingRecognitionMode::Vertical);
        std::cerr << "vertical pipeline passed\n";
        pipelineTest(ScreenshotScrollingRecognitionMode::Horizontal);
        pauseWithDispatchedFramePreservesPreview();
        viewportPreviewTest(ScreenshotScrollingRecognitionMode::Vertical);
        viewportPreviewTest(ScreenshotScrollingRecognitionMode::Horizontal);
        snapshotRequestLifetime();
        snapshotCancellationLifetime();
        acceptedSnapshotsSurviveTeardown();
        nativeFrameLeasesRemainBoundedAndSurviveStitching();
        overloadTest();
        replaySourceTest();
        sourceFailureTest();
#ifdef Q_OS_MACOS
        viewportReconfigurationTest();
#endif
        pipelineDiagnosticsTest();
        std::cout << "scrolling image replay tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

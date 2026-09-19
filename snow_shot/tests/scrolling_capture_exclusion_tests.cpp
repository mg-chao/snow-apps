#include "presentation/capture/screenshotscrollingpipeline.h"
#include "presentation/capture/windowcaptureexclusion.h"
#include "snow_capture.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <vector>

struct SnowCaptureStreamImpl {
    std::mutex mutex;
    std::condition_variable changed;
    bool stopped = false;
};
namespace {
using namespace snow_shot::capture_detail;
using snow_shot::presentation::WindowCaptureExclusion;
std::mutex recordsMutex;
std::vector<QVector<std::uint32_t>> capturedIds;
std::atomic<int> liveStreams = 0;
std::atomic<bool> failCreate = false;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
template <class Predicate> void until(Predicate ready) {
    QElapsedTimer deadline;
    deadline.start();
    while (!ready() && deadline.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    require(ready(), "scrolling worker did not complete in time");
}
std::size_t creates() {
    std::lock_guard lock(recordsMutex);
    return capturedIds.size();
}
void streamRecreationPreservesSuccessfulExclusions(bool overlaySucceeds, bool toolbarSucceeds) {
    QWidget overlay, toolbar;
    overlay.show();
    toolbar.show();
    std::vector<QWidget*> restored;
    const auto baseline = creates();
    {
        WindowCaptureExclusion exclusion([&](QWidget* widget, bool excluded) {
            if (!excluded) {
                require(liveStreams == 0, "restore must happen after native streams are destroyed");
                restored.push_back(widget);
            }
            return widget == &overlay ? overlaySucceeds : toolbarSucceeds;
        });
        exclusion.exclude(&overlay);
        exclusion.exclude(&toolbar);
        const auto ids = exclusion.windowIds([&](QWidget* widget) {
            return std::optional<std::uint32_t>(widget == &overlay ? 17 : 23);
        });
        // The factory must own its arrays while queued on the capture thread.
        auto temporary = ids;
        auto source = nativeScrollingSource(QRect(0, 0, 64, 64), false, temporary);
        temporary.fill(999);
        temporary.clear();
        int errors = 0;
        ScreenshotScrollingPipeline pipeline({}, [&](quint64, QString) { ++errors; });
        pipeline.begin(1, QSize(64, 64), ScreenshotScrollingRecognitionMode::Vertical, source);
        until([&] { return creates() == baseline + 1 && liveStreams == 1; });
        pipeline.begin(2, QSize(64, 64), ScreenshotScrollingRecognitionMode::Horizontal,
                       nativeScrollingSource(QRect(0, 0, 64, 64), false, ids));
        until([&] { return creates() == baseline + 2 && liveStreams == 1; });
        pipeline.pause(2);
        until([] { return liveStreams == 0; });
        require(restored.empty(), "export pause must retain sharing policies");
        pipeline.resume(2, QSize(64, 64), nativeScrollingSource(QRect(0, 0, 64, 64), false, ids));
        until([&] { return creates() == baseline + 3 && liveStreams == 1; });
        require(overlay.isVisible() && toolbar.isVisible(), "exclusion never hides capture UI");
        {
            std::lock_guard lock(recordsMutex);
            for (auto index = baseline; index < capturedIds.size(); ++index)
                require(capturedIds[index] == ids, "every recreated stream retains successful IDs");
        }
        failCreate = true;
        pipeline.begin(3, QSize(64, 64), ScreenshotScrollingRecognitionMode::Vertical, source);
        until([&] { return errors == 1; });
        require(liveStreams == 0, "failed replacement releases the previous native stream");
        failCreate = false;
        // Start again so destructor cleanup must stop a live receive operation.
        pipeline.begin(4, QSize(64, 64), ScreenshotScrollingRecognitionMode::Vertical, source);
        until([&] { return creates() == baseline + 5 && liveStreams == 1; });
    }
    std::vector<QWidget*> expected;
    if (toolbarSucceeds)
        expected.push_back(&toolbar);
    if (overlaySucceeds)
        expected.push_back(&overlay);
    require(restored == expected, "teardown restores successful windows in reverse order");
    require(liveStreams == 0, "teardown joins every stream");
}
} // namespace

extern "C" {
SnowCaptureStream* snow_capture_stream_create_region(const SnowCaptureStreamConfig* config) {
    require(config->version == SNOW_CAPTURE_STREAM_CONFIG_VERSION &&
                config->struct_size == sizeof(*config),
            "scrolling source must use the current capture ABI");
    QVector<std::uint32_t> ids;
    for (size_t i = 0; i < config->exclusions.window_count; ++i)
        ids.push_back(config->exclusions.windows[i]);
    {
        std::lock_guard lock(recordsMutex);
        capturedIds.push_back(ids);
    }
    if (failCreate)
        return nullptr;
    ++liveStreams;
    return new SnowCaptureStream;
}
uint8_t snow_capture_stream_receive(SnowCaptureStream* stream, uint32_t timeout,
                                    SnowCaptureStreamEvent* event) {
    std::unique_lock lock(stream->mutex);
    stream->changed.wait_for(lock, std::chrono::milliseconds(timeout),
                             [&] { return stream->stopped; });
    *event = {};
    event->kind =
        stream->stopped ? SNOW_CAPTURE_STREAM_EVENT_ENDED : SNOW_CAPTURE_STREAM_EVENT_TIMEOUT;
    return 1;
}
uint8_t snow_capture_stream_stop(SnowCaptureStream* stream) {
    std::lock_guard lock(stream->mutex);
    stream->stopped = true;
    stream->changed.notify_all();
    return 1;
}
void snow_capture_stream_destroy(SnowCaptureStream* stream) {
    delete stream;
    --liveStreams;
}
uint8_t snow_capture_stream_stats(const SnowCaptureStream*, SnowCaptureStreamStats* stats) {
    *stats = {};
    return 1;
}
uint8_t snow_capture_stream_set_target_fps(SnowCaptureStream*, uint32_t) {
    return 1;
}
uint8_t snow_capture_stream_frame_info(const SnowCaptureStreamFrame*, SnowCaptureStreamFrameInfo*) {
    return 0;
}
void snow_capture_stream_frame_release(SnowCaptureStreamFrame*) {}
const char* snow_capture_last_error_message() {
    return "injected stream creation failure";
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    for (bool overlay : {false, true})
        for (bool toolbar : {false, true})
            streamRecreationPreservesSuccessfulExclusions(overlay, toolbar);
    return 0;
}

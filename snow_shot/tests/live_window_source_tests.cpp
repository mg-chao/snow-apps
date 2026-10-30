#include "snow_shot/presentation/livewindowsource.h"
#include "../src/presentation/pinned/livewindownative_p.h"
#include "snow_capture.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QThread>
#include <QTranslator>
#include <QWidget>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

using snow_shot::presentation::LiveWindowState;
namespace {
std::atomic<LiveWindowState> sourceState{LiveWindowState::Live};
std::atomic<int> activeStreams{0};
std::atomic<int> createdStreams{0};
std::atomic<int> liveFrames{0};
std::atomic<bool> captureFails{false};
std::atomic<bool> blockCapture{false};
std::atomic<bool> captureBlocked{false};
std::atomic<bool> cleanupEntered{false};
std::atomic<bool> cleanupReleased{false};
std::atomic<int> stopRequests{0};
std::mutex backendMutex;
std::condition_variable backendChanged;
std::atomic<int> sourceWidth{100};
std::atomic<int> sourceHeight{80};
int activeMonitors = 0;
int createdMonitors = 0;
bool nativeFramesEnabled = false;
bool nativePresentationFails = false;
std::atomic<int> nativeReadbacks{0};
std::atomic<bool> nativeReadbackFails{false};
int nativePresentations = 0;
std::function<void()> notifyNativeState;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
template <typename Predicate> bool waitFor(Predicate predicate, int timeout = 4000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    QApplication::processEvents();
    return predicate();
}

void stateMessagesFollowLanguageChanges(snow_shot::presentation::LiveWindowSource& source,
                                        const char* expectedSourceText) {
    class Translator final : public QTranslator {
      public:
        explicit Translator(QString prefix) : m_prefix(std::move(prefix)) {}
        bool isEmpty() const override {
            return false;
        }
        QString translate(const char* context, const char* text, const char*, int) const override {
            return QByteArray(context) == "LiveWindowSource" ? m_prefix + QString::fromUtf8(text)
                                                             : QString();
        }

      private:
        const QString m_prefix;
    };
    const auto before = source.state();
    const bool nativePresentation = source.usesNativePresentation();
    for (const auto& prefix : {QStringLiteral("first-locale:"), QStringLiteral("next-locale:")}) {
        Translator translator(prefix);
        require(QCoreApplication::installTranslator(&translator),
                "Install the live-source translator.");
        QEvent languageChange(QEvent::LanguageChange);
        QCoreApplication::sendEvent(&source, &languageChange);
        require(source.stateMessage() == prefix + QString::fromUtf8(expectedSourceText),
                "Cached source status messages must use the current locale.");
        require(source.state() == before && source.usesNativePresentation() == nativePresentation,
                "Retranslation preserves capture state and native presentation.");
        QCoreApplication::removeTranslator(&translator);
    }
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&source, &languageChange);
    require(source.stateMessage() == QString::fromUtf8(expectedSourceText),
            "Removing the translator restores the source-language status message.");
}
} // namespace

struct SnowCaptureStreamImpl {
    std::atomic<bool> stopped{false};
};
struct SnowCaptureStreamFrameImpl {
    std::vector<uint8_t> bytes;
    int width = 0;
    int height = 0;
};

extern "C" {
SnowCaptureStream* snow_capture_stream_create_window(const SnowCaptureWindowStreamConfig* config) {
    require(config && config->window_id == 42 && config->target_fps == 30 &&
                config->buffer_depth == 1,
            "The broker uses native window capture at the specified bounded cadence.");
    ++activeStreams;
    ++createdStreams;
    return new SnowCaptureStream;
}
void snow_capture_stream_destroy(SnowCaptureStream* stream) {
    if (blockCapture) {
        cleanupEntered = true;
        std::unique_lock lock(backendMutex);
        require(backendChanged.wait_for(lock, std::chrono::seconds(4),
                                        [] { return cleanupReleased.load(); }),
                "GUI shutdown must return before blocked native cleanup is released.");
    }
    --activeStreams;
    delete stream;
}
uint8_t snow_capture_stream_stop(SnowCaptureStream* stream) {
    {
        std::lock_guard lock(backendMutex);
        stream->stopped = true;
        ++stopRequests;
    }
    backendChanged.notify_all();
    return 1;
}
uint8_t snow_capture_stream_receive(SnowCaptureStream* stream, uint32_t,
                                    SnowCaptureStreamEvent* event) {
    if (blockCapture) {
        captureBlocked = true;
        std::unique_lock lock(backendMutex);
        require(backendChanged.wait_for(lock, std::chrono::seconds(4),
                                        [&] { return stream->stopped.load(); }),
                "The final capture consumer promptly cancels the native stream.");
        *event = {};
        event->kind = SNOW_CAPTURE_STREAM_EVENT_ENDED;
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    *event = {};
    if (captureFails) {
        event->kind = SNOW_CAPTURE_STREAM_EVENT_ERROR;
        return 1;
    }
    auto* frame = new SnowCaptureStreamFrame;
    frame->width = sourceWidth;
    frame->height = sourceHeight;
    frame->bytes.resize(static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height) * 4);
    for (size_t index = 0; index < frame->bytes.size(); index += 4) {
        frame->bytes[index] = 40;
        frame->bytes[index + 1] = 120;
        frame->bytes[index + 2] = 200;
        frame->bytes[index + 3] = 255;
    }
    event->kind = SNOW_CAPTURE_STREAM_EVENT_FRAME;
    event->frame = frame;
    ++liveFrames;
    return 1;
}
uint8_t snow_capture_stream_frame_info(const SnowCaptureStreamFrame* frame,
                                       SnowCaptureStreamFrameInfo* info) {
    *info = {};
    info->width = static_cast<uint32_t>(frame->width);
    info->height = static_cast<uint32_t>(frame->height);
    info->stride_bytes = info->width * 4;
    info->rgba_bytes = frame->bytes.data();
    info->rgba_len = frame->bytes.size();
    return 1;
}
uint8_t snow_capture_stream_frame_window_geometry(const SnowCaptureStreamFrame* frame,
                                                  SnowCaptureWindowGeometry* geometry) {
    *geometry = {};
    geometry->x = 100;
    geometry->y = 200;
    geometry->width = frame->width;
    geometry->height = frame->height;
    return 1;
}
void snow_capture_stream_frame_release(SnowCaptureStreamFrame* frame) {
    delete frame;
}
}

namespace snow_shot::presentation::live_window_detail {
class FakeNativeFrame final : public NativeWindowFrame {
  public:
    explicit FakeNativeFrame(SnowCaptureStreamFrame* frame) : m_frame(frame) {}
    ~FakeNativeFrame() override {
        snow_capture_stream_frame_release(m_frame);
    }
    QSize size() const override {
        return QSize(m_frame->width, m_frame->height);
    }
    QImage image() const override {
        ++nativeReadbacks;
        if (nativeReadbackFails)
            return {};
        return QImage(m_frame->bytes.data(), m_frame->width, m_frame->height,
                      QImage::Format_RGBA8888)
            .copy();
    }
    const void* pixelBuffer() const override {
        return this;
    }

  private:
    SnowCaptureStreamFrame* m_frame;
};
class FakeNativeCapture final : public NativeWindowCapture {
  public:
    FakeNativeCapture() {
        SnowCaptureWindowStreamConfig config{};
        config.window_id = 42;
        config.target_fps = 30;
        config.buffer_depth = 1;
        m_stream = snow_capture_stream_create_window(&config);
    }
    ~FakeNativeCapture() override {
        snow_capture_stream_destroy(m_stream);
    }
    void cancel() override {
        snow_capture_stream_stop(m_stream);
    }
    bool receive(NativeCaptureFrame* output) override {
        SnowCaptureStreamEvent event{};
        if (!snow_capture_stream_receive(m_stream, 50, &event) ||
            event.kind != SNOW_CAPTURE_STREAM_EVENT_FRAME)
            return false;
        output->pixels = std::make_shared<FakeNativeFrame>(event.frame);
        output->desktopBounds = QRectF(100, 200, event.frame->width, event.frame->height);
        return true;
    }

  private:
    SnowCaptureStream* m_stream;
};
std::unique_ptr<NativeWindowCapture> createNativeWindowCapture(const LiveWindowTarget&) {
    if (nativeFramesEnabled)
        return std::make_unique<FakeNativeCapture>();
    return {};
}
class FakeNativeMonitor final : public NativeWindowMonitor {
  public:
    FakeNativeMonitor() {
        ++activeMonitors;
        ++createdMonitors;
    }
    ~FakeNativeMonitor() override {
        --activeMonitors;
        notifyNativeState = {};
    }
    bool initialize(QString*) override {
        notifyNativeState = [this] {
            if (changed)
                changed();
        };
        return true;
    }
    NativeState inspect() override {
        const auto state = sourceState.load();
        return {state, QRect(100, 200, sourceWidth, sourceHeight),
                state == LiveWindowState::Unavailable
                    ? QCoreApplication::translate("LiveWindowSource",
                                                  "The selected window is no longer available.")
                    : QString()};
    }
};
class FakeNativePresentation final : public NativeWindowPresentation {
  public:
    bool consumesFrames() const override {
        return nativeFramesEnabled;
    }
    bool supportsTransform(const QTransform& transform) const override {
        return nativeFramesEnabled || transform.isIdentity();
    }
    bool presentFrame(std::shared_ptr<const NativeWindowFrame> frame, const QRectF&) override {
        require(bool(frame), "Texture presentation retains a native frame.");
        ++nativePresentations;
        return !nativePresentationFails;
    }
    bool present(const LiveWindowPresentation& value, const QRectF&, const QRect&,
                 QString*) override {
        m_presenting = value.visible;
        return true;
    }
    bool presenting() const override {
        return m_presenting;
    }
    void stopPresentation() override {
        m_presenting = false;
    }

  private:
    bool m_presenting = false;
};
std::unique_ptr<NativeWindowMonitor> createNativeWindowMonitor(const LiveWindowTarget&) {
    return std::make_unique<FakeNativeMonitor>();
}
std::unique_ptr<NativeWindowPresentation> createNativeWindowPresentation(const LiveWindowTarget&) {
    return std::make_unique<FakeNativePresentation>();
}
} // namespace snow_shot::presentation::live_window_detail

namespace {
void shutdownDoesNotWaitForNativeCleanup();
void nativePresentationAvoidsContinuousReadback() {
    using namespace snow_shot::presentation;
    nativeFramesEnabled = true;
    sourceState = LiveWindowState::Live;
    const LiveWindowTarget target{42, 7, QRect(100, 200, 100, 80), QPoint(150, 240)};
    auto first = createLiveWindowSource(target, QRect(120, 220, 60, 40));
    auto second = createLiveWindowSource(target, QRect(130, 230, 20, 20));
    int initialized = 0;
    for (auto source : {first, second})
        source->initialize([&](bool ok, const QString&) {
            require(ok, "Native pixel leases support initial raster publication.");
            ++initialized;
        });
    require(waitFor([&] { return initialized == 2; }), "Native consumers initialize.");
    QWidget owner;
    LiveWindowPresentation presentation;
    presentation.owner = &owner;
    presentation.imageTransform.rotate(90).scale(-1, 1);
    require(first->setPresentation(presentation) && second->setPresentation(presentation),
            "Native presentation supports rotations and reflections.");
    const int reads = nativeReadbacks;
    const int presented = nativePresentations;
    require(waitFor([&] { return nativePresentations >= presented + 12; }),
            "Both pins continue presenting GPU frames.");
    require(nativeReadbacks == reads && activeStreams == 1,
            "GPU presentation shares one stream without continuous CPU readback.");
    QImage snapshot;
    first->requestFrame([&](const QImage& image, const QString&) { snapshot = image; });
    require(waitFor([&] { return !snapshot.isNull(); }), "Image actions receive CPU snapshots.");
    require(snapshot.size() == QSize(60, 40) && snapshot.pixelColor(0, 0) == QColor(40, 120, 200) &&
                nativeReadbacks == reads + 1,
            "Only the requested snapshot reads pixels and preserves its untransformed crop.");
    presentation.sourceViewport = QRectF(0.25, 0.25, 0.5, 0.5);
    require(first->setPresentation(presentation),
            "Display cropping preserves native presentation.");
    require(nativeReadbacks == reads + 1, "Changing the display crop does not read CPU pixels.");
    presentation.visible = false;
    require(first->setPresentation(presentation) && second->setPresentation(presentation),
            "Hidden native consumers suspend presentation.");
    require(waitFor([&] { return activeStreams == 0; }),
            "Hiding every native consumer releases the shared capture stream.");
    presentation.visible = true;
    const int beforeShow = nativePresentations;
    require(first->setPresentation(presentation) && second->setPresentation(presentation),
            "Showing native consumers resumes presentation.");
    require(waitFor([&] { return nativePresentations > beforeShow + 4 && activeStreams == 1; }),
            "Shown native consumers resume one shared stream.");
    require(nativeReadbacks == reads + 1, "Showing GPU content does not require CPU readback.");
    nativeReadbackFails = true;
    bool readbackRejected = false;
    first->requestFrame([&](const QImage& image, const QString& error) {
        readbackRejected = image.isNull() && !error.isEmpty();
    });
    require(waitFor([&] { return readbackRejected; }),
            "A failed CPU readback completes the image action with an error.");
    nativeReadbackFails = false;
    require(waitFor([&] { return first->state() == LiveWindowState::Live; }),
            "Readback failure recovers through the normal capture retry policy.");
    nativePresentationFails = true;
    QImage fallback;
    QObject::connect(first.get(), &LiveWindowSource::frameChanged,
                     [&](const QImage& image) { fallback = image; });
    require(waitFor([&] { return !first->usesNativePresentation() && !fallback.isNull(); }),
            "A presenter failure falls back to fresh CPU frames without losing the source.");
    require(first->state() == LiveWindowState::Live, "GPU fallback keeps image actions available.");
    first->shutdown();
    second->shutdown();
    require(waitFor([&] { return activeStreams == 0; }),
            "Native capture retires after final close.");
    nativePresentationFails = false;
    shutdownDoesNotWaitForNativeCleanup();
    nativeFramesEnabled = false;
}

void shutdownDoesNotWaitForNativeCleanup() {
    using namespace snow_shot::presentation;
    const LiveWindowTarget target{42, 7, QRect(100, 200, 100, 80), QPoint(150, 240)};
    for (bool duringInitialization : {true, false}) {
        sourceState = LiveWindowState::Live;
        captureFails = false;
        captureBlocked = false;
        cleanupEntered = false;
        cleanupReleased = false;
        blockCapture = duringInitialization;
        auto source = createLiveWindowSource(target, QRect(120, 220, 60, 40));
        std::weak_ptr<LiveWindowSource> weakSource = source;
        int cancellations = 0;
        bool initialized = false;
        source->initialize([&](bool ok, const QString& error) {
            initialized = ok;
            if (!ok) {
                require(!error.isEmpty(), "Cancelled initialization supplies an error.");
                ++cancellations;
            }
        });
        if (!duringInitialization) {
            require(waitFor([&] { return initialized; }), "Initialize pending-frame fixture.");
            require(waitFor([&] { return activeStreams == 0; }), "Initial capture goes idle.");
            blockCapture = true;
            source->requestFrame([&](const QImage& frame, const QString& error) {
                require(frame.isNull() && !error.isEmpty(), "Pending snapshot is cancelled.");
                ++cancellations;
            });
        }
        require(waitFor([&] { return captureBlocked.load(); }), "Native capture is blocked.");
        const int stopsBefore = stopRequests;
        if (duringInitialization)
            source->shutdown();
        source.reset();
        require(weakSource.expired() && cancellations == 1,
                "Closing releases the source and completes pending actions immediately.");
        require(waitFor([&] { return cleanupEntered.load(); }),
                "Background teardown cancels capture and reaches native cleanup.");
        require(stopRequests == stopsBefore + 1 && activeStreams == 1,
                "Native stream is cancelled once while cleanup remains blocked.");
        {
            std::lock_guard lock(backendMutex);
            cleanupReleased = true;
        }
        backendChanged.notify_all();
        require(waitFor([&] { return activeStreams == 0; }), "Retired stream eventually releases.");
        blockCapture = false;
        require(cancellations == 1, "Retired callbacks cannot complete actions twice.");
    }
}

void failedInitializationCanRestart() {
    using namespace snow_shot::presentation;
    const LiveWindowTarget target{42, 7, QRect(100, 200, 100, 80), QPoint(150, 240)};
    sourceState = LiveWindowState::Live;
    captureFails = true;
    auto source = createLiveWindowSource(target, QRect(120, 220, 60, 40));
    int completions = 0;
    source->initialize([&](bool ok, const QString& error) {
        require(!ok && !error.isEmpty(), "The first initialization reports capture failure.");
        ++completions;
    });
    require(waitFor([&] { return completions == 1 && activeStreams == 0; }),
            "Failed initialization releases the failed capture stream.");
    captureFails = false;
    source->initialize([&](bool ok, const QString&) {
        require(ok, "A new worker can retry failed initialization.");
        ++completions;
    });
    require(waitFor([&] { return completions == 2; }), "Retried initialization completes once.");
    source->shutdown();
    source.reset();
    require(waitFor([&] { return activeStreams == 0; }), "Retry cleanup releases all streams.");
    require(activeMonitors == 0, "Retry cleanup releases its shared monitor.");
}

void callbacksMayShutdownTheirSource() {
    using namespace snow_shot::presentation;
    const LiveWindowTarget target{42, 7, QRect(100, 200, 100, 80), QPoint(150, 240)};
    for (int scenario = 0; scenario < 5; ++scenario) {
        sourceState = LiveWindowState::Live;
        captureFails = false;
        auto source = createLiveWindowSource(target, QRect(120, 220, 60, 40));
        bool stopped = false;
        int stoppedCount = 0;
        int initializationCount = 0;
        bool initialized = false;
        int frameCount = 0;
        int postStopFrames = 0;
        QObject::connect(source.get(), &LiveWindowSource::stopped, [&] {
            stopped = true;
            ++stoppedCount;
        });
        QObject::connect(source.get(), &LiveWindowSource::frameChanged, [&](const QImage&) {
            ++frameCount;
            if (stopped)
                ++postStopFrames;
        });
        if (scenario == 1) {
            QObject::connect(source.get(), &LiveWindowSource::stateChanged,
                             [&](LiveWindowState state, const QString&) {
                                 if (state == LiveWindowState::Live)
                                     source->shutdown();
                             });
        }
        source->initialize([&](bool ok, const QString&) {
            ++initializationCount;
            initialized = ok;
            if (scenario == 0)
                source->shutdown();
        });
        require(waitFor([&] { return initializationCount == 1; }),
                "Reentrant fixtures complete their initialization callback.");
        if (scenario < 2) {
            require(stopped && stoppedCount == 1 && frameCount == 0,
                    "Initialization and initial-state callbacks may stop delivery before its first "
                    "frame.");
            require(initialized == (scenario == 0),
                    "Stopping during initial state completes pending initialization as cancelled.");
        } else {
            require(initialized && !stopped, "Delivery fixtures initialize before stopping.");
            QWidget owner;
            LiveWindowPresentation presentation;
            presentation.owner = &owner;
            require(source->setPresentation(presentation),
                    "Initialize the native display fixture.");
            if (scenario == 2) {
                QObject::connect(source.get(), &LiveWindowSource::frameChanged,
                                 [&](const QImage&) { source->shutdown(); });
                presentation.imageTransform.scale(-1, 1);
                require(source->setPresentation(presentation),
                        "Request native-to-frame handoff before reentrant shutdown.");
                require(waitFor([&] { return stopped; }),
                        "A frame callback can shut down during native-display handoff.");
            } else if (scenario == 3) {
                sourceState = LiveWindowState::Minimized;
                require(waitFor([&] { return source->state() == LiveWindowState::Minimized; }),
                        "Restoration fixture reaches minimized status.");
                const int beforeRestore = frameCount;
                QObject::connect(source.get(), &LiveWindowSource::stateChanged,
                                 [&](LiveWindowState state, const QString&) {
                                     if (state == LiveWindowState::Live)
                                         source->shutdown();
                                 });
                sourceState = LiveWindowState::Live;
                require(waitFor([&] { return stopped; }),
                        "Restoration state callbacks can shut down the source.");
                require(frameCount == beforeRestore,
                        "Shutdown from restored state prevents publishing its pending frame.");
            } else {
                int completed = 0;
                int successful = 0;
                source->requestFrame([&](const QImage& frame, const QString&) {
                    ++completed;
                    successful += !frame.isNull() ? 1 : 0;
                    source->shutdown();
                });
                source->requestFrame([&](const QImage& frame, const QString& error) {
                    ++completed;
                    require(frame.isNull() && !error.isEmpty(),
                            "Later frame requests are cancelled after a callback shuts down.");
                });
                require(waitFor([&] { return stopped && completed == 2; }),
                        "Reentrant shutdown completes every outstanding frame request once.");
                require(successful == 1, "Only the pre-shutdown request receives image pixels.");
            }
        }
        require(waitFor([&] { return activeStreams == 0; }),
                "Reentrant source shutdown releases its capture stream.");
        require(postStopFrames == 0 && stoppedCount == 1,
                "No queued delivery may emit frames after the source has stopped.");
        source->shutdown();
        require(stoppedCount == 1, "Reentrant cleanup retains idempotent shutdown.");
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    using namespace snow_shot::presentation;
    const LiveWindowTarget target{42, 7, QRect(100, 200, 100, 80), QPoint(150, 240),
                                  QStringLiteral("Source document — Editor")};
    auto first = createLiveWindowSource(target, QRect(120, 220, 60, 40));
    auto second = createLiveWindowSource(target, QRect(130, 230, 20, 20));
    int initialized = 0;
    first->initialize([&](bool ok, const QString&) {
        require(ok, "First source initializes from a real mocked capture frame.");
        ++initialized;
    });
    second->initialize([&](bool ok, const QString&) {
        require(ok, "Second source initializes independently.");
        ++initialized;
    });
    require(waitFor([&] { return initialized == 2; }), "Both initialization callbacks complete.");
    require(first->windowTitle() == target.title && second->windowTitle() == target.title,
            "Each crop retains its selected source window title.");
    require(first->initialFrame().size() == QSize(60, 40) &&
                second->initialFrame().size() == QSize(20, 20),
            "Matching source consumers retain independent fixed crop extents.");
    require(createdStreams == 1, "Concurrent initialization shares one native source stream.");
    require(createdMonitors == 1 && activeMonitors == 1,
            "Multiple crops share one native lifecycle observer independently of their surfaces.");
    const QImage firstPaint = first->initialFrame();
    first->releaseInitialFrame();
    require(first->initialFrame().isNull() && firstPaint.pixelColor(0, 0) == QColor(40, 120, 200),
            "Releasing initialization storage leaves the shell's first painted frame valid.");

    QWidget owner;
    LiveWindowPresentation presentation;
    presentation.owner = &owner;
    presentation.imageTransform.scale(-1, 1);
    require(first->setPresentation(presentation) && second->setPresentation(presentation),
            "Both consumers can request continuous transformed display.");
    QImage latest;
    QObject::connect(first.get(), &LiveWindowSource::frameChanged,
                     [&](const QImage& frame) { latest = frame; });
    require(waitFor([&] { return activeStreams == 1 && !latest.isNull(); }),
            "Continuous consumers share one active capture session.");
    sourceWidth = 30;
    sourceHeight = 30;
    require(waitFor([&] {
                return latest.size() == QSize(60, 40) && latest.pixelColor(20, 20).alpha() == 0;
            }),
            "Source shrink preserves the output dimensions and leaves uncovered pixels blank.");
    require(latest.pixelColor(0, 0) == QColor(40, 120, 200),
            "Pixels still inside the original crop retain their source-local position.");
    sourceWidth = 100;
    sourceHeight = 80;
    require(waitFor([&] { return latest.pixelColor(20, 20).alpha() == 255; }),
            "Source growth restores the previously blank content.");

    sourceState = LiveWindowState::Minimized;
    notifyNativeState();
    require(first->state() == LiveWindowState::Minimized &&
                second->state() == LiveWindowState::Minimized,
            "One native notification updates every crop without waiting for per-pin polling.");
    require(waitFor([&] { return first->state() == LiveWindowState::Minimized; }),
            "Minimized state is independent of capture-frame delivery.");
    stateMessagesFollowLanguageChanges(
        *first, "The window is minimized. The latest content cannot be displayed.");
    bool refused = false;
    first->requestFrame([&](const QImage& image, const QString& error) {
        refused = image.isNull() && !error.isEmpty();
    });
    require(refused, "Image actions refuse minimized sources instead of returning stale pixels.");
    sourceState = LiveWindowState::Live;
    require(waitFor([&] { return first->state() == LiveWindowState::Live; }),
            "Restoration waits for a fresh frame and resumes live state.");

    captureFails = true;
    require(waitFor([&] { return first->state() == LiveWindowState::Unavailable; }),
            "Capture errors keep the source object in a recoverable unavailable state.");
    stateMessagesFollowLanguageChanges(*first,
                                       "The window content is temporarily unavailable. Retrying…");
    const int beforeRetry = createdStreams;
    QThread::msleep(100);
    QApplication::processEvents();
    require(createdStreams == beforeRetry, "The status polling timer cannot bypass retry backoff.");
    captureFails = false;
    require(waitFor([&] { return first->state() == LiveWindowState::Live; }),
            "A recoverable capture error retries and restores the association.");

    sourceState = LiveWindowState::Unavailable;
    require(waitFor([&] { return first->state() == LiveWindowState::Unavailable; }),
            "Native inspection errors retain their translated status message.");
    stateMessagesFollowLanguageChanges(*first, "The selected window is no longer available.");
    sourceState = LiveWindowState::Live;
    require(waitFor([&] { return first->state() == LiveWindowState::Live; }),
            "A native inspection error resumes through the existing retry path.");

    bool completedOnShutdown = false;
    int stoppedCount = 0;
    QObject::connect(first.get(), &LiveWindowSource::stopped, [&] { ++stoppedCount; });
    first->requestFrame([&](const QImage&, const QString&) { completedOnShutdown = true; });
    first->shutdown();
    require(activeMonitors == 1, "Closing one crop retains its peer's lifecycle observer.");
    require(completedOnShutdown, "Shutdown completes every outstanding image-operation callback.");
    first->shutdown();
    require(stoppedCount == 1, "Full shutdown publishes exactly one stop notification.");
    sourceState = LiveWindowState::Closed;
    require(waitFor([&] { return second->state() == LiveWindowState::Closed; }),
            "A destroyed source enters the terminal closed-source state.");
    require(second->windowTitle() == target.title,
            "A closed source retains its title for the pinned error heading.");
    stateMessagesFollowLanguageChanges(
        *second, "The window is closed. The latest content cannot be displayed.");
    sourceState = LiveWindowState::Live;
    QApplication::processEvents();
    require(second->state() == LiveWindowState::Closed,
            "A recycled native identity cannot restore a closed source.");
    second->shutdown();
    require(activeMonitors == 0, "The final crop releases the shared lifecycle observer.");
    require(waitFor([&] { return activeStreams == 0; }),
            "Closing the final consumer releases the shared capture resources.");
    callbacksMayShutdownTheirSource();
    shutdownDoesNotWaitForNativeCleanup();
    failedInitializationCanRestart();
    nativePresentationAvoidsContinuousReadback();
    return EXIT_SUCCESS;
}

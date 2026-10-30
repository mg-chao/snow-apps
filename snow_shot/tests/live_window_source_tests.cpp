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
std::atomic<int> sourceWidth{100};
std::atomic<int> sourceHeight{80};

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

struct SnowCaptureStreamImpl {};
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
    --activeStreams;
    delete stream;
}
uint8_t snow_capture_stream_receive(SnowCaptureStream*, uint32_t, SnowCaptureStreamEvent* event) {
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
class FakeNativeWindow final : public NativeWindow {
  public:
    bool initialize(QString*) override {
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
    bool present(const LiveWindowPresentation& value, const QRectF&, const QRectF&,
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
std::unique_ptr<NativeWindow> createNativeWindow(const LiveWindowTarget&) {
    return std::make_unique<FakeNativeWindow>();
}
} // namespace snow_shot::presentation::live_window_detail

namespace {
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
    const LiveWindowTarget target{42, 7, QRect(100, 200, 100, 80), QPoint(150, 240)};
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
    require(first->initialFrame().size() == QSize(60, 40) &&
                second->initialFrame().size() == QSize(20, 20),
            "Matching source consumers retain independent fixed crop extents.");
    require(createdStreams == 1, "Concurrent initialization shares one native source stream.");

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
    require(completedOnShutdown, "Shutdown completes every outstanding image-operation callback.");
    first->shutdown();
    require(stoppedCount == 1, "Full shutdown publishes exactly one stop notification.");
    sourceState = LiveWindowState::Closed;
    require(waitFor([&] { return second->state() == LiveWindowState::Closed; }),
            "A destroyed source enters the terminal closed-source state.");
    stateMessagesFollowLanguageChanges(
        *second, "The window is closed. The latest content cannot be displayed.");
    sourceState = LiveWindowState::Live;
    QApplication::processEvents();
    require(second->state() == LiveWindowState::Closed,
            "A recycled native identity cannot restore a closed source.");
    second->shutdown();
    require(waitFor([&] { return activeStreams == 0; }),
            "Closing the final consumer releases the shared capture resources.");
    callbacksMayShutdownTheirSource();
    return EXIT_SUCCESS;
}

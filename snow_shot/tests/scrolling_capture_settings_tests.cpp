#include "presentation/capture/screenshotscrollingpipeline.h"
#include "snow_shot/platform/screenshotnative.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotoverlayeventsink.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshotscrollingcapturecontroller.h"
#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "widgets/button.h"
#include "widgets/modal.h"
#include "widgets/switch.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QWindow>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>

namespace snow_shot::platform {
bool snowTestSetWindowExcludedFromCapture(QWidget* window, bool excluded);
std::optional<std::uint32_t> snowTestCaptureWindowId(QWidget* window);
#ifdef Q_OS_MACOS
bool snowTestScrollPermission();
ScrollInputResult snowTestSendScreenshotScroll(const QRect&, const QPoint&);
#else
namespace windows {
ScrollInputResult snowTestSendScrollingWheelStep(const QRect&, const QPoint&);
bool snowTestFlushWindowComposition();
} // namespace windows
#endif
} // namespace snow_shot::platform

namespace {
using namespace snow_shot::capture_detail;
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
        QThread::msleep(1);
    }
    require(ready(), "scrolling capture did not complete in time");
}

struct SourceState {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<QImage> frames;
    bool stopped = false;
    void push(const QImage& image) {
        std::lock_guard lock(mutex);
        frames.push_back(image);
        changed.notify_all();
    }
};
std::atomic<int> liveSources = 0;
class ManualSource final : public ScrollingFrameSource {
  public:
    explicit ManualSource(std::shared_ptr<SourceState> state) : m_state(std::move(state)) {
        ++liveSources;
    }
    ~ManualSource() override {
        --liveSources;
    }
    ScrollingSourceEvent receive(int timeout) override {
        std::unique_lock lock(m_state->mutex);
        m_state->changed.wait_for(lock, std::chrono::milliseconds(timeout),
                                  [this] { return m_state->stopped || !m_state->frames.empty(); });
        ScrollingSourceEvent event;
        if (!m_state->frames.empty()) {
            event.kind = ScrollingSourceEvent::Kind::Frame;
            event.frame.image = std::move(m_state->frames.front());
            m_state->frames.pop_front();
        }
        return event;
    }
    void setTargetFps(int) override {}
    ScrollingSourceStats stats() const override {
        return {};
    }
    void stop() override {
        std::lock_guard lock(m_state->mutex);
        m_state->stopped = true;
        m_state->changed.notify_all();
    }

  private:
    std::shared_ptr<SourceState> m_state;
};
struct CaptureRequest {
    QRect selection;
    QVector<std::uint32_t> excludedIds;
    quint64 generation;
    std::shared_ptr<SourceState> source;
};
QVector<CaptureRequest> requests;
QVector<QWidget*> excludedWindows;

class OverlayEvents final : public ScreenshotOverlayEventSink {
  public:
    bool shouldHandleOverlayMouseEvent(const ScreenshotOverlayWindow*, const QPointF&,
                                       bool) const override {
        return false;
    }
    void handleOverlayMousePress(ScreenshotOverlayWindow*, const QPointF&) override {}
    void handleOverlayMouseMove(ScreenshotOverlayWindow*, const QPointF&) override {}
    void handleOverlayMouseRelease(ScreenshotOverlayWindow*, const QPointF&) override {}
    ScreenshotOverlayRightClickResult handleOverlayRightClick(ScreenshotOverlayWindow*,
                                                              const QPointF&) override {
        return ScreenshotOverlayRightClickResult::Ignored;
    }
    bool handleOverlayWheel(ScreenshotOverlayWindow*, const QWheelEvent&) override {
        return false;
    }
    bool shouldBlockUnhandledOverlayKeyInput() const override {
        return false;
    }
    void raiseToolbarForCanvasInteraction() override {}
};
class ToolbarCommands final : public ScreenshotToolbarCommandSink,
                              public ScreenshotSelectionToolbarCommandSink {
  public:
    ScreenshotScrollingCaptureController* capture = nullptr;
    int restarts = 0;
    QPointer<QWidget> settingsSurface;
    QPointer<QWindow> settingsNativeWindow;
    void restartScrollingScreenshot() override {
        require(settingsSurface.isNull() && settingsNativeWindow.isNull(),
                "recapture must wait for the settings surface and native window teardown");
        ++restarts;
        require(capture->restart(), "changed settings must restart the active scrolling session");
    }
    void setMoveTool() override {}
    void setSelectTool() override {}
    void setShapeTool() override {}
    void setArrowTool() override {}
    void setLineTool() override {}
    void setFreeDrawTool() override {}
    void setHighlightTool() override {}
    void setPenHighlightTool() override {}
    void setEraserTool() override {}
    void setFilterTool() override {}
    void setWatermarkTool() override {}
    void setWatermarkConfigFromToolbar(const SnowCanvasWatermarkConfig&) override {}
    void previewWatermarkFromToolbar(const SnowCanvasWatermarkConfig&) override {}
    void setFilterStyleFromToolbar(const SnowCanvasFilterStyle&, quint32) override {}
    void setTextTool() override {}
    void setSerialNumberTool() override {}
    void setOcrTool() override {}
    void startScrollingScreenshot() override {}
    void pinSelectionToScreen() override {}
    void cancelCapture() override {}
    void copySelectionToClipboard() override {}
    void startScreenRecording() override {}
    void setShapeStyleFromToolbar(const SnowCanvasShapeStyle&, quint32,
                                  SnowCanvasShapeKind) override {}
    void setTextStyleFromToolbar(const SnowCanvasTextStyle&, quint32) override {}
    void setSerialNumberStyleFromToolbar(const SnowCanvasSerialNumberStyle&) override {}
    void decrementSelectedSerialNumbers() override {}
    void incrementSelectedSerialNumbers() override {}
    void createTextForSelectedSerialNumber() override {}
    void repositionToolbarForContentChange() override {}
    void hideColorPickersForScreenshotUi() override {}
    void toggleSelectionAspectRatioLockFromToolbar() override {}
    void setSelectionAspectRatioPresetFromToolbar(ScreenshotSelectionAspectRatioPreset) override {}
    void openSelectionResizeModalFromToolbar() override {}
    void adjustSelectionFromToolbar(int, int, int, int) override {}
    void setSelectionCornerRadiusFromToolbar(int) override {}
    void setSelectionShadowWidthFromToolbar(int) override {}
    void setSelectionToolbarHovered(bool) override {}
};

ScreenshotScrollingSnapshot snapshot(ScreenshotScrollingCaptureController& capture) {
    ScreenshotScrollingSnapshot result;
    bool completed = false;
    require(capture.requestTrimmedSnapshot([&](ScreenshotScrollingSnapshot value) {
        result = std::move(value);
        completed = true;
    }),
            "scrolling result must support export");
    until([&] { return completed; });
    require(result.isValid(), "scrolling snapshot must be valid");
    return result;
}

void changedSettingsRecapture(ScreenshotScrollingRecognitionMode mode, bool autoScroll) {
    using namespace adqt::widgets;
    const snow_shot::storage::ScreenshotSettings settings;
    require(settings.setCaptureUiInScrollingScreenshot(false), "seed excluded capture UI");
    requests.clear();
    OverlayEvents events;
    SnowCanvasRuntime runtime;
    snow_shot::presentation::WindowShortcutManager shortcuts;
    ScreenshotOverlayWindow overlay(events, new SnowCanvasWidget);
    overlay.setCaptureGeometry(QRect(0, 0, 400, 300));
    overlay.show();
    CapturedDisplayModel display;
    display.logicalRect = overlay.captureGeometry();
    display.physicalRect = display.logicalRect;
    display.canvasRect = display.physicalRect;
    display.active = true;
    display.geometryResolved = true;
    display.image = QImage(display.physicalRect.size(), QImage::Format_RGB32);
    display.image.fill(Qt::red);
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display, &overlay);
    ScreenshotGeometryMapper geometry;
    geometry.rebuild(displays);
    ToolbarCommands commands;
    ScreenshotOverlayCoordinator coordinator(events, runtime, shortcuts);
    coordinator.setToolbarCommandSinks(commands, commands);
    coordinator.attachToolbarToOverlay(&overlay);
    auto* toolbar = coordinator.toolbar();
    ScreenshotScrollingCaptureController capture({
        displays,
        geometry,
        coordinator,
        [] { return false; },
        [&] { return settings.captureUiInScrollingScreenshot(); },
        [] { require(false, "recapture must not fail"); },
        [] { return true; },
    });
    commands.capture = &capture;
    require(!capture.restart() && requests.isEmpty(), "inactive capture must not restart");
    const QRect selection(40, 30, 64, 64);
    require(capture.start(selection, mode), "start scrolling capture");
    capture.setAutoScrollIntervalMs(kScreenshotScrollingAutoScrollIntervalMaximum);
    capture.setAutoScroll(autoScroll);
    toolbar->setScrollingScreenshotMode(true);
    toolbar->palette()->setScrollingRecognitionMode(mode);
    coordinator.showToolbar();
    QCoreApplication::processEvents();
    until([] { return liveSources == 1; });
    require(requests.size() == 1 && !requests.back().excludedIds.isEmpty(),
            "initial capture must exclude the interface");
    const auto initialIds = requests.back().excludedIds;
    QImage frame(selection.size(), QImage::Format_RGBA8888);
    frame.fill(Qt::red);
    requests.back().source->push(frame);
    until([&] { return capture.state().value(QStringLiteral("ready")).toBool(); });
    require(capture.setTrimRange(8, 40), "seed a trimmed result before recapture");
    const auto previousSnapshot = snapshot(capture);
    require(previousSnapshot.materialize().pixelColor(0, 0) == QColor(Qt::red),
            "initial exported result must use the original frame");

    for (const bool captureInterface : {true, false}) {
        const auto previousGeneration = requests.back().generation;
        auto* settingsButton = toolbar->palette()->findChild<AdButton*>(
            QStringLiteral("screenshotScrollingSettingsButton"));
        require(settingsButton != nullptr, "scrolling settings button must exist");
        require(settingsButton->isEnabled(), "scrolling settings button must be enabled");
        settingsButton->click();
        QCoreApplication::processEvents();
        auto* modal = toolbar->palette()->findChild<AdModal*>(
            QStringLiteral("screenshotScrollingSettingsModal"));
        require(modal && modal->isOpen(), "open scrolling capture settings");
        auto* editor = modal->contentWidget()->findChild<AdSwitch*>();
        require(editor != nullptr, "capture interface switch must exist");
        commands.settingsSurface = modal->contentWidget()->window();
        commands.settingsNativeWindow = commands.settingsSurface->windowHandle();
        require(commands.settingsNativeWindow != nullptr,
                "settings must create a native window before capture resumes");
        const int previousRestarts = commands.restarts;
        editor->setChecked(captureInterface);
        modal->acceptButton()->click();
        require(!modal->isOpen() && commands.restarts == previousRestarts &&
                    requests.back().generation == previousGeneration,
                "accepting settings must leave recapture pending while its surface still exists");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(commands.settingsSurface != nullptr && commands.restarts == previousRestarts,
                "destroying the dialog must not restart capture before its separate surface");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(commands.settingsSurface.isNull() && commands.settingsNativeWindow.isNull() &&
                    commands.restarts == previousRestarts,
                "native teardown must finish before the queued recapture command");
        until([&] { return commands.restarts == previousRestarts + 1; });
        require(capture.active() && capture.canvasSelection() == selection &&
                    capture.recognitionMode() == mode &&
                    capture.state().value(QStringLiteral("auto_scroll")).toBool() == autoScroll,
                "recapture must close settings and preserve selection, direction and auto-scroll");
        require(capture.trimmedSize().isEmpty() &&
                    !capture.state().value(QStringLiteral("ready")).toBool(),
                "recapture after closing settings must discard the previous result and trim");
        until([] { return liveSources == 1; });
        require(requests.back().generation > previousGeneration &&
                    requests.back().selection == selection &&
                    requests.back().excludedIds ==
                        (captureInterface ? QVector<std::uint32_t>{} : initialIds),
                "recapture must create a fresh source with the new interface exclusion policy");
        require(captureInterface ? excludedWindows.isEmpty() : !excludedWindows.isEmpty(),
                "recapture must also reapply the native window sharing policy");
        frame.fill(captureInterface ? Qt::blue : Qt::green);
        requests.back().source->push(frame);
        until([&] { return capture.state().value(QStringLiteral("ready")).toBool(); });
        require(capture.trimmedSize() == selection.size() &&
                    snapshot(capture).materialize().pixelColor(0, 0) ==
                        QColor(captureInterface ? Qt::blue : Qt::green),
                "the new result must contain only recaptured pixels with a reset trim range");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    require(commands.restarts == 2 && requests.size() == 3 &&
                previousSnapshot.materialize().pixelColor(0, 0) == QColor(Qt::red),
            "each changed setting must recapture once while detached snapshots remain valid");
    capture.stop(false);
    require(liveSources == 0 && excludedWindows.isEmpty(),
            "teardown must release capture sources and window exclusions");
}
} // namespace

namespace snow_shot::capture_detail {
ScrollingSourceFactory nativeScrollingSource(QRect selection, bool,
                                             const QVector<std::uint32_t>& excludedIds,
                                             quint64 generation) {
    auto state = std::make_shared<SourceState>();
    requests.push_back({selection, excludedIds, generation, state});
    return [state] { return std::make_unique<ManualSource>(state); };
}
} // namespace snow_shot::capture_detail

namespace snow_shot::platform {
bool snowTestSetWindowExcludedFromCapture(QWidget* window, bool excluded) {
    require(liveSources == 0, "native sources must stop before window sharing changes");
    if (excluded) {
        excludedWindows.push_back(window);
    } else {
        excludedWindows.removeAll(window);
    }
    return true;
}
std::optional<std::uint32_t> snowTestCaptureWindowId(QWidget* window) {
    return static_cast<std::uint32_t>(excludedWindows.indexOf(window) + 11);
}
#ifdef Q_OS_MACOS
bool snowTestScrollPermission() {
    return true;
}
ScrollInputResult snowTestSendScreenshotScroll(const QRect&, const QPoint&) {
    return {ScrollInputResult::Status::Posted, 0};
}
#else
namespace windows {
ScrollInputResult snowTestSendScrollingWheelStep(const QRect&, const QPoint&) {
    return {ScrollInputResult::Status::Posted, 0};
}
bool snowTestFlushWindowComposition() {
    return true;
}
} // namespace windows
#endif
} // namespace snow_shot::platform

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(temporary.isValid() &&
                storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "scrolling settings tests require isolated storage");
    changedSettingsRecapture(ScreenshotScrollingRecognitionMode::Vertical, false);
    changedSettingsRecapture(ScreenshotScrollingRecognitionMode::Horizontal, true);
    storage.shutdown();
    return 0;
}

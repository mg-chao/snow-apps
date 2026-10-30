#include "presentation/capture/screenshotscrollingpipeline.h"
#include "snow_shot/platform/screenshotnative.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotoverlayeventsink.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshotscrollingcapturecontroller.h"
#include "snow_shot/presentation/imagesplitwindow.h"
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
#include <QColorSpace>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QPainter>
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
template <class Predicate>
void until(Predicate ready, const char* message = "scrolling capture did not complete in time") {
    QElapsedTimer deadline;
    deadline.start();
    while (!ready() && deadline.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(ready(), message);
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
std::mutex exclusionMutex;
QHash<std::uint32_t, int> excludedSourceCounts;
QHash<QWidget*, std::uint32_t> nativeWindowIds;
class ManualSource final : public ScrollingFrameSource {
  public:
    ManualSource(std::shared_ptr<SourceState> state, QVector<std::uint32_t> excludedIds)
        : m_state(std::move(state)), m_excludedIds(std::move(excludedIds)) {
        const std::lock_guard lock(exclusionMutex);
        for (const auto id : m_excludedIds)
            ++excludedSourceCounts[id];
        ++liveSources;
    }
    ~ManualSource() override {
        const std::lock_guard lock(exclusionMutex);
        for (const auto id : m_excludedIds)
            --excludedSourceCounts[id];
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
    QVector<std::uint32_t> m_excludedIds;
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
    void adjustSelectionEffectsFromWheel(int, int) override {}
    void setSelectionCornerRadiusFromToolbar(int) override {}
    void setSelectionEffectWidthFromToolbar(int) override {}
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

void destructionKeepsControllerAvailableToStateObservers() {
    OverlayEvents events;
    SnowCanvasRuntime runtime;
    snow_shot::presentation::WindowShortcutManager shortcuts;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotOverlayCoordinator coordinator(events, runtime, shortcuts);
    auto capture = std::make_unique<ScreenshotScrollingCaptureController>(
        ScreenshotScrollingCaptureControllerContext{displays, geometry, coordinator});
    QObject observer;
    int notifications = 0;
    auto* retiring = capture.get();
    QObject::connect(retiring, &ScreenshotScrollingCaptureController::stateChanged, &observer, [&] {
        require(!retiring->active() && retiring->canvasSelection().isEmpty(),
                "final scrolling state observers must be able to inspect the retired session");
        ++notifications;
    });
    capture.reset();
    require(notifications == 1, "scrolling destruction must publish its final state once");
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
    require(overlay.mask().isEmpty(), "starting scrolling must leave the overlay unmasked");
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
        require(overlay.mask().isEmpty(), "restarting scrolling must leave the overlay unmasked");
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
        // Retired workers retain sharing exclusions until native teardown completes.
        until(
            [&] {
                return captureInterface ? excludedWindows.isEmpty() : !excludedWindows.isEmpty();
            },
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
    require(overlay.mask().isEmpty(), "stopping scrolling must leave the overlay unmasked");
    until([] { return liveSources == 0 && excludedWindows.isEmpty(); },
          "teardown must release capture sources and window exclusions");
}

void detachedScrollingResultLoadsIntoImageSplitter(ScreenshotScrollingRecognitionMode mode) {
    using namespace snow_shot::presentation;
    requests.clear();
    OverlayEvents events;
    SnowCanvasRuntime runtime;
    WindowShortcutManager shortcuts;
    ScreenshotOverlayWindow overlay(events, new SnowCanvasWidget);
    overlay.setCaptureGeometry(QRect(0, 0, 800, 700));
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
    ScreenshotScrollingCaptureController capture(
        {displays, geometry, coordinator, [] { return false; }, [] { return false; },
         [] { require(false, "split capture must not fail"); }, [] { return true; }});
    require(!capture.requestTrimmedSnapshot([](auto) {}),
            "inactive scrolling capture must not produce a split source");
    const QRect selection(40, 30, 400, 400);
    require(capture.start(selection, mode), "start scrolling image split fixture");
    require(!capture.requestTrimmedSnapshot([](auto) {}),
            "scrolling capture without a result must not produce a split source");
    capture.setAutoScrollIntervalMs(kScreenshotScrollingAutoScrollIntervalMaximum);
    capture.setAutoScroll(true);
    until([] { return liveSources == 1; });
    const bool horizontal = mode == ScreenshotScrollingRecognitionMode::Horizontal;
    QImage original(horizontal ? QSize(450, 400) : QSize(400, 450), QImage::Format_RGBA8888);
    original.setColorSpace(QColorSpace::SRgb);
    original.fill(Qt::white);
    {
        QPainter painter(&original);
        quint32 random = 1234567;
        for (int y = 0; y < original.height(); y += 7) {
            for (int x = 0; x < original.width(); x += 7) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                painter.fillRect(x, y, 5, 5, QColor::fromRgb(random | 0xff000000U));
            }
        }
    }
    requests.back().source->push(original.copy(QRect(QPoint(), selection.size())));
    until([&] { return capture.state().value(QStringLiteral("ready")).toBool(); });
    requests.back().source->push(
        original.copy(QRect(horizontal ? QPoint(50, 0) : QPoint(0, 50), selection.size())));
    until([&] { return capture.trimmedSize() == original.size(); },
          "the split source must contain both stitched frames");
    require(capture.setTrimRange(12, 438), "trim the stitched image before splitting");
    const QImage expected =
        original.copy(horizontal ? QRect(12, 0, 426, 400) : QRect(0, 12, 400, 426));
    ImageSplitWindow splitWindow;
    bool delivered = false;
    const auto createdUtc = QDateTime::currentDateTimeUtc();
    const auto initialDirection =
        capture.recognitionMode() == ScreenshotScrollingRecognitionMode::Vertical
            ? ImageSplitDirection::Horizontal
            : ImageSplitDirection::Vertical;
    require(capture.requestTrimmedSnapshot([&](ScreenshotScrollingSnapshot result) {
        require(!capture.active() && !capture.state().value(QStringLiteral("auto_scroll")).toBool(),
                "capture and auto-scroll must end before the result is imported");
        require(result.isValid(), "the accepted result must survive capture teardown");
        splitWindow.presentSource({ScreenshotExportSource::fromScrollingSnapshot(std::move(result)),
                                   {},
                                   createdUtc,
                                   {},
                                   expected.size(),
                                   initialDirection});
        delivered = true;
    }),
            "request the current scrolling image for splitting");
    capture.detachPendingResultRequest();
    coordinator.hideToolbar();
    overlay.hide();
    capture.stop(false);
    require(!capture.active() && !overlay.isVisible() && !splitWindow.modal()->isOpen(),
            "capture presentation must close before asynchronous import starts");
    until([&] { return delivered && !splitWindow.loading(); },
          "the image splitting window must finish loading the scrolling result");
    require(splitWindow.modal()->isOpen() && splitWindow.lastError().isEmpty() &&
                splitWindow.document()->image() == expected,
            "the splitting window must import the current stitched pixels and trim range");
    require(
        splitWindow.document()->settings().direction ==
                (horizontal ? ImageSplitDirection::Vertical : ImageSplitDirection::Horizontal) &&
            splitWindow.document()->layout().regionAt(0).size() ==
                (horizontal ? QSize(213, 400) : QSize(400, 213)) &&
            !splitWindow.document()->canUndo(),
        "scrolling results initialize perpendicular split guides without an undo entry");
    splitWindow.shutdown();
    until([] { return liveSources == 0 && excludedWindows.isEmpty(); },
          "image splitting must release capture sources and window exclusions");
}
} // namespace

namespace snow_shot::capture_detail {
ScrollingSourceFactory nativeScrollingSource(QRect selection, bool,
                                             const QVector<std::uint32_t>& excludedIds,
                                             quint64 generation) {
    auto state = std::make_shared<SourceState>();
    requests.push_back({selection, excludedIds, generation, state});
    return [state, excludedIds] { return std::make_unique<ManualSource>(state, excludedIds); };
}
} // namespace snow_shot::capture_detail

namespace snow_shot::platform {
bool snowTestSetWindowExcludedFromCapture(QWidget* window, bool excluded) {
    if (!excluded) {
        const std::lock_guard lock(exclusionMutex);
        require(
            excludedSourceCounts.value(nativeWindowIds.value(window)) == 0,
            "native sources excluding a window must stop before its sharing policy is restored");
    }
    if (excluded) {
        excludedWindows.push_back(window);
    } else {
        excludedWindows.removeAll(window);
    }
    return true;
}
std::optional<std::uint32_t> snowTestCaptureWindowId(QWidget* window) {
    auto id = nativeWindowIds.find(window);
    if (id == nativeWindowIds.end())
        id =
            nativeWindowIds.insert(window, static_cast<std::uint32_t>(nativeWindowIds.size() + 11));
    return id.value();
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
    if (app.arguments().contains(QStringLiteral("--image-split-only"))) {
        detachedScrollingResultLoadsIntoImageSplitter(ScreenshotScrollingRecognitionMode::Vertical);
        detachedScrollingResultLoadsIntoImageSplitter(
            ScreenshotScrollingRecognitionMode::Horizontal);
    } else {
        destructionKeepsControllerAvailableToStateObservers();
        changedSettingsRecapture(ScreenshotScrollingRecognitionMode::Vertical, false);
        changedSettingsRecapture(ScreenshotScrollingRecognitionMode::Horizontal, true);
    }
    storage.shutdown();
    return 0;
}

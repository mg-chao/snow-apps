#ifndef SNOW_SHOT_TESTS_SCREENSHOT_SELECTION_PRESENTATION_FIXTURE_H
#define SNOW_SHOT_TESTS_SCREENSHOT_SELECTION_PRESENTATION_FIXTURE_H

#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_shot/presentation/screenshotcolorpickercontroller.h"
#include "snow_shot/presentation/screenshotcolorpickerwindow.h"
#include "snow_shot/platform/physicalcursor.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotoverlayeventsink.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshotpresentationservices.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshotselectiontoolbarwidget.h"
#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshottoolbarpresenter.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QPaintEvent>
#include <QScreen>
#include <QTemporaryDir>
#include <QTranslator>

#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace selection_presentation_test {
class IsolatedStorage final {
  public:
    IsolatedStorage() {
        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        if (!m_directory.isValid() ||
            !storage.initialize({m_directory.path(), m_directory.path(), 60000}).success)
            throw std::runtime_error("selection presentation fixture requires isolated storage");
    }
    ~IsolatedStorage() {
        snow_shot::storage::ApplicationStorage::instance().shutdown();
    }

  private:
    QTemporaryDir m_directory;
};

class NoopOverlayEvents final : public ScreenshotOverlayEventSink {
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

class NoopToolbarCommands final : public ScreenshotToolbarCommandSink,
                                  public ScreenshotSelectionToolbarCommandSink {
  public:
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
    void hideColorPickersForScreenshotUi() override {
        hideColorPickers();
    }
    void toggleSelectionAspectRatioLockFromToolbar() override {}
    void setSelectionAspectRatioPresetFromToolbar(ScreenshotSelectionAspectRatioPreset) override {}
    void openSelectionResizeModalFromToolbar() override {}
    void adjustSelectionFromToolbar(int, int, int, int) override {}
    void setSelectionCornerRadiusFromToolbar(int) override {}
    void setSelectionShadowWidthFromToolbar(int) override {}
    void setSelectionToolbarHovered(bool) override {}
    std::function<void()> hideColorPickers = [] {};
};

// Count observable translation requests to distinguish content rebuilding from movement.
class HintTranslationObserver final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char* context, const char*, const char*, int) const override {
        if (context != nullptr && std::strcmp(context, "ScreenshotShortcutHintsWidget") == 0)
            ++requests;
        return {};
    }
    mutable qint64 requests = 0;
};

class PaintObserver final : public QObject {
  public:
    QObject* canvas = nullptr;
    QObject* colorPicker = nullptr;
    qint64 canvasPaints = 0;
    qint64 uiPaints = 0;
    qint64 canvasDamagePixels = 0;
    qint64 colorPickerPaints = 0;
    qint64 colorPickerMoves = 0;
    qint64 colorPickerOwnerChanges = 0;

    void reset() {
        canvasPaints = 0;
        uiPaints = 0;
        canvasDamagePixels = 0;
        colorPickerPaints = 0;
        colorPickerMoves = 0;
        colorPickerOwnerChanges = 0;
    }

  protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event != nullptr && event->type() == QEvent::Paint) {
            if (object == canvas) {
                ++canvasPaints;
                for (const QRect& rect : static_cast<QPaintEvent*>(event)->region())
                    canvasDamagePixels += static_cast<qint64>(rect.width()) * rect.height();
            } else {
                ++uiPaints;
                if (object == colorPicker)
                    ++colorPickerPaints;
            }
        }
        if (object == colorPicker && event != nullptr) {
            if (event->type() == QEvent::Move)
                ++colorPickerMoves;
            else if (event->type() == QEvent::ParentChange)
                ++colorPickerOwnerChanges;
        }
        return false;
    }
};

class Fixture final {
  public:
    explicit Fixture(QSize logicalSize = QSize(1200, 800), bool animation = true,
                     bool virtualClock = true)
        : overlay(events, new SnowCanvasWidget(canvasRuntime)),
          coordinator(events, canvasRuntime, shortcuts), toolbar(coordinator, geometry, displays) {
        static_cast<void>(QCoreApplication::installTranslator(&hintTranslations));
        overlay.setWindowTitle(QStringLiteral("Snow Shot selection presentation benchmark"));
        overlay.setCaptureGeometry(QRect(QPoint(), logicalSize));
        const qreal dpr = overlay.devicePixelRatioF();
        const QSize physicalSize(static_cast<int>(std::ceil(logicalSize.width() * dpr)),
                                 static_cast<int>(std::ceil(logicalSize.height() * dpr)));
        CapturedDisplayModel display;
        display.stableId = QStringLiteral("selection-presentation-fixture");
        display.logicalRect = overlay.captureGeometry();
        display.physicalRect = QRect(QPoint(), physicalSize);
        display.logicalToPhysicalScale = dpr;
        display.screen = QGuiApplication::primaryScreen();
        display.active = true;
        display.geometryResolved = true;
        display.image = QImage(physicalSize, QImage::Format_RGB32);
        for (int y = 0; y < physicalSize.height(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(display.image.scanLine(y));
            for (int x = 0; x < physicalSize.width(); ++x)
                row[x] =
                    qRgb((x * 17 + y * 3) & 255, (x * 5 + y * 19) & 255, (x * 11 + y * 7) & 255);
        }
        displays.appendDisplay(std::move(display), &overlay);
        displays.startup = std::make_shared<ScreenshotStartupContext>();
        displays.startup->sessionId = 1;
        displays.startup->phase = ScreenshotStartupContext::Phase::Revealed;
        displays.startup->displaySlot = 0;
        displays.startup->displayId = displays.displayAt(0).stableId;
        displays.startup->logicalPosition = QPoint(30, 30);
        geometry.rebuild(displays);
        coordinator.setToolbarCommandSinks(commands, commands);
        coordinator.applyDisplayModels(displays);
        coordinator.attachToolbarToOverlay(&overlay);
        coordinator.attachSelectionToolbarToOverlay(&overlay);
        interaction.enterOverlayVisible(true);
        intelligentSelection.beginCaptureSession(true);
        captureState.sessionId = 1;
        captureState.sessionState = ScreenshotSessionState::OverlayVisible;
        ScreenshotPresentationServicesContext context{captureState,
                                                      coordinator,
                                                      toolbar,
                                                      geometry,
                                                      displays,
                                                      interaction,
                                                      selection,
                                                      intelligentSelection,
                                                      {},
                                                      [this]() {
                                                          ++stateNotifications;
                                                          if (onStateChanged)
                                                              onStateChanged();
                                                      }};
#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE) ||                                         \
    defined(SNOW_SHOT_SELECTION_BASELINE_HAS_FRAME_SCHEDULER)
        if (virtualClock)
            context.monotonicNanoseconds = [this]() { return nowNanoseconds; };
#else
        Q_UNUSED(virtualClock);
#endif
#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
        context.presentColorPicker = [this](ScreenshotOverlayWindow* owner,
                                            const QPointF& position) {
            if (colorPickerController)
                colorPickerController->updateForOverlay(owner, position,
                                                        services->colorPickerContext());
        };
#endif
        services = std::make_unique<ScreenshotPresentationServices>(std::move(context));
        commands.hideColorPickers = [this] {
#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
            services->discardColorPickerPresentation();
#endif
            coordinator.hideColorPicker();
        };
        preferences.selectionTransitionAnimationEnabled = animation;
        services->setUiPreferences(preferences);
        overlay.show();
        requestSelection(baseSelection());
        flushFrame();
        processEvents();
        processEvents();
        paintObserver.canvas = overlay.canvas();
        for (QWidget* widget : QApplication::allWidgets()) {
            auto* mainToolbar = coordinator.toolbar();
            if (widget == &overlay || overlay.isAncestorOf(widget) ||
                (mainToolbar != nullptr &&
                 (widget == mainToolbar || mainToolbar->isAncestorOf(widget))))
                widget->installEventFilter(&paintObserver);
        }
        resetCounters();
    }

    ~Fixture() {
        services.reset();
        coordinator.destroyUiResources();
        static_cast<void>(QCoreApplication::removeTranslator(&hintTranslations));
    }

    QRectF baseSelection() const {
        const QRectF bounds = geometry.canvasBounds();
        return QRectF(bounds.width() * 0.1, bounds.height() * 0.1, bounds.width() * 0.4,
                      bounds.height() * 0.35);
    }
    void requestSelection(const QRectF& rect) {
        selection.setSelectionRect(rect);
        services->updateOverlayState();
    }
    void requestPointer(const QPointF& position) {
        displays.startup->logicalPosition = overlay.mapToGlobal(position.toPoint());
#if defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE) &&                                          \
    !defined(SNOW_SHOT_SELECTION_BASELINE_HAS_FRAME_SCHEDULER)
        coordinator.updateGuideLines(
            displays, &overlay, position, !interaction.inactive() && guidesEnabled,
            preferences.cursorGuideLineColor, preferences.monitorCenterGuideLineColor,
            preferences.selectionCenterGuideLineColor);
#else
        services->updatePointerPresentation(&overlay, position);
#endif
    }
    void enableColorPicker(ScreenshotColorPickerDisplayMode mode) {
        coordinator.createColorPicker(overlay.mapToGlobal(QPoint(30, 30)));
        coordinator.prepareColorPickerSurface(displays);
        colorPickerController = std::make_unique<ScreenshotColorPickerController>(
            coordinator, geometry, displays, physicalCursor);
        colorPickerController->setDisplayMode(mode);
        preferences.colorPickerDisplayMode = mode;
        services->setUiPreferences(preferences);
        paintObserver.colorPicker = coordinator.colorPicker();
        coordinator.colorPicker()->installEventFilter(&paintObserver);
        requestMagnifier(QPointF(30, 30));
        flushFrame();
        processEvents();
        resetCounters();
    }
    void requestMagnifier(const QPointF& position) {
        requestPointer(position);
#if defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
        colorPickerController->updateForOverlay(&overlay, position, services->colorPickerContext());
#else
        services->requestColorPickerPresentation(&overlay, position);
#endif
    }
    void enableGuides() {
        guidesEnabled = true;
        services->setGuideLinesVisible(true);
        flushFrame();
        processEvents();
        resetCounters();
    }
    void advanceClock(qint64 milliseconds) {
        nowNanoseconds += milliseconds * 1000000;
    }
    void flushFrame() {
#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE) ||                                         \
    defined(SNOW_SHOT_SELECTION_BASELINE_HAS_FRAME_SCHEDULER)
        services->flushPendingFrame();
#endif
    }
    static void processEvents() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
        QCoreApplication::processEvents();
    }
    void resetCounters() {
        stateNotifications = 0;
        hintTranslations.requests = 0;
        paintObserver.reset();
#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
        if (auto* picker = coordinator.colorPicker())
            picker->resetWorkCounters();
#endif
        // Diagnostic regions otherwise accumulate a crosshair grid across frames,
        // distorting timing according to the number of coalesced pointer samples.
        resetSelectionRenderDiagnosticsForCurrentThread();
        resetGuideLineRenderDiagnosticsForCurrentThread();
    }
    QRectF displayedSelection() const {
        return overlay.screenshotRendererForTesting()->selection();
    }

    NoopOverlayEvents events;
    NoopToolbarCommands commands;
    SnowCanvasRuntime canvasRuntime;
    snow_shot::presentation::WindowShortcutManager shortcuts;
    ScreenshotOverlayWindow overlay;
    ScreenshotOverlayCoordinator coordinator;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotToolbarPresenter toolbar;
    ScreenshotCaptureState captureState;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    ScreenshotUiPreferences preferences;
    HintTranslationObserver hintTranslations;
    PaintObserver paintObserver;
    snow_shot::platform::PhysicalCursor physicalCursor{
        {true, [] { return std::optional<QPoint>{}; }, [](const QPoint&) { return true; }}};
    std::unique_ptr<ScreenshotColorPickerController> colorPickerController;
    std::unique_ptr<ScreenshotPresentationServices> services;
    qint64 nowNanoseconds = 1000000000;
    qint64 stateNotifications = 0;
    std::function<void()> onStateChanged;
    bool guidesEnabled = false;
};
} // namespace selection_presentation_test

#endif // SNOW_SHOT_TESTS_SCREENSHOT_SELECTION_PRESENTATION_FIXTURE_H

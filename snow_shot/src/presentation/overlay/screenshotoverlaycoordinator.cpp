#include "snow_shot/presentation/screenshotoverlaycoordinator.h"

#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshotoverlayeventsink.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "../pinned/screenshotpintoperfinstrumentation.h"

#include <QGuiApplication>
#include <QScreen>
#include <QWidget>

#include <limits>

ScreenshotOverlayCoordinator::ScreenshotOverlayCoordinator(ScreenshotOverlayEventSink& eventSink,
                                                           SnowCanvasRuntime& canvasRuntime,
                                                           snow_shot::presentation::WindowShortcutManager&
                                                               shortcutManager)
    : m_overlayPool(
          eventSink, canvasRuntime, shortcutManager,
          ScreenshotOverlayPoolCallbacks{
              [this](ScreenshotOverlayWindow* overlay) { detachOverlayTransientUi(overlay); },
              [this](ScreenshotOverlayWindow* overlay) { clearOverlayCanvas(overlay); },
          }),
      m_canvasPresenter([this](ScreenshotOverlayWindow* overlay) {
          return m_overlayPool.ensureOverlay(overlay);
      }) {}

ScreenshotOverlayCoordinator::~ScreenshotOverlayCoordinator() {
    destroyUiResources();
}

void ScreenshotOverlayCoordinator::setToolbarCommandSinks(
    ScreenshotToolbarCommandSink& toolbarCommands,
    ScreenshotSelectionToolbarCommandSink& selectionToolbarCommands) {
    m_uiHost.setToolbarCommandSinks(toolbarCommands, selectionToolbarCommands);
}

void ScreenshotOverlayCoordinator::prewarmDisplayPool(ScreenshotDisplaySession& displaySession,
                                                      int displayCount) {
    m_overlayPool.prewarmDisplayPool(displaySession, displayCount);
}

void ScreenshotOverlayCoordinator::clearOverlayCanvas(ScreenshotOverlayWindow* overlay) const {
    if (overlay != nullptr) {
        overlay->setScrollingCaptureMode(false);
        overlay->clearInputPassThroughRect();
    }
    m_canvasPresenter.clearOverlayCanvas(overlay);
    m_uiHost.hideColorPickerForOverlay(overlay);
}

void ScreenshotOverlayCoordinator::detachOverlayTransientUi(ScreenshotOverlayWindow* overlay) {
    m_uiHost.detachOverlayTransientUi(overlay);
}

void ScreenshotOverlayCoordinator::clearOverlayCanvases(
    const ScreenshotDisplaySession& displaySession) const {
    m_overlayPool.clearOverlayCanvases(displaySession);
}

void ScreenshotOverlayCoordinator::clearDisplays(ScreenshotDisplaySession& displaySession) {
    m_overlayPool.clearDisplays(displaySession);
    m_uiHost.resetColorPickerForNewCapture();
}

void ScreenshotOverlayCoordinator::destroyDisplayPool(ScreenshotDisplaySession& displaySession) {
    m_overlayPool.destroyDisplayPool(displaySession);
}

void ScreenshotOverlayCoordinator::resetForNewCapture(ScreenshotDisplaySession& displaySession) {
    flushDeferredOverlayMaintenance(displaySession);
    m_overlayPool.resetForNewCapture(displaySession);
    m_uiHost.resetColorPickerForNewCapture();
    m_uiHost.hideShortcutHints();

    resetToolbarForNewCapture();
}

void ScreenshotOverlayCoordinator::prepareDisplayModels(ScreenshotDisplaySession& displaySession) {
    m_canvasPresenter.prepareDisplayModels(displaySession);
}

void ScreenshotOverlayCoordinator::applyDisplayModels(ScreenshotDisplaySession& displaySession) {
    m_canvasPresenter.applyDisplayModels(displaySession);
}

bool ScreenshotOverlayCoordinator::preparePreCaptureOverlayWindows(
    ScreenshotDisplaySession& displaySession) {
    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        return false;
    }

    const qsizetype screenCount = screens.size();
    const int prewarmCount = screenCount > std::numeric_limits<int>::max()
                                 ? std::numeric_limits<int>::max()
                                 : static_cast<int>(screenCount);
    m_overlayPool.prewarmDisplayPool(displaySession, prewarmCount);
    for (qsizetype index = 0; index < displaySession.size(); ++index) {
        CapturedDisplayModel& display = displaySession.displayAt(index);
        if (index >= screenCount) {
            display.active = false;
            continue;
        }

        QScreen* screen = screens.at(index);
        if (screen == nullptr) {
            display.active = false;
            continue;
        }

        const QRect physicalRect = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
        display.stableId.clear();
        display.name = screen->name();
        display.logicalRect = screen->geometry();
        display.physicalRect = physicalRect;
        display.canvasRect = physicalRect;
        display.screen = screen;
        display.image = QImage();
        display.active = true;

        ScreenshotOverlayWindow* overlay =
            displaySession.ensureOverlayAt(index, [this](ScreenshotOverlayWindow* existingOverlay) {
                return m_overlayPool.ensureOverlay(existingOverlay);
            });
        if (overlay == nullptr) {
            continue;
        }
        if (overlay->screen() != screen) {
            overlay->setScreen(screen);
        }
        if (overlay->geometry() != display.logicalRect) {
            overlay->setGeometry(display.logicalRect);
        }
        overlay->setCanvasClearBackgroundEnabled(false);
        overlay->setScreenshotMaskVisible(true);
    }

    return true;
}

void ScreenshotOverlayCoordinator::showOverlayWindows(
    const ScreenshotDisplaySession& displaySession, ScreenshotOverlayShowMode mode) {
    flushDeferredOverlayMaintenance(displaySession);
    m_canvasPresenter.showOverlayWindows(displaySession, mode);
}

void ScreenshotOverlayCoordinator::hideOverlayWindowsImmediately(
    const ScreenshotDisplaySession& displaySession) {
    // Reset the pooled toolbars before hiding them. Translucent Windows
    // surfaces retain their last composed frame while hidden, so resetting
    // after hide lets the old style/selection row flash on the next show.
    // Retire only the native overlay surfaces here: pin/copy exports may still
    // consume the renderer and canvas state asynchronously after this hide.
    m_uiHost.resetToolbarForNewCapture();
    m_uiHost.hideToolbar();
    m_uiHost.hideShortcutHints();
    SNOW_SHOT_PIN_PERF_MILESTONE("overlay.toolbar_hidden");
    displaySession.forEachOverlay([](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (overlay == nullptr) {
            return;
        }
        overlay->setCanvasClearBackgroundEnabled(false);
        overlay->clearInputPassThroughRect();
        // The export has already snapshotted the display/model inputs by
        // the time this presentation is detached. Retire only the native
        // surface; the renderer and scrolling thumbnail remain available
        // until the asynchronous export consumes them.
        overlay->releaseNativeSurface();
    });
    m_overlayMaintenancePending = true;
    SNOW_SHOT_PIN_PERF_MILESTONE("overlay.overlays_hidden");
}

void ScreenshotOverlayCoordinator::hideOverlayWindows(
    const ScreenshotDisplaySession& displaySession) {
    // The full hide is used once an active export has settled. Clear each
    // overlay's render state, then synchronously retire its native surface and
    // desktop-sized backing store. The pooled QWidget graph remains reusable.
    if (m_overlayMaintenancePending) {
        flushDeferredOverlayMaintenance(displaySession);
        return;
    }

    m_uiHost.resetToolbarForNewCapture();
    m_uiHost.hideToolbar();
    m_uiHost.hideShortcutHints();
    displaySession.forEachOverlay([this](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (overlay == nullptr) {
            return;
        }
        overlay->setCanvasClearBackgroundEnabled(false);
        overlay->clearInputPassThroughRect();
        m_canvasPresenter.clearOverlayCanvas(overlay);
        overlay->releaseNativeSurface();
    });
    m_overlayMaintenancePending = false;
}

void ScreenshotOverlayCoordinator::flushDeferredOverlayMaintenance(
    const ScreenshotDisplaySession& displaySession) {
    if (!m_overlayMaintenancePending) {
        return;
    }
    displaySession.forEachOverlay([this](qsizetype, ScreenshotOverlayWindow* overlay) {
        m_canvasPresenter.clearOverlayCanvas(overlay);
        overlay->releaseNativeSurface();
    });
    m_uiHost.resetToolbarForNewCapture();
    m_overlayMaintenancePending = false;
}

void ScreenshotOverlayCoordinator::updateOverlayState(
    const ScreenshotDisplaySession& displaySession, const QRectF& selection, int cornerRadius,
    int shadowWidth, const QColor& shadowColor, bool selectionToolbarHovered,
    bool selectionHandlesVisible, bool intelligentSelecting, bool manualSelecting, bool dragging) {
    m_canvasPresenter.updateOverlayState(displaySession, selection, cornerRadius, shadowWidth,
                                         shadowColor, selectionToolbarHovered,
                                         selectionHandlesVisible, intelligentSelecting,
                                         manualSelecting, dragging);
}

namespace {
QRect scrollingHoleForDisplay(const CapturedDisplayModel& display, const QRectF& selection) {
    const QRectF canvasRect = ScreenshotGeometryMapper::displayCanvasRect(display);
    const QRectF intersection = selection.intersected(canvasRect);
    if (intersection.isEmpty() || canvasRect.isEmpty() || display.logicalRect.isEmpty()) {
        return {};
    }

    const qreal scaleX = static_cast<qreal>(display.logicalRect.width()) / canvasRect.width();
    const qreal scaleY = static_cast<qreal>(display.logicalRect.height()) / canvasRect.height();
    const QRectF localRect((intersection.left() - canvasRect.left()) * scaleX,
                           (intersection.top() - canvasRect.top()) * scaleY,
                           intersection.width() * scaleX, intersection.height() * scaleY);
    return localRect.toAlignedRect().intersected(QRect(QPoint(0, 0), display.logicalRect.size()));
}
} // namespace

void ScreenshotOverlayCoordinator::setScrollingCaptureMode(
    const ScreenshotDisplaySession& displaySession, const QRectF& selection, bool enabled) {
    displaySession.forEachActiveOverlay([enabled, &selection](qsizetype,
                                                              const CapturedDisplayModel& display,
                                                              ScreenshotOverlayWindow* overlay) {
        if (overlay == nullptr) {
            return;
        }
        SnowCanvasWidget* canvas = overlay->canvas();
        if (enabled) {
            if (canvas != nullptr) {
                canvas->clearCursorForLayer(SnowCanvasCursorLayer::Host);
            }
            overlay->setInputPassThroughRect(scrollingHoleForDisplay(display, selection));
            overlay->setScrollingCaptureMode(true);
            return;
        }

        overlay->setScrollingCaptureMode(false);
    });
}

void ScreenshotOverlayCoordinator::updateOverlayCursors(
    const ScreenshotDisplaySession& displaySession, bool selecting, bool dragging) const {
    m_canvasPresenter.updateOverlayCursors(displaySession, selecting, dragging);
}

void ScreenshotOverlayCoordinator::setSelectionMaskColor(
    const ScreenshotDisplaySession& displaySession, const QColor& color) const {
    displaySession.forEachOverlay([&color](qsizetype, ScreenshotOverlayWindow* overlay) {
        overlay->setScreenshotMaskColor(color);
    });
}

void ScreenshotOverlayCoordinator::updateGuideLines(
    const ScreenshotDisplaySession& displaySession, ScreenshotOverlayWindow* owner,
    const QPointF& localPosition, bool selecting, const QColor& cursorColor,
    const QColor& monitorCenterColor) const {
    m_canvasPresenter.updateGuideLines(displaySession, owner, localPosition, selecting,
                                       cursorColor, monitorCenterColor);
}

void ScreenshotOverlayCoordinator::updateGuideLinesAtGlobalPosition(
    const ScreenshotDisplaySession& displaySession, const QPoint& globalPosition, bool selecting,
    const QColor& cursorColor, const QColor& monitorCenterColor) const {
    m_canvasPresenter.updateGuideLinesAtGlobalPosition(displaySession, globalPosition, selecting,
                                                       cursorColor, monitorCenterColor);
}

void ScreenshotOverlayCoordinator::clearGuideLines(
    const ScreenshotDisplaySession& displaySession) const {
    m_canvasPresenter.clearGuideLines(displaySession);
}

void ScreenshotOverlayCoordinator::setOverlayCursor(ScreenshotOverlayWindow* overlay,
                                                    ScreenshotSelectionDragMode dragMode) const {
    m_canvasPresenter.setOverlayCursor(overlay, dragMode);
}

void ScreenshotOverlayCoordinator::setCanvasInteractionEnabled(
    const ScreenshotDisplaySession& displaySession, bool enabled) const {
    m_canvasPresenter.setCanvasInteractionEnabled(displaySession, enabled);
}

void ScreenshotOverlayCoordinator::setCanvasTool(const ScreenshotDisplaySession& displaySession,
                                                 SnowCanvasTool tool) {
    m_canvasPresenter.setCanvasTool(displaySession, tool);
}

void ScreenshotOverlayCoordinator::refreshCanvasCreationStyles(
    const ScreenshotDisplaySession& displaySession,
    const SnowCanvasStyleDefaults& defaults) const {
    m_canvasPresenter.refreshCanvasCreationStyles(displaySession, defaults);
    if (ScreenshotToolbarWindow* toolbarWindow = toolbar()) {
        if (ScreenshotToolPalette* palette = toolbarWindow->palette()) {
            palette->setCreationStyleDefaults(defaults);
        }
    }
}

bool ScreenshotOverlayCoordinator::resetEditingState(
    const ScreenshotDisplaySession& displaySession) const {
    return m_canvasPresenter.resetEditingState(displaySession);
}

bool ScreenshotOverlayCoordinator::tryCurrentRectangleStyle(
    const ScreenshotDisplaySession& displaySession, SnowCanvasShapeStyle* outStyle) const {
    return m_canvasPresenter.tryCurrentRectangleStyle(displaySession, outStyle);
}

bool ScreenshotOverlayCoordinator::tryCurrentStyleToolbarState(
    const ScreenshotDisplaySession& displaySession, SnowCanvasStyleToolbarState* outState) const {
    return m_canvasPresenter.tryCurrentStyleToolbarState(displaySession, outState);
}

SnowCanvasShapeStyle ScreenshotOverlayCoordinator::currentRectangleStyle(
    const ScreenshotDisplaySession& displaySession) const {
    return m_canvasPresenter.currentRectangleStyle(displaySession);
}

void ScreenshotOverlayCoordinator::setShapeStylePatch(
    const ScreenshotDisplaySession& displaySession, const SnowCanvasShapeStyle& style,
    quint32 properties, SnowCanvasShapeKind kind) {
    m_canvasPresenter.setShapeStylePatch(displaySession, style, properties, kind);
}

void ScreenshotOverlayCoordinator::setFilterStyle(const ScreenshotDisplaySession& displaySession,
                                                  const SnowCanvasFilterStyle& style,
                                                  quint32 properties) {
    m_canvasPresenter.setFilterStyle(displaySession, style, properties);
}

void ScreenshotOverlayCoordinator::setWatermarkConfig(
    const ScreenshotDisplaySession& displaySession, const SnowCanvasWatermarkConfig& config) {
    m_canvasPresenter.setWatermarkConfig(displaySession, config);
}

void ScreenshotOverlayCoordinator::previewWatermarkConfig(
    const ScreenshotDisplaySession& displaySession, const SnowCanvasWatermarkConfig& config) {
    m_canvasPresenter.previewWatermarkConfig(displaySession, config);
}

void ScreenshotOverlayCoordinator::setTextStyle(const ScreenshotDisplaySession& displaySession,
                                                const SnowCanvasTextStyle& style) {
    m_canvasPresenter.setTextStyle(displaySession, style);
}

void ScreenshotOverlayCoordinator::setSerialNumberStyle(
    const ScreenshotDisplaySession& displaySession, const SnowCanvasSerialNumberStyle& style) {
    m_canvasPresenter.setSerialNumberStyle(displaySession, style);
}

void ScreenshotOverlayCoordinator::adjustSelectedSerialNumbers(
    const ScreenshotDisplaySession& displaySession, qint64 delta) {
    m_canvasPresenter.adjustSelectedSerialNumbers(displaySession, delta);
}

void ScreenshotOverlayCoordinator::createTextForSelectedSerialNumber(
    const ScreenshotDisplaySession& displaySession) {
    m_canvasPresenter.createTextForSelectedSerialNumber(displaySession);
}

void ScreenshotOverlayCoordinator::reorderSelectedElements(
    const ScreenshotDisplaySession& displaySession, SnowCanvasSelectionOrder order) {
    m_canvasPresenter.reorderSelectedElements(displaySession, order);
}

void ScreenshotOverlayCoordinator::setSelectedElementsOpacity(
    const ScreenshotDisplaySession& displaySession, qreal opacity) {
    m_canvasPresenter.setSelectedElementsOpacity(displaySession, opacity);
}

void ScreenshotOverlayCoordinator::duplicateSelectedElements(
    const ScreenshotDisplaySession& displaySession) {
    m_canvasPresenter.duplicateSelectedElements(displaySession);
}

void ScreenshotOverlayCoordinator::deleteSelectedElements(
    const ScreenshotDisplaySession& displaySession) {
    m_canvasPresenter.deleteSelectedElements(displaySession);
}

ScreenshotToolbarWindow* ScreenshotOverlayCoordinator::ensureToolbar() {
    return m_uiHost.ensureToolbar();
}

void ScreenshotOverlayCoordinator::setSpotlightConfig(ScreenshotDisplaySession& displaySession,
                                                      const SnowCanvasSpotlightConfig& config) {
    m_canvasPresenter.setSpotlightConfig(displaySession, config);
}

void ScreenshotOverlayCoordinator::previewSpotlightConfig(ScreenshotDisplaySession& displaySession,
                                                          const SnowCanvasSpotlightConfig& config) {
    m_canvasPresenter.previewSpotlightConfig(displaySession, config);
}

ScreenshotToolbarWindow* ScreenshotOverlayCoordinator::toolbar() const {
    return m_uiHost.toolbar();
}

void ScreenshotOverlayCoordinator::attachToolbarToOverlay(ScreenshotOverlayWindow* overlay) {
    m_uiHost.attachToolbarToOverlay(overlay);
}

void ScreenshotOverlayCoordinator::undoCanvasEdit() {
    m_uiHost.undoCanvasEdit();
}

void ScreenshotOverlayCoordinator::redoCanvasEdit() {
    m_uiHost.redoCanvasEdit();
}

ScreenshotColorPickerWidget* ScreenshotOverlayCoordinator::ensureColorPicker() {
    return m_uiHost.ensureColorPicker();
}

ScreenshotColorPickerWidget* ScreenshotOverlayCoordinator::colorPicker() const {
    return m_uiHost.colorPicker();
}

void ScreenshotOverlayCoordinator::updateColorPicker(ScreenshotOverlayWindow* overlay,
                                                     const QImage& image, const QRect& physicalRect,
                                                     const QPoint& physicalPoint,
                                                     const QPointF& localPosition, qreal opacity) {
    m_uiHost.updateColorPicker(overlay, image, physicalRect, physicalPoint, localPosition, opacity);
}

void ScreenshotOverlayCoordinator::hideColorPicker() {
    m_uiHost.hideColorPicker();
}

void ScreenshotOverlayCoordinator::setColorPickerCenterGuideLineColor(const QColor& color) {
    m_uiHost.setColorPickerCenterGuideLineColor(color);
}

void ScreenshotOverlayCoordinator::updateShortcutHints(ScreenshotOverlayWindow* overlay,
                                                       ScreenshotShortcutHintMode mode,
                                                       qreal opacity,
                                                       const QRectF& selectionGlobal) {
    m_uiHost.updateShortcutHints(overlay, mode, opacity, selectionGlobal);
}

void ScreenshotOverlayCoordinator::updateShortcutHints(ScreenshotOverlayWindow* overlay,
                                                       const ScreenshotShortcutHintContext& context,
                                                       qreal opacity,
                                                       const QRectF& selectionGlobal) {
    m_uiHost.updateShortcutHints(overlay, context, opacity, selectionGlobal);
}

void ScreenshotOverlayCoordinator::hideShortcutHints() {
    m_uiHost.hideShortcutHints();
}

bool ScreenshotOverlayCoordinator::screenshotUiContainsGlobalCursor() const {
    return m_uiHost.screenshotUiContainsGlobalCursor();
}

bool ScreenshotOverlayCoordinator::stepToolbarStrokeWidth(int direction) {
    return m_uiHost.stepToolbarStrokeWidth(direction);
}

bool ScreenshotOverlayCoordinator::stepToolbarSelectionOpacity(int direction) {
    return m_uiHost.stepToolbarSelectionOpacity(direction);
}

ScreenshotSelectionToolbarWidget* ScreenshotOverlayCoordinator::selectionToolbar() const {
    return m_uiHost.selectionToolbar();
}

void ScreenshotOverlayCoordinator::attachSelectionToolbarToOverlay(
    ScreenshotOverlayWindow* overlay) {
    m_uiHost.attachSelectionToolbarToOverlay(overlay);
}

bool ScreenshotOverlayCoordinator::stepToolbarSpotlightOpacity(int direction) {
    return m_uiHost.stepToolbarSpotlightOpacity(direction);
}

bool ScreenshotOverlayCoordinator::stepToolbarFilterIntensity(int direction) {
    return m_uiHost.stepToolbarFilterIntensity(direction);
}

bool ScreenshotOverlayCoordinator::stepToolbarPenFilterStrokeWidth(int direction) {
    return m_uiHost.stepToolbarPenFilterStrokeWidth(direction);
}

bool ScreenshotOverlayCoordinator::stepToolbarWatermarkFontSize(int direction) {
    return m_uiHost.stepToolbarWatermarkFontSize(direction);
}

void ScreenshotOverlayCoordinator::resetToolbarForNewCapture() {
    m_uiHost.resetToolbarForNewCapture();
}

void ScreenshotOverlayCoordinator::hideToolbar() {
    m_uiHost.hideToolbar();
}

void ScreenshotOverlayCoordinator::showToolbar() {
    m_uiHost.showToolbar();
}

void ScreenshotOverlayCoordinator::raiseToolbar() {
    m_uiHost.raiseToolbar();
}

void ScreenshotOverlayCoordinator::hideSelectionToolbar() {
    m_uiHost.hideSelectionToolbar();
}

void ScreenshotOverlayCoordinator::showSelectionToolbar() {
    m_uiHost.showSelectionToolbar();
}

void ScreenshotOverlayCoordinator::raiseSelectionToolbar() {
    m_uiHost.raiseSelectionToolbar();
}

void ScreenshotOverlayCoordinator::destroyUiResources() {
    m_uiHost.destroyUiResources();
}

QVector<std::uintptr_t>
ScreenshotOverlayCoordinator::excludedHwnds(const ScreenshotDisplaySession& displaySession) const {
    QVector<std::uintptr_t> hwnds;
    hwnds.reserve(displaySession.size() + 1);

    const auto appendWidgetHwnd = [&hwnds](QWidget* widget) {
        if (widget == nullptr) {
            return;
        }
        const WId id = widget->winId();
        if (id != 0) {
            hwnds.push_back(static_cast<std::uintptr_t>(id));
        }
    };

    displaySession.forEachOverlay([&appendWidgetHwnd](qsizetype, ScreenshotOverlayWindow* overlay) {
        appendWidgetHwnd(overlay);
    });
    appendWidgetHwnd(m_uiHost.toolbar());
    return hwnds;
}

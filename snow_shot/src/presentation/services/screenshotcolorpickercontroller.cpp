#include "snow_shot/presentation/screenshotcolorpickercontroller.h"

#include "snow_shot/platform/physicalcursor.h"
#include "snow_shot/presentation/screenshotcolorpickerwindow.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotselectionlimits.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>

#include <algorithm>
#include <optional>

namespace {
constexpr int kSelectionOpacityTolerance = 4;
} // namespace

ScreenshotColorPickerController::ScreenshotColorPickerController(
    ScreenshotOverlayCoordinator& overlayCoordinator, const ScreenshotGeometryMapper& geometry,
    const ScreenshotDisplaySession& displaySession,
    const snow_shot::platform::PhysicalCursor& physicalCursor)
    : m_overlayCoordinator(overlayCoordinator), m_geometry(geometry),
      m_displaySession(displaySession), m_physicalCursor(physicalCursor) {}

void ScreenshotColorPickerController::reset() {
    m_overlay = nullptr;
    m_suppressed = false;
}

void ScreenshotColorPickerController::hide() const {
    m_overlayCoordinator.hideColorPicker();
}

void ScreenshotColorPickerController::setSuppressed(bool suppressed) {
    if (m_suppressed == suppressed) {
        return;
    }

    m_suppressed = suppressed;
    if (m_suppressed) {
        hide();
    }
}

void ScreenshotColorPickerController::setDisplayMode(ScreenshotColorPickerDisplayMode mode) {
    if (m_displayMode == mode) {
        return;
    }
    m_displayMode = mode;
    if (m_displayMode == ScreenshotColorPickerDisplayMode::AlwaysHide) {
        hide();
    }
}

void ScreenshotColorPickerController::updateForOverlay(
    ScreenshotOverlayWindow* overlay, const QPointF& localPosition,
    const ScreenshotColorPickerContext& context) {
    if (overlay == nullptr || !enabled(context)) {
        hide();
        return;
    }

    const QPointF virtualPosition =
        m_geometry.canvasPositionForOverlayLocalPoint(m_displaySession, overlay, localPosition);
    updateAtCanvasPoint(virtualPosition, context);
}

void ScreenshotColorPickerController::updateAtCanvasPoint(
    const QPointF& point, const ScreenshotColorPickerContext& context, qreal opacity,
    std::optional<QPoint> cursorLogicalPosition) {
    updateAtPhysicalPoint(physicalPositionForCanvasPoint(point), context, opacity,
                          m_geometry.displayForCanvasPoint(m_displaySession, point),
                          cursorLogicalPosition);
}

void ScreenshotColorPickerController::updateAtPhysicalPoint(
    const QPoint& physicalPoint, const ScreenshotColorPickerContext& context, qreal opacity,
    const CapturedDisplayModel* display, std::optional<QPoint> cursorLogicalPosition) {
    if (!enabled(context)) {
        hide();
        return;
    }

    if (!display)
        display = displayForPhysicalPoint(physicalPoint);
    ScreenshotOverlayWindow* overlay = m_displaySession.overlayForDisplay(display);
    if (display == nullptr || overlay == nullptr) {
        hide();
        return;
    }

    const QPoint logicalPoint = logicalPositionForPhysicalPoint(physicalPoint, *display);
    if (m_overlayCoordinator.screenshotUiContainsGlobalPoint(
            cursorLogicalPosition.value_or(logicalPoint))) {
        hide();
        return;
    }
    const QPointF overlayLocalPosition = QPointF(logicalPoint - overlay->geometry().topLeft());
    const qreal pickerOpacity =
        std::min(std::clamp<qreal>(opacity, 0.0, 1.0),
                 opacityForPoint(m_geometry.canvasPositionForPhysicalPoint(*display, physicalPoint),
                                 opacity < 1.0, context));

    m_overlayCoordinator.updateColorPicker(overlay, display->image, display->physicalRect,
                                           physicalPoint, overlayLocalPosition, pickerOpacity);
    m_overlay = overlay;
}

void ScreenshotColorPickerController::updateAtCurrentCursor(
    const ScreenshotColorPickerContext& context) {
    if (!enabled(context)) {
        hide();
        return;
    }

    if (const auto* startup = m_displaySession.startup.get(); startup && startup->anchored()) {
        const CapturedDisplayModel* display = m_displaySession.startupDisplay();
        if (display)
            updateAtPhysicalPoint(startup->physicalPosition, context, 1.0, display);
        else
            hide();
        return;
    }
#ifdef Q_OS_MACOS
    const auto logical = m_physicalCursor.logicalPosition();
    const QPointF point = logical ? *logical : QPointF(QCursor::pos());
    const auto* display = m_geometry.displayForLogicalPoint(m_displaySession, point);
    if (display)
        updateAtPhysicalPoint(m_geometry.physicalPositionForLogicalPoint(m_displaySession, point),
                              context, 1.0, display);
    else
        hide();
#else
    std::optional<QPoint> currentPosition = m_physicalCursor.position();
    if (!currentPosition.has_value() && !m_physicalCursor.isSupported()) {
        currentPosition =
            m_geometry.physicalPositionForLogicalPoint(m_displaySession, QCursor::pos());
    }
    if (!currentPosition.has_value()) {
        hide();
        return;
    }
    updateAtPhysicalPoint(currentPosition.value(), context);
#endif
}

void ScreenshotColorPickerController::updateForSelectionDrag(
    const QPointF& virtualPosition, const ScreenshotColorPickerContext& context) {
    if (!enabled(context) || !context.dragging) {
        return;
    }

    const std::optional<QPointF> anchor =
        screenshotSelectionDragAnchor(context.selectionCanvas, context.dragMode, virtualPosition,
                                      snow_shot::presentation::kScreenshotSelectionMinimumSize);
    if (!anchor.has_value()) {
        updateAtCanvasPoint(virtualPosition, context);
        return;
    }

    std::optional<QPoint> cursorLogicalPosition;
    if (const auto* cursorDisplay =
            m_geometry.displayForCanvasPoint(m_displaySession, virtualPosition))
        cursorLogicalPosition =
            m_geometry.logicalPositionForCanvasPoint(*cursorDisplay, virtualPosition).toPoint();
    updateAtCanvasPoint(anchor.value(), context,
                        screenshotColorPickerOpacity(m_displayMode,
                                                     ScreenshotColorPickerVisibilityState{
                                                         context.intelligentSelecting,
                                                         context.manualSelecting,
                                                         context.movingSelection,
                                                         context.dragging,
                                                         true,
                                                         context.selectionPixels.width() >= 1 &&
                                                             context.selectionPixels.height() >= 1,
                                                         true,
                                                     }),
                        cursorLogicalPosition);
}

void ScreenshotColorPickerController::updateAfterCursorMove(
    const QPoint& physicalPosition, const ScreenshotColorPickerContext& context) {
    if (!enabled(context)) {
        hide();
        return;
    }

    if (context.dragging) {
        updateForSelectionDrag(canvasPositionForPhysicalPoint(physicalPosition), context);
    } else {
        updateAtPhysicalPoint(physicalPosition, context);
    }
}

bool ScreenshotColorPickerController::copyColorToClipboard(
    const ScreenshotColorPickerContext& context) {
    ScreenshotColorPickerWindow* picker = m_overlayCoordinator.colorPicker();
    if (m_overlay == nullptr || picker == nullptr || !picker->hasCurrentColor() ||
        !enabled(context)) {
        return false;
    }

    QApplication::clipboard()->setText(picker->currentColorText());
    return true;
}

bool ScreenshotColorPickerController::cycleFormat(const ScreenshotColorPickerContext& context) {
    if (!enabled(context)) {
        return false;
    }

    ScreenshotColorPickerWindow* picker = m_overlayCoordinator.colorPicker();
    if (picker == nullptr) {
        return false;
    }
    picker->cycleColorFormat();
    return true;
}

bool ScreenshotColorPickerController::enabled(const ScreenshotColorPickerContext& context) const {
    return !m_suppressed && context.active && context.moveToolActive &&
           (context.intelligentSelecting || context.manualSelecting || context.movingSelection);
}

const CapturedDisplayModel*
ScreenshotColorPickerController::displayForPhysicalPoint(const QPointF& point) const {
#ifdef Q_OS_MACOS
    const auto logical = m_physicalCursor.logicalPosition();
    if (const auto* owner = m_geometry.displayForLogicalPoint(
            m_displaySession, logical ? *logical : QPointF(QCursor::pos())))
        return owner;
#endif
    return m_geometry.displayForPhysicalPoint(m_displaySession, point);
}

QPoint ScreenshotColorPickerController::logicalPositionForPhysicalPoint(
    const QPointF& point, const CapturedDisplayModel& display) const {
    return m_geometry.logicalPositionForPhysicalPoint(display, point).toPoint();
}

QPoint ScreenshotColorPickerController::physicalPositionForCanvasPoint(const QPointF& point) const {
    return m_geometry.physicalPositionForCanvasPoint(m_displaySession, point);
}

QPointF
ScreenshotColorPickerController::canvasPositionForPhysicalPoint(const QPointF& point) const {
    if (const auto* owner = displayForPhysicalPoint(point))
        return m_geometry.canvasPositionForPhysicalPoint(*owner, point);
    return point;
}

qreal ScreenshotColorPickerController::opacityForPoint(
    const QPointF& canvasPoint, bool selectionDrag,
    const ScreenshotColorPickerContext& context) const {
    const bool hasSelection =
        context.selectionPixels.width() >= 1 && context.selectionPixels.height() >= 1;
    const QRect toleratedSelection =
        context.selectionPixels.adjusted(-kSelectionOpacityTolerance, -kSelectionOpacityTolerance,
                                         kSelectionOpacityTolerance, kSelectionOpacityTolerance);
    return screenshotColorPickerOpacity(
        m_displayMode, ScreenshotColorPickerVisibilityState{
                           context.intelligentSelecting,
                           context.manualSelecting,
                           context.movingSelection,
                           context.dragging,
                           selectionDrag,
                           hasSelection,
                           hasSelection && QRectF(toleratedSelection).contains(canvasPoint),
                       });
}

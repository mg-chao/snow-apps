#include "snow_shot/presentation/screenshottoolbarpresenter.h"

#include "../capture/screenshotcaptureperfinstrumentation.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshotselectiontoolbarwidget.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"

#include <QSize>
#include <QTimer>

#include <algorithm>

namespace {
constexpr int kSelectionToolbarGap = 4;

[[nodiscard]] bool hasValidSelection(const QRect& selection) {
    return selection.width() >= 1 && selection.height() >= 1;
}

void updateOcrAvailability(ScreenshotOverlayCoordinator& overlayCoordinator, bool available) {
    if (ScreenshotToolbarWindow* toolbar = overlayCoordinator.toolbar()) {
        toolbar->setOcrEnabled(available);
        toolbar->setTableEnabled(available);
        toolbar->setQrEnabled(available);
    }
}
} // namespace

ScreenshotToolbarPresenter::ScreenshotToolbarPresenter(
    ScreenshotOverlayCoordinator& overlayCoordinator, const ScreenshotGeometryMapper& geometry,
    const ScreenshotDisplaySession& displaySession)
    : m_overlayCoordinator(overlayCoordinator), m_geometry(geometry),
      m_displaySession(displaySession) {}

void ScreenshotToolbarPresenter::hideToolbar() {
    m_overlayCoordinator.hideToolbar();
}

void ScreenshotToolbarPresenter::hideMainToolbar() {
    ScreenshotToolbarWindow* toolbar = m_overlayCoordinator.toolbar();
    if (toolbar != nullptr) {
        toolbar->hide();
    }
}

void ScreenshotToolbarPresenter::showToolbar(const ScreenshotToolbarPresentationState& state) {
    m_overlayCoordinator.ensureToolbar();
    updateOcrAvailability(m_overlayCoordinator, state.ocrAvailable);
    updateSelectionToolbarState(state);
    moveToolbar(state);
    m_overlayCoordinator.showToolbar();
}

void ScreenshotToolbarPresenter::hideSelectionToolbar() {
    m_overlayCoordinator.hideSelectionToolbar();
}

void ScreenshotToolbarPresenter::showSelectionToolbar(
    const ScreenshotToolbarPresentationState& state) {
    updateOcrAvailability(m_overlayCoordinator, state.ocrAvailable);
    if (!hasValidSelection(state.selectionPixels) || state.inactive) {
        hideSelectionToolbar();
        return;
    }

    updateSelectionToolbarState(state);
}

void ScreenshotToolbarPresenter::repositionForContentChange(
    const ScreenshotToolbarPresentationState& state) {
    if (!state.selectionToolbarMode) {
        return;
    }

    ScreenshotToolbarWindow* toolbar = m_overlayCoordinator.toolbar();
    if (toolbar == nullptr || !toolbar->isVisible()) {
        return;
    }

    moveToolbar(state);
    moveSelectionToolbar(state);
}

void ScreenshotToolbarPresenter::updateSelectionToolbarState(
    const ScreenshotToolbarPresentationState& state, bool reposition) {
    updateOcrAvailability(m_overlayCoordinator, state.ocrAvailable);
    if (!state.selectionToolbarMode || !hasValidSelection(state.selectionPixels)) {
        m_overlayCoordinator.hideSelectionToolbar();
        return;
    }

    ScreenshotSelectionToolbarWidget* toolbarWidget = m_overlayCoordinator.selectionToolbar();
    if (toolbarWidget == nullptr) {
        return;
    }

    {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("toolbar.set_selection_state");
        toolbarWidget->setSelectionState(
            state.selectionPixels, state.aspectRatioLocked, state.cornerRadius,
            state.shadowWidth,
            state.intelligentSelecting
                ? ScreenshotSelectionToolbarWidget::DisplayMode::SizeOnly
                : ScreenshotSelectionToolbarWidget::DisplayMode::Full);
    }

    if (reposition) {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("toolbar.move_selection_toolbar");
        moveSelectionToolbar(state);
    }
    if (!toolbarWidget->isVisible()) {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("toolbar.show_selection_toolbar");
        m_overlayCoordinator.showSelectionToolbar();
    }
}

void ScreenshotToolbarPresenter::raiseToolbarForCanvasInteraction(
    const ScreenshotToolbarPresentationState& state) {
    ScreenshotToolbarWindow* toolbar = m_overlayCoordinator.toolbar();
    if (!state.editing || toolbar == nullptr || !toolbar->isVisible()) {
        return;
    }

    toolbar->raise();
    m_overlayCoordinator.raiseSelectionToolbar();
    QTimer::singleShot(0, toolbar, [this]() {
        ScreenshotToolbarWindow* delayedToolbar = m_overlayCoordinator.toolbar();
        if (delayedToolbar != nullptr && delayedToolbar->isVisible()) {
            delayedToolbar->raise();
            m_overlayCoordinator.raiseSelectionToolbar();
        }
    });
}

void ScreenshotToolbarPresenter::moveToolbar(const ScreenshotToolbarPresentationState& state) {
    ScreenshotToolbarWindow* toolbar = m_overlayCoordinator.toolbar();
    if (toolbar == nullptr) {
        return;
    }

    const QRect selection = state.selectionPixels;
    const CapturedDisplayModel* display = displayForCanvasPoint(state.selectionCanvas.center());
    if (display == nullptr) {
        display = displayForCanvasRect(selection);
    }

    ScreenshotOverlayWindow* overlay = m_displaySession.overlayForDisplay(display);
    const ScreenshotDisplayPlacementGeometry placementGeometry =
        ScreenshotGeometryMapper::displayPlacementGeometry(
            display, overlay != nullptr ? overlay->geometry() : QRect());
    if (!placementGeometry.valid) {
        return;
    }

    toolbar->setPlacementContext(placementGeometry.screen, placementGeometry.logicalBounds,
                                 placementGeometry.physicalBounds);
    toolbar->prepareForDisplay();

    const ScreenshotToolbarPlacementSnapshot toolbarGeometry = toolbar->placementSnapshot();
    if (!toolbarGeometry.bottom.isValid()) {
        return;
    }

    const ScreenshotHalfOpenRect selectionRect = ScreenshotHalfOpenRect::fromRect(selection);
    const double shadowWidth = static_cast<double>(std::max(0, state.shadowWidth));
    const ScreenshotHalfOpenRect renderedSelectionRect = ScreenshotHalfOpenRect::fromEdges(
        selectionRect.left - shadowWidth, selectionRect.top - shadowWidth,
        selectionRect.right + shadowWidth, selectionRect.bottom + shadowWidth);
    const QPoint bottomRightAnchor =
        display != nullptr
            ? logicalPositionForCanvasPoint(*display, renderedSelectionRect.bottomRight())
            : renderedSelectionRect.bottomRight().toPoint();
    const QPoint topRightAnchor =
        display != nullptr ? logicalPositionForCanvasPoint(
                                 *display, QPointF(renderedSelectionRect.right,
                                                   renderedSelectionRect.top))
                           : QPointF(renderedSelectionRect.right,
                                     renderedSelectionRect.top)
                                 .toPoint();
    const ScreenshotAnchoredToolbarPlacement placement =
        ScreenshotGeometryMapper::anchoredToolbarPlacement(
            bottomRightAnchor, topRightAnchor, toolbarGeometry.bottom, toolbarGeometry.top,
            placementGeometry.logicalBounds, kSelectionToolbarGap);

    m_overlayCoordinator.attachToolbarToOverlay(overlay);
    toolbar->setStyleToolbarAboveMain(placement.usesTopRightPlacement);
    toolbar->resetPositionForSelection(placement.contentPosition);
}

void ScreenshotToolbarPresenter::moveSelectionToolbar(
    const ScreenshotToolbarPresentationState& state) {
    ScreenshotSelectionToolbarWidget* toolbarWidget = m_overlayCoordinator.selectionToolbar();
    if (toolbarWidget == nullptr) {
        return;
    }

    const QRectF selection = state.selectionCanvas;
    if (!selection.isValid() || selection.isEmpty()) {
        return;
    }

    const QSize toolbarSize = toolbarWidget->contentSizeHint();
    const QRect toolbarRect(QPoint(0, 0), toolbarSize);
    const CapturedDisplayModel* display = displayForCanvasRect(selection);
    ScreenshotOverlayWindow* overlay = m_displaySession.overlayForDisplay(display);
    if (display == nullptr || overlay == nullptr) {
        m_overlayCoordinator.hideSelectionToolbar();
        return;
    }

    {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("toolbar.attach_to_overlay");
        m_overlayCoordinator.attachSelectionToolbarToOverlay(overlay);
    }
    const ScreenshotDisplayPlacementGeometry placementGeometry =
        ScreenshotGeometryMapper::displayPlacementGeometry(display, overlay->geometry());
    if (!placementGeometry.valid) {
        m_overlayCoordinator.hideSelectionToolbar();
        return;
    }

    const QPoint topLeftAnchor = logicalPositionForCanvasPoint(*display, selection.topLeft());
    QPoint pos(topLeftAnchor.x(), topLeftAnchor.y() - toolbarSize.height() - kSelectionToolbarGap);

    pos = ScreenshotGeometryMapper::clampContentPositionToRect(pos, toolbarRect,
                                                               placementGeometry.logicalBounds);

    const QPoint overlayOrigin = overlay->geometry().topLeft();
    toolbarWidget->moveContentTo(pos - overlayOrigin);
}

const CapturedDisplayModel*
ScreenshotToolbarPresenter::displayForCanvasPoint(const QPointF& point) const {
    return m_geometry.displayForCanvasPoint(m_displaySession, point);
}

const CapturedDisplayModel*
ScreenshotToolbarPresenter::displayForCanvasRect(const QRectF& rect) const {
    return m_geometry.displayForCanvasRect(m_displaySession, rect);
}

QPoint
ScreenshotToolbarPresenter::logicalPositionForCanvasPoint(const CapturedDisplayModel& display,
                                                          const QPointF& point) const {
    return m_geometry.logicalPositionForCanvasPoint(display, point).toPoint();
}

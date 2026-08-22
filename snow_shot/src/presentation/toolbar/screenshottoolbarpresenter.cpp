#include "snow_shot/presentation/screenshottoolbarpresenter.h"

#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshotselectiontoolbarwindow.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"

#include <QSize>
#include <QTimer>

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
    m_overlayCoordinator.ensureSelectionToolbar();
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
    static_cast<void>(moveSelectionToolbar(state));
}

void ScreenshotToolbarPresenter::updateSelectionToolbarState(
    const ScreenshotToolbarPresentationState& state) {
    updateOcrAvailability(m_overlayCoordinator, state.ocrAvailable);
    if (!state.selectionToolbarMode || !hasValidSelection(state.selectionPixels)) {
        m_overlayCoordinator.hideSelectionToolbar();
        return;
    }

    ScreenshotSelectionToolbarWindow* toolbar = m_overlayCoordinator.ensureSelectionToolbar();
    if (toolbar == nullptr) {
        return;
    }

    toolbar->setSelectionState(
        state.selectionPixels, state.aspectRatioLocked, state.cornerRadius, state.shadowWidth,
        state.intelligentSelecting ? ScreenshotSelectionToolbarWindow::DisplayMode::SizeOnly
                                   : ScreenshotSelectionToolbarWindow::DisplayMode::Full);

    // A selection toolbar must never be revealed before it has an overlay owner and a
    // placement derived from the current displayed selection. Otherwise a pooled or newly
    // created widget can briefly appear at Qt's default position.
    if (!moveSelectionToolbar(state)) {
        return;
    }
    if (!toolbar->isVisible()) {
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

    const QRect toolbarRect = toolbar->bottomPlacementContentRect();
    if (toolbarRect.isEmpty()) {
        return;
    }

    const QRect topRightToolbarRect = toolbar->topRightMainToolbarContentRect();
    const QRect topRightReservedRect = toolbar->topPlacementContentRect();
    const ScreenshotHalfOpenRect selectionRect = ScreenshotHalfOpenRect::fromRect(selection);
    const QPoint bottomRightAnchor =
        display != nullptr ? logicalPositionForCanvasPoint(*display, selectionRect.bottomRight())
                           : selectionRect.bottomRight().toPoint();
    const QPoint topRightAnchor =
        display != nullptr ? logicalPositionForCanvasPoint(
                                 *display, QPointF(selectionRect.right, selectionRect.top))
                           : QPointF(selectionRect.right, selectionRect.top).toPoint();
    const ScreenshotAnchoredToolbarPlacement placement =
        ScreenshotGeometryMapper::anchoredToolbarPlacement(
            bottomRightAnchor, topRightAnchor, toolbarRect, placementGeometry.logicalBounds,
            kSelectionToolbarGap, topRightToolbarRect, topRightReservedRect);

    m_overlayCoordinator.attachToolbarToOverlay(overlay);
    toolbar->setStyleToolbarAboveMain(placement.usesTopRightPlacement);
    toolbar->resetPositionForSelection(placement.contentPosition, toolbar->windowSizeHint());
}

bool ScreenshotToolbarPresenter::moveSelectionToolbar(
    const ScreenshotToolbarPresentationState& state) {
    ScreenshotSelectionToolbarWindow* toolbar = m_overlayCoordinator.selectionToolbar();
    if (toolbar == nullptr) {
        return false;
    }

    const QRectF selection = state.selectionCanvas;
    if (!selection.isValid() || selection.isEmpty()) {
        m_overlayCoordinator.hideSelectionToolbar();
        return false;
    }

    const QSize toolbarSize = toolbar->contentSizeHint();
    const QRect toolbarRect(QPoint(0, 0), toolbarSize);
    const CapturedDisplayModel* display = displayForCanvasRect(selection);
    ScreenshotOverlayWindow* overlay = m_displaySession.overlayForDisplay(display);
    if (display == nullptr || overlay == nullptr) {
        m_overlayCoordinator.hideSelectionToolbar();
        return false;
    }

    m_overlayCoordinator.attachSelectionToolbarToOverlay(overlay);
    const ScreenshotDisplayPlacementGeometry placementGeometry =
        ScreenshotGeometryMapper::displayPlacementGeometry(display, overlay->geometry());
    if (!placementGeometry.valid) {
        m_overlayCoordinator.hideSelectionToolbar();
        return false;
    }

    const QPoint topLeftAnchor = logicalPositionForCanvasPoint(*display, selection.topLeft());
    QPoint pos(topLeftAnchor.x(), topLeftAnchor.y() - toolbarSize.height() - kSelectionToolbarGap);

    pos = ScreenshotGeometryMapper::clampContentPositionToRect(pos, toolbarRect,
                                                               placementGeometry.logicalBounds);

    const QPoint overlayOrigin = overlay->geometry().topLeft();
    toolbar->moveContentTo(pos - overlayOrigin);
    return true;
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

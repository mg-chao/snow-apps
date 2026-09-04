#include "screenshotpinnednativegeometrycontroller.h"

#include <QtGlobal>

namespace {
constexpr int kNativeRoundTripTolerance = 2;

bool validGeometry(const QRect& geometry) {
    return geometry.isValid() && !geometry.isEmpty();
}

bool withinRoundTripTolerance(const QPoint& delta) {
    return qAbs(delta.x()) <= kNativeRoundTripTolerance &&
           qAbs(delta.y()) <= kNativeRoundTripTolerance;
}
} // namespace

bool ScreenshotPinnedNativeGeometryController::initialize(const QRect& geometry) {
    if (!validGeometry(geometry) || m_phase != Phase::Uninitialized) {
        return false;
    }

    m_committedGeometry = geometry;
    m_targetGeometry = geometry;
    m_phase = Phase::Stable;
    m_origin = Origin::InitialPlacement;
    return true;
}

void ScreenshotPinnedNativeGeometryController::beginClosing() {
    m_phase = Phase::Closing;
    m_targetGeometry = m_committedGeometry;
    m_acceptedInteractiveGeometry = false;
}

ScreenshotPinnedNativeGeometryController::Phase
ScreenshotPinnedNativeGeometryController::phase() const {
    return m_phase;
}

QRect ScreenshotPinnedNativeGeometryController::committedGeometry() const {
    return m_committedGeometry;
}

QRect ScreenshotPinnedNativeGeometryController::targetGeometry() const {
    return m_targetGeometry;
}

bool ScreenshotPinnedNativeGeometryController::hasInteractiveTransaction() const {
    return m_phase == Phase::MovePending || m_phase == Phase::Moving ||
           m_phase == Phase::ResizePending || m_phase == Phase::Resizing;
}

bool ScreenshotPinnedNativeGeometryController::hasAcceptedInteractiveGeometry() const {
    return hasInteractiveTransaction() && m_acceptedInteractiveGeometry;
}

bool ScreenshotPinnedNativeGeometryController::beginInteractive(Phase phase, Origin origin) {
    if (m_phase != Phase::Stable || !validGeometry(m_committedGeometry)) {
        return false;
    }

    m_transactionStartGeometry = m_committedGeometry;
    m_targetGeometry = m_committedGeometry;
    m_phase = phase;
    m_origin = origin;
    m_acceptedInteractiveGeometry = false;
    m_dpiChanged = false;
    return true;
}

bool ScreenshotPinnedNativeGeometryController::beginMove(const QPoint& nativeCursorPosition) {
    if (!beginInteractive(Phase::MovePending, Origin::UserMove)) {
        return false;
    }
    m_moveReferenceGeometry = m_transactionStartGeometry;
    m_moveStartCursor = nativeCursorPosition;
    return true;
}

bool ScreenshotPinnedNativeGeometryController::beginResize(
    screenshot_pinned_resize_geometry::DragHandle handle) {
    if (!beginInteractive(Phase::ResizePending, Origin::UserResize)) {
        return false;
    }
    m_resizeHandle = handle;
    return true;
}

void ScreenshotPinnedNativeGeometryController::cancelPendingInteraction() {
    if (!hasInteractiveTransaction()) {
        return;
    }
    m_targetGeometry = m_committedGeometry;
    resetTransaction();
}

QRect ScreenshotPinnedNativeGeometryController::constrainWindowPos(const QRect& proposed,
                                                                   bool moveRequested,
                                                                   bool sizeRequested) const {
    if (m_phase == Phase::Uninitialized || !validGeometry(proposed)) {
        return proposed;
    }

    const QRect desired = validGeometry(m_targetGeometry) ? m_targetGeometry : m_committedGeometry;
    if (!validGeometry(desired)) {
        return proposed;
    }

    QRect constrained = proposed;
    if (moveRequested) {
        constrained.moveTopLeft(desired.topLeft());
    }
    if (sizeRequested) {
        constrained.setSize(desired.size());
    }
    return constrained;
}

QRect ScreenshotPinnedNativeGeometryController::updateMove(const QRect& proposed,
                                                           const QPoint& nativeCursorPosition) {
    if (m_phase == Phase::Stable && validGeometry(proposed)) {
        const QPoint proposedDelta = proposed.topLeft() - m_committedGeometry.topLeft();
        if (!beginMove(nativeCursorPosition - proposedDelta)) {
            return m_targetGeometry;
        }
    }
    if (m_phase != Phase::MovePending && m_phase != Phase::Moving) {
        return m_targetGeometry;
    }

    const QPoint cursorDelta = nativeCursorPosition - m_moveStartCursor;
    if (cursorDelta.isNull()) {
        m_targetGeometry = m_moveReferenceGeometry;
        return m_targetGeometry;
    }

    QRect cursorDerived(m_moveReferenceGeometry.topLeft() + cursorDelta,
                        m_moveReferenceGeometry.size());
    QRect accepted = validGeometry(proposed) ? proposed : cursorDerived;
    if (accepted.size() == cursorDerived.size() &&
        withinRoundTripTolerance(accepted.topLeft() - cursorDerived.topLeft())) {
        accepted = cursorDerived;
    }

    m_targetGeometry = accepted;
    m_phase = Phase::Moving;
    m_acceptedInteractiveGeometry = true;
    return m_targetGeometry;
}

std::optional<QRect> ScreenshotPinnedNativeGeometryController::updateResize(
    const QRect& proposed, screenshot_pinned_resize_geometry::DragHandle handle,
    const QSize& baseline, double minimumScale, double maximumScale) {
    if (m_phase == Phase::Stable && !beginResize(handle)) {
        return std::nullopt;
    }
    if (m_phase != Phase::ResizePending && m_phase != Phase::Resizing) {
        return std::nullopt;
    }

    m_resizeHandle = handle;
    QRect modified;
    if (!screenshot_pinned_resize_geometry::proportionalResizeRect(
            proposed, m_transactionStartGeometry, baseline, m_resizeHandle, minimumScale,
            maximumScale, &modified)) {
        return std::nullopt;
    }

    m_targetGeometry = modified;
    m_phase = Phase::Resizing;
    m_acceptedInteractiveGeometry = true;
    return m_targetGeometry;
}

bool ScreenshotPinnedNativeGeometryController::adoptDpiTarget(
    const QRect& suggested, const std::optional<QPoint>& nativeCursorPosition) {
    if (!validGeometry(suggested) || m_phase == Phase::Closing || m_phase == Phase::Uninitialized) {
        return false;
    }

    // The system owns the geometry of a DPI transition: the suggested rect is
    // accepted verbatim, position included, so the window never re-anchors
    // itself while the monitor change scales it.
    const bool moving = m_phase == Phase::MovePending || m_phase == Phase::Moving;
    if (moving && !nativeCursorPosition.has_value()) {
        return false;
    }
    if (m_phase == Phase::Stable) {
        m_transactionStartGeometry = m_committedGeometry;
        m_phase = Phase::DpiChanging;
        m_origin = Origin::DpiTransition;
    } else if (!moving && m_phase != Phase::ResizePending && m_phase != Phase::Resizing) {
        return false;
    } else {
        m_acceptedInteractiveGeometry = true;
    }

    m_targetGeometry = suggested;
    if (moving) {
        m_moveReferenceGeometry = suggested;
        m_moveStartCursor = nativeCursorPosition.value();
    }
    m_dpiChanged = true;
    return true;
}

bool ScreenshotPinnedNativeGeometryController::beginProgrammatic(const QRect& target,
                                                                 Origin origin) {
    if (!validGeometry(target) || m_phase != Phase::Stable) {
        return false;
    }

    m_transactionStartGeometry = m_committedGeometry;
    m_targetGeometry = target;
    m_phase = Phase::Programmatic;
    m_origin = origin;
    m_acceptedInteractiveGeometry = false;
    m_dpiChanged = false;
    return true;
}

QRect ScreenshotPinnedNativeGeometryController::finishInteractiveTarget() const {
    if (!hasInteractiveTransaction()) {
        return m_targetGeometry;
    }
    return m_acceptedInteractiveGeometry ? m_targetGeometry : m_transactionStartGeometry;
}

ScreenshotPinnedNativeGeometryController::GeometryChange
ScreenshotPinnedNativeGeometryController::commitTarget(bool transactionFinished) {
    GeometryChange change;
    if (!validGeometry(m_targetGeometry)) {
        return change;
    }

    change.previousGeometry = m_committedGeometry;
    change.geometry = m_targetGeometry;
    change.positionChanged = change.previousGeometry.topLeft() != change.geometry.topLeft();
    change.sizeChanged = change.previousGeometry.size() != change.geometry.size();
    change.dpiChanged = m_dpiChanged;
    m_committedGeometry = m_targetGeometry;

    if (transactionFinished) {
        resetTransaction();
    }
    return change;
}

void ScreenshotPinnedNativeGeometryController::prepareRollback() {
    if (m_phase == Phase::Uninitialized || m_phase == Phase::Closing) {
        return;
    }
    m_targetGeometry = m_committedGeometry;
    m_phase = Phase::Programmatic;
    m_origin = Origin::Restoration;
    m_acceptedInteractiveGeometry = false;
    m_dpiChanged = false;
}

ScreenshotPinnedNativeGeometryController::GeometryChange
ScreenshotPinnedNativeGeometryController::finishRollback() {
    return commitTarget(true);
}

void ScreenshotPinnedNativeGeometryController::resetTransaction() {
    m_transactionStartGeometry = {};
    m_targetGeometry = m_committedGeometry;
    m_moveReferenceGeometry = {};
    m_moveStartCursor = {};
    m_phase = Phase::Stable;
    m_origin = Origin::InitialPlacement;
    m_acceptedInteractiveGeometry = false;
    m_dpiChanged = false;
}

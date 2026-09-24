#include "snow_shot/presentation/pinnedgeometry.h"
#include "snow_shot/presentation/screenshotwheelinput.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "pinnedwindowplatform.h"
#include "screenshotpinnednativegeometrycontroller.h"
#include "screenshotpinnedresizegeometry.h"
#include "screenshotpinnedhidetotopcontroller.h"
#include "snow_shot/presentation/screenshotpinnededitcontroller.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "widgets/button.h"
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <utility>

namespace platform = snow_shot::presentation;
namespace resize_geometry = screenshot_pinned_resize_geometry;
namespace {
constexpr int kPreciseWheelZoomStepDelta = 100;
constexpr int kAngleWheelZoomStepDelta = 120;
// Wheels without scroll phases need an idle boundary for a new immediate step.
constexpr quint64 kWheelZoomBurstIntervalMs = 250;

std::optional<int> resizeHandle(const QPointF& p, const QSize& size) {
    constexpr qreal margin = 6;
    if (!QRectF(QPointF(), QSizeF(size)).contains(p))
        return {};
    const bool left = p.x() < margin, right = p.x() >= size.width() - margin;
    const bool top = p.y() < margin, bottom = p.y() >= size.height() - margin;
    using H = resize_geometry::DragHandle;
    if (left && top)
        return int(H::TopLeft);
    if (right && top)
        return int(H::TopRight);
    if (right && bottom)
        return int(H::BottomRight);
    if (left && bottom)
        return int(H::BottomLeft);
    if (left)
        return int(H::Left);
    if (right)
        return int(H::Right);
    if (top)
        return int(H::Top);
    if (bottom)
        return int(H::Bottom);
    return {};
}
Qt::CursorShape resizeCursor(int handle) {
    using H = resize_geometry::DragHandle;
    switch (H(handle)) {
    case H::TopLeft:
    case H::BottomRight:
        return Qt::SizeFDiagCursor;
    case H::TopRight:
    case H::BottomLeft:
        return Qt::SizeBDiagCursor;
    case H::Top:
    case H::Bottom:
        return Qt::SizeVerCursor;
    case H::Left:
    case H::Right:
        return Qt::SizeHorCursor;
    }
    return Qt::ArrowCursor;
}
} // namespace

void ScreenshotPinnedWindow::reconcilePlatformEnvironment(bool layoutChanged) {
    if (!m_platform || !m_platform->usesControlledInteraction() || !m_presented || m_closing ||
        m_platformApplying)
        return;
    m_platformRecoveryPending = m_platformRecoveryPending || layoutChanged;
    if (m_platformReconciliationPending)
        return;
    m_platformReconciliationPending = true;
    QTimer::singleShot(0, this, [this] {
        m_platformReconciliationPending = false;
        if (m_closing || !m_nativeGeometryController || !m_platformPlacement || m_platformApplying)
            return;
        if (m_interactionPlacement) {
            const auto displays = QGuiApplication::screens();
            const bool originExists =
                std::any_of(displays.begin(), displays.end(), [this](QScreen* display) {
                    return !m_interactionPlacement->displaySerial.isEmpty()
                               ? display->serialNumber() == m_interactionPlacement->displaySerial
                               : display->name() == m_interactionPlacement->displayName;
                });
            if (originExists)
                return;
            endControlledInteraction(false);
        }
        const bool recover = std::exchange(m_platformRecoveryPending, false);
        auto placement = *m_platformPlacement;
        QScreen* target = platform::pinnedDisplay(placement, screen());
        if (!target)
            return;
        if (recover)
            placement = platform::recoverPinnedPlacement(placement, *target);
        m_platformApplying = true;
        const bool applied = m_platform->applyStablePlacement(placement, target);
        m_platformApplying = false;
        if (!applied)
            return;
        m_platformPlacement = m_platform->placement();
        if (!m_platformPlacement)
            return;
        target = platform::pinnedDisplay(*m_platformPlacement, target);
        if (m_preThumbnailPlacement.isValid()) {
            if (recover)
                m_preThumbnailPlacement =
                    platform::recoverPinnedPlacement(m_preThumbnailPlacement, *target);
            m_preThumbnailNativeGeometry = platform::pinnedWindowRect(
                m_preThumbnailPlacement, *platform::pinnedDisplay(m_preThumbnailPlacement, target));
        }
        const QRect targetRect = platform::pinnedWindowRect(*m_platformPlacement, *target);
        if (m_nativeGeometryController->beginProgrammatic(
                targetRect, ScreenshotPinnedNativeGeometryController::Origin::DpiTransition)) {
            static_cast<void>(m_nativeGeometryController->commitTarget());
        }
        m_preserveScaleForSettledGeometry = true;
        updateCanvasViewport();
        updateControlsGeometry();
        if (hideToTopActive())
            m_hideToTop->reconcileScreen(screenshot_pinned_hide_to_top::screenGeometry(target));
        if (m_clickThroughActive && !updateClickThroughExitButtonGeometry())
            static_cast<void>(setClickThroughMode(false));
        if (m_editController)
            m_editController->updatePlacement();
        schedulePersistence();
    });
}

bool ScreenshotPinnedWindow::beginControlledInteraction(const QPointF& desktopPosition,
                                                        std::optional<int> handle) {
    if (m_interactionPlacement || m_closing || m_geometryAnimating || !screen())
        return false;
    const auto placement = m_platform->placement();
    if (!placement)
        return false;
    if (handle ? !interactiveResizingEnabled() : (!windowDragEnabled() && !m_clickThroughActive))
        return false;
    exitHideToTop();
    const QRect pixels = platform::pinnedWindowRect(*placement, *screen());
    const bool begun =
        handle ? m_nativeGeometryController->beginResize(resize_geometry::DragHandle(*handle))
               : m_nativeGeometryController->beginMove(pixels.topLeft());
    if (!begun)
        return false;
    if (!handle && m_recognitionContent && (m_ocrMode || m_hiddenTextSelection)) {
        m_recognitionContent->clearOcrSelection();
    }
    resetPinnedGestures();
    m_interactionPlacement = placement;
    m_interactionResizeHandle = handle;
    m_interactionPointer = desktopPosition;
    m_interactionAnchor =
        (desktopPosition - platform::pinnedDesktopRect(*placement, *screen()).topLeft()) *
        platform::pinnedGeometryScale(screen()->devicePixelRatio());
    m_systemSizingActive = handle.has_value();
    m_windowDragActive = !handle.has_value();
    if (m_editController) {
        if (handle && m_editController->editMode())
            static_cast<void>(m_editController->beginTemporaryResizeWindowTool());
        m_editController->beginNativeWindowInteraction();
    }
    if (!m_clickThroughActive)
        static_cast<void>(m_platform->activate());
    m_interactionGrabber = QWidget::mouseGrabber();
    if (!m_interactionGrabber) {
        m_interactionGrabber = this;
        grabMouse();
    }
    qApp->installEventFilter(this);
    setWindowDragCursor(handle ? resizeCursor(*handle) : Qt::ClosedHandCursor);
    return true;
}

void ScreenshotPinnedWindow::updateControlledInteraction(const QPointF& desktopPosition) {
    if (!m_interactionPlacement || !screen())
        return;
    auto placement = *m_interactionPlacement;
    QScreen* target = screen();
    if (!m_interactionResizeHandle) {
        if (QScreen* underPointer = platform::pinnedDisplayAt(desktopPosition))
            target = underPointer;
        placement =
            platform::pinnedPlacementAtPointer(placement, platform::pinnedDisplayGeometry(*target),
                                               desktopPosition, m_interactionAnchor);
    } else {
        QScreen* originScreen = platform::pinnedDisplay(placement, screen());
        const QRect origin = platform::pinnedWindowRect(placement, *originScreen);
        QRect proposed = origin;
        const QPoint delta = ((desktopPosition - m_interactionPointer) *
                              platform::pinnedGeometryScale(originScreen->devicePixelRatio()))
                                 .toPoint();
        using H = resize_geometry::DragHandle;
        const H handle = H(*m_interactionResizeHandle);
        if (handle == H::Left || handle == H::TopLeft || handle == H::BottomLeft)
            proposed.setLeft(origin.left() + delta.x());
        if (handle == H::Right || handle == H::TopRight || handle == H::BottomRight)
            proposed.setRight(origin.right() + delta.x());
        if (handle == H::Top || handle == H::TopLeft || handle == H::TopRight)
            proposed.setTop(origin.top() + delta.y());
        if (handle == H::Bottom || handle == H::BottomLeft || handle == H::BottomRight)
            proposed.setBottom(origin.bottom() + delta.y());
        QRect resized;
        if (!resize_geometry::proportionalResizeRect(proposed, origin, orientedInitialWindowSize(),
                                                     handle, .1, 5., &resized))
            return;
        target = originScreen;
        placement = platform::pinnedPlacement(resized, *target);
    }
    placement.displayName = target->name();
    placement.displaySerial = target->serialNumber();
    bool settled = false;
    const int attempts = placement.units == platform::PinnedGeometryUnits::LogicalPixels ? 1 : 3;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        m_platformApplying = true;
        const bool applied = m_platform->applyPlacement(placement, target);
        m_platformApplying = false;
        if (!applied)
            break;
        m_platformPlacement = m_platform->placement();
        if (!m_platformPlacement)
            break;
        target = platform::pinnedDisplay(*m_platformPlacement, target);
        if (m_platformPlacement->windowSize == placement.windowSize) {
            settled = true;
            break;
        }
        if (!m_interactionResizeHandle) {
            placement = platform::pinnedPlacementAtPointer(*m_interactionPlacement,
                                                           platform::pinnedDisplayGeometry(*target),
                                                           desktopPosition, m_interactionAnchor);
        } else {
            placement.displayName = target->name();
            placement.displaySerial = target->serialNumber();
            placement.position = m_platformPlacement->position;
        }
    }
    if (!settled) {
        endControlledInteraction(true);
        return;
    }
    static_cast<void>(m_nativeGeometryController->acceptInteractiveGeometry(
        platform::pinnedWindowRect(*m_platformPlacement, *target)));
    if (m_systemSizingActive)
        setEffectiveScale(100. * m_platformPlacement->windowSize.width() /
                              std::max(1, orientedInitialWindowSize().width()),
                          true);
    updateCanvasViewport();
    if (m_clickThroughActive)
        static_cast<void>(updateClickThroughExitButtonGeometry());
}

void ScreenshotPinnedWindow::endControlledInteraction(bool cancel) {
    if (!m_interactionPlacement)
        return;
    const auto original = *m_interactionPlacement;
    m_interactionPlacement.reset();
    m_interactionResizeHandle.reset();
    qApp->removeEventFilter(this);
    auto grabber = m_interactionGrabber;
    m_interactionGrabber = nullptr;
    if (grabber && QWidget::mouseGrabber() == grabber)
        grabber->releaseMouse();
    if (cancel) {
        m_nativeGeometryController->cancelPendingInteraction();
        if (QScreen* target = platform::pinnedDisplay(original, screen())) {
            m_platformApplying = true;
            const bool restored = m_platform->applyStablePlacement(original, target);
            m_platformApplying = false;
            if (restored)
                m_platformPlacement = m_platform->placement();
            else
                QTimer::singleShot(0, this, &QWidget::close);
        }
    } else {
        static_cast<void>(m_nativeGeometryController->commitTarget());
    }
    m_systemSizingActive = false;
    m_windowDragActive = false;
    clearWindowDragCursor();
    if (m_editController) {
        m_editController->endTemporaryResizeWindowTool();
        m_editController->endNativeWindowInteraction();
    }
    if (!m_closing) {
        updateCanvasViewport();
        adoptSettledNativeScale();
        refreshControlsPointerPresence();
        if (m_platformRecoveryPending)
            reconcilePlatformEnvironment();
        schedulePersistence();
    }
}

bool ScreenshotPinnedWindow::handleControlledPointer(QObject* watched, QEvent* event) {
    if (!m_platform->usesControlledInteraction() || !event)
        return false;
    if (m_controlledEscapeRelease && event->type() == QEvent::KeyRelease &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        m_controlledEscapeRelease = false;
        event->accept();
        return true;
    }
    if (m_interactionPlacement) {
        if (event->type() == QEvent::KeyPress &&
            static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            m_controlledEscapeRelease = true;
            endControlledInteraction(true);
            event->accept();
            return true;
        }
        if ((event->type() == QEvent::UngrabMouse && watched == m_interactionGrabber) ||
            (event->type() == QEvent::WindowDeactivate && watched == this) ||
            (event->type() == QEvent::Hide && watched == this)) {
            endControlledInteraction(true);
            return false;
        }
        if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            // A release can be lost during input/session interruption. The next
            // move's button state ends ownership; its hover position is not a
            // drag target. Actual release events still apply their final position.
            if (event->type() == QEvent::MouseMove && !mouse->buttons().testFlag(Qt::LeftButton)) {
                endControlledInteraction(true);
                return false;
            }
            if (event->type() == QEvent::MouseButtonRelease && mouse->button() != Qt::LeftButton)
                return false;
            updateControlledInteraction(mouse->globalPosition());
            if (event->type() == QEvent::MouseButtonRelease)
                endControlledInteraction(false);
            return true;
        }
        return false;
    }
    const bool moveControl = watched == m_clickThroughMoveButton.get();
    if (watched != this && watched != m_canvas && watched != m_recognitionContent && !moveControl)
        return false;
    if (event->type() != QEvent::MouseMove && event->type() != QEvent::MouseButtonPress)
        return false;
    auto* mouse = static_cast<QMouseEvent*>(event);
    const QPointF local = windowPositionForEvent(watched, mouse->position());
    const auto handle =
        interactiveResizingEnabled() && !moveControl ? resizeHandle(local, size()) : std::nullopt;
    if (event->type() == QEvent::MouseMove) {
        if (handle) {
            setWindowDragCursor(resizeCursor(*handle));
            return true;
        }
        return false;
    }
    if (mouse->button() == Qt::LeftButton &&
        (handle || moveControl || windowDragEnabledAt(local.toPoint())))
        return beginControlledInteraction(mouse->globalPosition(), handle);
    return false;
}

void ScreenshotPinnedWindow::resetPinnedGestures() {
    m_scrollWheelRemainder = 0;
    m_scrollWheelDirection = 0;
    m_scrollWheelStepDelta = 0;
    m_scrollWheelTimestamp = 0;
    m_scrollOpacity = 0;
    m_pinchActive = false;
}

bool ScreenshotPinnedWindow::handlePinnedGesture(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide)
        resetPinnedGestures();
    if (m_clickThroughActive || m_interactionPlacement || m_geometryAnimating || m_closing) {
        resetPinnedGestures();
        return false;
    }
    if (watched != this && watched != m_canvas && watched != m_recognitionContent)
        return false;
    if ((!m_ocrMode && m_editController && m_editController->editMode()) ||
        (m_ocrMode &&
         (!m_recognitionSession || !m_recognitionSession->originalImageTranslationActive())))
        return false;
    if (event->type() == QEvent::NativeGesture) {
        auto* gesture = static_cast<QNativeGestureEvent*>(event);
        if (gesture->gestureType() == Qt::BeginNativeGesture) {
            resetPinnedGestures();
            m_pinchActive = true;
            return true;
        }
        if (gesture->gestureType() == Qt::EndNativeGesture) {
            m_pinchActive = false;
            return true;
        }
        if (gesture->gestureType() != Qt::ZoomNativeGesture)
            return false;
        const double factor = 1. + gesture->value();
        if (std::isfinite(factor) && factor > 0) {
            const QPointF local = windowPositionForEvent(watched, gesture->position());
            applyWheelScale(std::clamp(m_scalePercent * factor, 10., 500.),
                            nativePositionForWindowPosition(local));
        }
        return true;
    }
    if (event->type() != QEvent::Wheel)
        return false;
    auto* wheel = static_cast<QWheelEvent*>(event);
    if (wheel->phase() == Qt::ScrollMomentum || m_pinchActive)
        return true;
    if (wheel->phase() == Qt::ScrollBegin)
        resetPinnedGestures();
    if (wheel->phase() == Qt::ScrollEnd) {
        resetPinnedGestures();
        wheel->accept();
        return true;
    }
    const bool precise = platform::usesPreciseWheelDelta(*wheel);
    const int delta = precise ? wheel->pixelDelta().y() : wheel->angleDelta().y();
    if (wheel->modifiers().testFlag(Qt::ControlModifier)) {
        m_scrollWheelRemainder = 0;
        m_scrollWheelDirection = 0;
        if (!precise) {
            m_scrollOpacity = 0;
            return false;
        }
        if (m_scrollOpacity == 0)
            m_scrollOpacity = m_opacityPercent;
        m_scrollOpacity = std::clamp(m_scrollOpacity + delta * 5. / 120., 25., 100.);
        setOpacityPercent(qRound(m_scrollOpacity));
    } else if (delta != 0) {
        m_scrollOpacity = 0;
        const int stepDelta = precise ? kPreciseWheelZoomStepDelta : kAngleWheelZoomStepDelta;
        const int direction = delta > 0 ? 1 : -1;
        const quint64 timestamp = wheel->timestamp();
        const bool newBurst = wheel->phase() == Qt::NoScrollPhase &&
                              timestamp > m_scrollWheelTimestamp &&
                              timestamp - m_scrollWheelTimestamp >= kWheelZoomBurstIntervalMs;
        if (direction != m_scrollWheelDirection || stepDelta != m_scrollWheelStepDelta ||
            newBurst) {
            // Advance on the first point, then once per full step of continued movement.
            // Keeping this credit in the accumulator makes event coalescing irrelevant.
            m_scrollWheelRemainder = direction * (stepDelta - 1);
        }
        m_scrollWheelDirection = direction;
        m_scrollWheelStepDelta = stepDelta;
        m_scrollWheelTimestamp = timestamp;
        const qint64 accumulated = qint64(m_scrollWheelRemainder) + delta;
        const int steps = int(accumulated / stepDelta);
        m_scrollWheelRemainder = int(accumulated % stepDelta);
        if (steps != 0) {
            applyWheelScaleSteps(steps, nativePositionForWindowPosition(
                                            windowPositionForEvent(watched, wheel->position())));
        }
    }
    wheel->accept();
    return true;
}

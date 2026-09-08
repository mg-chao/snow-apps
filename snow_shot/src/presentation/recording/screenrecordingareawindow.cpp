#include "snow_shot/presentation/screenrecordingareawindow.h"

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "screenrecordinggeometry.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QScreen>
#include <QShowEvent>
#include <QWheelEvent>

#if defined(Q_OS_WIN) || defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
constexpr QColor kIdleColor(0x40, 0x96, 0xff);
constexpr QColor kRecordingColor(0xf5, 0x22, 0x2d);
constexpr QColor kPausedColor(0xfa, 0xad, 0x14);
constexpr qreal kResizeHitWidth = 6.0;
} // namespace

ScreenRecordingAreaWindow::ScreenRecordingAreaWindow(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                          Qt::MSWindowsFixedSizeDialogHint | Qt::NoDropShadowWindowHint),
      m_canvasRuntime(std::make_unique<SnowCanvasRuntime>()),
      m_canvas(new SnowCanvasWidget(*m_canvasRuntime, this)) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setMouseTracking(true);
    installEventFilter(this);
    m_canvas->setMouseTracking(true);
    m_canvas->setObjectName(QStringLiteral("screenRecordingCanvas"));
    m_canvas->setAttribute(Qt::WA_TranslucentBackground, true);
    m_canvas->setAttribute(Qt::WA_NoSystemBackground, true);
    m_canvas->setAttribute(Qt::WA_OpaquePaintEvent, false);
    m_canvas->setClearBackgroundEnabled(false);
    m_canvas->setWheelZoomEnabled(false);
    m_canvas->setCanvasContentVisible(true);
    m_canvas->installEventFilter(this);
    snow_shot::presentation::applyScreenshotCanvasToolStyles(
        *m_canvas, snow_shot::presentation::screenshotCanvasToolStyleDefaults());
    applyInputMode();
}

ScreenRecordingAreaWindow::~ScreenRecordingAreaWindow() = default;

QRect ScreenRecordingAreaWindow::physicalRegion() const {
    return m_physicalRegion;
}

void ScreenRecordingAreaWindow::setPhysicalRegion(const QRect& region) {
    if (!region.isValid() || region.isEmpty()) {
        return;
    }
    const bool opensNewRegion = m_physicalRegion.isValid() && m_physicalRegion != region;
    m_physicalRegion = region;
    QScreen* screen = ScreenshotGeometryMapper::screenForPhysicalRect(region);
    const QRectF logicalRegion =
        ScreenshotGeometryMapper::logicalRectFForPhysicalRect(region, screen);
    const qreal scale = screen != nullptr ? screen->devicePixelRatio() : 1.0;
    const auto frameGeometry =
        snow_shot::presentation::recording::screenRecordingAreaFrameGeometry(logicalRegion, scale);
    m_frameRect = frameGeometry.frameRect;
    m_selectionRect = frameGeometry.selectionRect;
    m_paddingWidth = frameGeometry.paddingWidth;
    setGeometry(frameGeometry.windowGeometry);
    if (m_canvas != nullptr) {
        m_canvas->setGeometry(m_selectionRect.toAlignedRect());
        m_canvas->raise();
    }
    if (opensNewRegion && m_canvasRuntime != nullptr) {
        static_cast<void>(m_canvasRuntime->clearDocumentPreservingViewports());
    }
    update();
}

void ScreenRecordingAreaWindow::setRecordingState(ScreenshotToolPalette::RecordingState state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    cancelRegionGesture();
    applyInputMode();
}

void ScreenRecordingAreaWindow::setInputMode(InputMode mode) {
    if (m_inputMode == mode) {
        applyInputMode();
        return;
    }
    m_inputMode = mode;
    cancelRegionGesture();
    m_gestureInProgress = false;
    applyInputMode();
}

ScreenRecordingAreaWindow::InputMode ScreenRecordingAreaWindow::inputMode() const {
    return m_inputMode;
}

void ScreenRecordingAreaWindow::setDrawingBlocked(bool blocked) {
    if (m_drawingBlocked == blocked) {
        return;
    }
    m_drawingBlocked = blocked;
    cancelRegionGesture();
    m_gestureInProgress = false;
    applyInputMode();
}

bool ScreenRecordingAreaWindow::drawingBlocked() const {
    return m_drawingBlocked;
}

SnowCanvasWidget* ScreenRecordingAreaWindow::canvas() const {
    return m_canvas;
}

QRect ScreenRecordingAreaWindow::canvasGeometry() const {
    return m_canvas != nullptr ? m_canvas->geometry() : QRect();
}

bool ScreenRecordingAreaWindow::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == this || watched == m_canvas) && event != nullptr &&
        handleRegionMouseEvent(watched, event)) {
        return true;
    }
    if (watched != m_canvas || event == nullptr || m_inputMode != InputMode::Drawing ||
        m_drawingBlocked) {
        return QWidget::eventFilter(watched, event);
    }

    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            m_gestureInProgress = true;
        }
        break;
    }
    case QEvent::MouseButtonRelease: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            m_gestureInProgress = false;
        }
        break;
    }
    case QEvent::Wheel: {
        auto* wheel = static_cast<QWheelEvent*>(event);
        const int delta =
            !wheel->pixelDelta().isNull() ? wheel->pixelDelta().y() : wheel->angleDelta().y();
        if (delta != 0) {
            emit drawingWheelRequested(delta > 0 ? 1 : -1);
        }
        wheel->accept();
        return true;
    }
    case QEvent::KeyPress: {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() != Qt::Key_Escape || key->isAutoRepeat()) {
            break;
        }
        if (m_canvas->hasActiveTextEditing()) {
            static_cast<void>(m_canvas->cancelActiveTextEditing());
            key->accept();
            return true;
        }
        if (m_gestureInProgress) {
            m_gestureInProgress = false;
            return false;
        }
        emit drawingDeactivationRequested();
        key->accept();
        return true;
    }
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void ScreenRecordingAreaWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    applyInputMode();
}

void ScreenRecordingAreaWindow::applyInputMode() {
    const bool drawing = m_inputMode == InputMode::Drawing && !m_drawingBlocked;
    const bool interactive = drawing || regionEditingEnabled();
    setAttribute(Qt::WA_TransparentForMouseEvents, !interactive);
    setAttribute(Qt::WA_ShowWithoutActivating, !interactive);
    setFocusPolicy(interactive ? Qt::StrongFocus : Qt::NoFocus);
    if (m_canvas != nullptr) {
        m_canvas->setInteractionEnabled(drawing);
        m_canvas->setFocusPolicy(drawing ? Qt::StrongFocus : Qt::NoFocus);
    }
    applyNativePassThrough(!interactive);
    update();
    if (drawing && isVisible() && m_canvas != nullptr) {
        activateWindow();
        m_canvas->setFocus(Qt::OtherFocusReason);
    } else if (m_canvas != nullptr) {
        m_canvas->clearFocus();
    }
}

bool ScreenRecordingAreaWindow::regionEditingEnabled() const {
    return m_state == ScreenshotToolPalette::RecordingState::Idle && !m_drawingBlocked;
}

Qt::Edges ScreenRecordingAreaWindow::resizeEdgesAt(const QPointF& position) const {
    if (!regionEditingEnabled() || !rect().contains(position.toPoint())) {
        return {};
    }
    Qt::Edges edges;
    const qreal horizontalHitWidth = qMin(kResizeHitWidth, m_selectionRect.width() / 2.0);
    const qreal verticalHitWidth = qMin(kResizeHitWidth, m_selectionRect.height() / 2.0);
    if (position.x() <= m_selectionRect.left() + horizontalHitWidth) {
        edges |= Qt::LeftEdge;
    } else if (position.x() >= m_selectionRect.right() - horizontalHitWidth) {
        edges |= Qt::RightEdge;
    }
    if (position.y() <= m_selectionRect.top() + verticalHitWidth) {
        edges |= Qt::TopEdge;
    } else if (position.y() >= m_selectionRect.bottom() - verticalHitWidth) {
        edges |= Qt::BottomEdge;
    }
    return edges;
}

void ScreenRecordingAreaWindow::cancelRegionGesture() {
    m_regionGestureInProgress = false;
    m_resizeEdges = {};
    if (QWidget::mouseGrabber() == this) {
        releaseMouse();
    }
    unsetCursor();
    if (m_canvas != nullptr) {
        m_canvas->unsetCursor();
    }
}

bool ScreenRecordingAreaWindow::handleRegionMouseEvent(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate ||
        event->type() == QEvent::UngrabMouse) {
        cancelRegionGesture();
        return false;
    }
    if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseMove &&
        event->type() != QEvent::MouseButtonRelease) {
        return false;
    }
    if (!regionEditingEnabled() || m_gestureInProgress) {
        return false;
    }
    auto* mouse = static_cast<QMouseEvent*>(event);
    const QPointF position =
        mouse->position() + (watched == m_canvas ? QPointF(m_canvas->pos()) : QPointF());
    const Qt::Edges edges = resizeEdgesAt(position);
    const bool movable =
        m_inputMode == InputMode::RegionEditing && m_selectionRect.contains(position);
    if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton &&
        (edges != Qt::Edges() || movable)) {
        QScreen* screen = ScreenshotGeometryMapper::screenForPhysicalRect(m_physicalRegion);
        if (screen == nullptr) {
            return false;
        }
        m_regionGestureBounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
        if (!m_regionGestureBounds.contains(m_physicalRegion)) {
            // Preserve editable selections spanning monitors, including their existing extent
            // if the display topology changed after the selection was opened.
            for (const QScreen* attachedScreen : QGuiApplication::screens()) {
                m_regionGestureBounds = m_regionGestureBounds.united(
                    ScreenshotGeometryMapper::physicalRectForScreen(*attachedScreen));
            }
            m_regionGestureBounds = m_regionGestureBounds.united(m_physicalRegion);
        }
        m_regionGestureScale = screen->devicePixelRatio();
        m_regionGestureStart = mouse->globalPosition();
        m_regionGestureRect = m_physicalRegion;
        m_resizeEdges = edges;
        m_regionGestureInProgress = true;
        grabMouse();
        mouse->accept();
        return true;
    }
    if (m_regionGestureInProgress &&
        (event->type() == QEvent::MouseMove ||
         (event->type() == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton))) {
        const QPointF logicalDelta = mouse->globalPosition() - m_regionGestureStart;
        const QPoint delta(qRound(logicalDelta.x() * m_regionGestureScale),
                           qRound(logicalDelta.y() * m_regionGestureScale));
        QRect region = m_regionGestureRect;
        const QRect bounds = m_regionGestureBounds;
        if (m_resizeEdges == Qt::Edges()) {
            region.moveLeft(qBound(bounds.left(), region.left() + delta.x(),
                                   bounds.right() - region.width() + 1));
            region.moveTop(qBound(bounds.top(), region.top() + delta.y(),
                                  bounds.bottom() - region.height() + 1));
        } else {
            if (m_resizeEdges.testFlag(Qt::LeftEdge)) {
                region.setLeft(
                    qBound(bounds.left(), region.left() + delta.x(), region.right() - 1));
            }
            if (m_resizeEdges.testFlag(Qt::RightEdge)) {
                region.setRight(
                    qBound(region.left() + 1, region.right() + delta.x(), bounds.right()));
            }
            if (m_resizeEdges.testFlag(Qt::TopEdge)) {
                region.setTop(qBound(bounds.top(), region.top() + delta.y(), region.bottom() - 1));
            }
            if (m_resizeEdges.testFlag(Qt::BottomEdge)) {
                region.setBottom(
                    qBound(region.top() + 1, region.bottom() + delta.y(), bounds.bottom()));
            }
        }
        if (region != m_physicalRegion) {
            setPhysicalRegion(region);
            emit physicalRegionChanged(region);
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            cancelRegionGesture();
        }
        mouse->accept();
        return true;
    }
    if (event->type() == QEvent::MouseMove) {
        Qt::CursorShape cursor = Qt::ArrowCursor;
        if ((edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::TopEdge)) ||
            (edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::BottomEdge))) {
            cursor = Qt::SizeFDiagCursor;
        } else if ((edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::TopEdge)) ||
                   (edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::BottomEdge))) {
            cursor = Qt::SizeBDiagCursor;
        } else if (edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge)) {
            cursor = Qt::SizeHorCursor;
        } else if (edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge)) {
            cursor = Qt::SizeVerCursor;
        } else if (movable) {
            cursor = Qt::SizeAllCursor;
        }
        if (edges != Qt::Edges() || movable) {
            setCursor(cursor);
            m_canvas->setCursor(cursor);
            mouse->accept();
            return true;
        }
        unsetCursor();
        m_canvas->unsetCursor();
    }
    return false;
}

bool ScreenRecordingAreaWindow::nativeEvent(const QByteArray& eventType, void* message,
                                            qintptr* result) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const auto* nativeMessage = static_cast<MSG*>(message);
    if (nativeMessage != nullptr && result != nullptr && nativeMessage->message == WM_NCHITTEST &&
        m_inputMode == InputMode::PassThrough && regionEditingEnabled() &&
        !m_regionGestureInProgress) {
        RECT bounds{};
        if (GetWindowRect(nativeMessage->hwnd, &bounds)) {
            const int x = static_cast<short>(LOWORD(nativeMessage->lParam));
            const int y = static_cast<short>(HIWORD(nativeMessage->lParam));
            const QPointF local((x - bounds.left) / devicePixelRatioF(),
                                (y - bounds.top) / devicePixelRatioF());
            if (resizeEdgesAt(local) == Qt::Edges()) {
                *result = HTTRANSPARENT;
                return true;
            }
        }
    }
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

void ScreenRecordingAreaWindow::applyNativePassThrough(bool enabled) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND handle = reinterpret_cast<HWND>(winId());
    if (handle == nullptr) {
        return;
    }
    const LONG_PTR previous = GetWindowLongPtrW(handle, GWL_EXSTYLE);
    LONG_PTR next = enabled ? previous | WS_EX_TRANSPARENT : previous & ~WS_EX_TRANSPARENT;
    next = enabled ? next | WS_EX_NOACTIVATE : next & ~WS_EX_NOACTIVATE;
    if (next != previous) {
        SetWindowLongPtrW(handle, GWL_EXSTYLE, next);
        SetWindowPos(handle, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE |
                         SWP_FRAMECHANGED);
    }
#else
    Q_UNUSED(enabled);
#endif
}

void ScreenRecordingAreaWindow::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QColor color = kIdleColor;
    if (m_state == ScreenshotToolPalette::RecordingState::Recording) {
        color = kRecordingColor;
    } else if (m_state == ScreenshotToolPalette::RecordingState::Paused) {
        color = kPausedColor;
    }

    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    if ((m_inputMode == InputMode::Drawing && !m_drawingBlocked) ||
        (m_inputMode == InputMode::RegionEditing && regionEditingEnabled())) {
        // Windows passes mouse input through zero-alpha pixels in layered windows.
        painter.fillRect(m_selectionRect, QColor(0, 0, 0, 2));
    } else if (regionEditingEnabled()) {
        // Keep a usable resize target while the center remains transparent to input.
        const QRectF interior = m_selectionRect.adjusted(kResizeHitWidth, kResizeHitWidth,
                                                         -kResizeHitWidth, -kResizeHitWidth);
        painter.setClipRegion(QRegion(rect()) - QRegion(interior.toAlignedRect()));
        painter.fillRect(m_selectionRect, QColor(0, 0, 0, 2));
        painter.setClipping(false);
    }
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const auto border = snow_shot::presentation::recording::screenRecordingAreaBorderGeometry(
        m_frameRect, m_selectionRect, m_paddingWidth);
    painter.fillRect(border.top, color);
    painter.fillRect(border.bottom, color);
    painter.fillRect(border.left, color);
    painter.fillRect(border.right, color);
}

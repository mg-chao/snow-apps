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
} // namespace

ScreenRecordingAreaWindow::ScreenRecordingAreaWindow(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                          Qt::MSWindowsFixedSizeDialogHint | Qt::NoDropShadowWindowHint),
      m_canvasRuntime(std::make_unique<SnowCanvasRuntime>()),
      m_canvas(new SnowCanvasWidget(*m_canvasRuntime, this)) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    m_canvas->setObjectName(QStringLiteral("screenRecordingCanvas"));
    m_canvas->setAttribute(Qt::WA_TranslucentBackground, true);
    m_canvas->setAttribute(Qt::WA_NoSystemBackground, true);
    m_canvas->setClearBackgroundEnabled(false);
    m_canvas->setWheelZoomEnabled(false);
    m_canvas->setCanvasContentVisible(true);
    m_canvas->installEventFilter(this);
    snow_shot::presentation::applyScreenshotCanvasToolStyles(
        *m_canvas, snow_shot::presentation::screenshotCanvasToolStyleDefaults());
    applyInputMode();
}

ScreenRecordingAreaWindow::~ScreenRecordingAreaWindow() = default;

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
    update();
}

void ScreenRecordingAreaWindow::setInputMode(InputMode mode) {
    if (m_inputMode == mode) {
        applyInputMode();
        return;
    }
    m_inputMode = mode;
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
    setAttribute(Qt::WA_TransparentForMouseEvents, !drawing);
    setAttribute(Qt::WA_ShowWithoutActivating, !drawing);
    setFocusPolicy(drawing ? Qt::StrongFocus : Qt::NoFocus);
    if (m_canvas != nullptr) {
        m_canvas->setInteractionEnabled(drawing);
        m_canvas->setFocusPolicy(drawing ? Qt::StrongFocus : Qt::NoFocus);
    }
    applyNativePassThrough(!drawing);
    if (drawing && isVisible() && m_canvas != nullptr) {
        activateWindow();
        m_canvas->setFocus(Qt::OtherFocusReason);
    } else if (m_canvas != nullptr) {
        m_canvas->clearFocus();
    }
}

void ScreenRecordingAreaWindow::applyNativePassThrough(bool enabled) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND handle = reinterpret_cast<HWND>(winId());
    if (handle == nullptr) {
        return;
    }
    const LONG_PTR previous = GetWindowLongPtrW(handle, GWL_EXSTYLE);
    const LONG_PTR mask = WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    const LONG_PTR next = enabled ? previous | mask : previous & ~mask;
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
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const auto border = snow_shot::presentation::recording::screenRecordingAreaBorderGeometry(
        m_frameRect, m_selectionRect, m_paddingWidth);
    painter.fillRect(border.top, color);
    painter.fillRect(border.bottom, color);
    painter.fillRect(border.left, color);
    painter.fillRect(border.right, color);
}

#include "snow_shot/presentation/screenrecordingareawindow.h"

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "recordingcountdownoverlay.h"
#include "screenrecordinggeometry.h"
#ifdef Q_OS_MACOS
#include "snow_shot/platform/screenshotnative.h"
#import <AppKit/AppKit.h>
#endif
#include "screenrecordingperfinstrumentation.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QEvent>
#include <QEnterEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QScreen>
#include <QShowEvent>
#include <QWheelEvent>
#include <QWindow>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QCloseEvent>
#include <QScopedValueRollback>

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
constexpr int kFrameInset = snow_shot::presentation::recording::screenRecordingPhysicalFrameInset;

QRect nativeClientGeometry(const QWidget& window) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (QGuiApplication::platformName() == QStringLiteral("windows") &&
        window.internalWinId() != 0) {
        const HWND handle = reinterpret_cast<HWND>(window.internalWinId());
        RECT client{};
        POINT origin{};
        if (GetClientRect(handle, &client) && ClientToScreen(handle, &origin)) {
            return QRect(origin.x, origin.y, client.right - client.left,
                         client.bottom - client.top);
        }
    }
#else
    Q_UNUSED(window);
#endif
    return {};
}
} // namespace

ScreenRecordingAreaWindow::ScreenRecordingAreaWindow(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                          Qt::NoDropShadowWindowHint),
      m_canvasRuntime(std::make_unique<SnowCanvasRuntime>(
          SnowCanvasRuntimeConfig{snow_shot::presentation::screenshotCanvasToolStyleDefaults()})),
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
    static_cast<void>(m_canvas->setCanvasTool(SnowCanvasTool::Select));
    m_canvas->installEventFilter(this);
    snow_shot::presentation::applyScreenshotCanvasToolStyles(
        *m_canvas, snow_shot::presentation::screenshotCanvasToolStyleDefaults());
    applyQuickSelectionPreferences();
    auto& applicationStorage = snow_shot::storage::ApplicationStorage::instance();
    if (applicationStorage.isInitialized()) {
        connect(&applicationStorage.configuration(),
                &snow_shot::storage::ConfigurationStore::valueChanged, this,
                [this](const QString& key, const QJsonValue&) {
                    if (key == QStringLiteral("drawing/quick_selection_disabled_tools")) {
                        applyQuickSelectionPreferences();
                    }
                });
    }
    applyInputMode();
}

ScreenRecordingAreaWindow::~ScreenRecordingAreaWindow() = default;

void ScreenRecordingAreaWindow::applyQuickSelectionPreferences() {
    auto& applicationStorage = snow_shot::storage::ApplicationStorage::instance();
    if (!applicationStorage.isInitialized() || m_canvasRuntime == nullptr) {
        return;
    }
    const auto tools = snow_shot::presentation::screenshotQuickSelectionDisabledTools(
        snow_shot::storage::DrawingSettings().quickSelectionDisabledTools());
    if (!m_canvasRuntime->setQuickSelectionDisabledTools(tools)) {
        qWarning("Failed to apply screen recording drawing quick-selection preferences");
    }
}

QRect ScreenRecordingAreaWindow::recordingRegion() const {
    return m_recordingRegion;
}

void ScreenRecordingAreaWindow::setRecordingRegion(const QRect& region) {
    if (region.width() < 2 || region.height() < 2) {
        return;
    }
    const QScopedValueRollback<bool> settingRegion(m_settingRegion, true);
    const bool opensNewRegion = m_recordingRegion.isValid() && m_recordingRegion != region;
    m_recordingRegion = region;
#ifdef Q_OS_MACOS
    const QRectF logicalRegion(region);
    const qreal scale = 1.0;
#else
    QScreen* screen = ScreenshotGeometryMapper::screenForPhysicalRect(region);
    const QRectF logicalRegion =
        ScreenshotGeometryMapper::logicalRectFForPhysicalRect(region, screen);
    const qreal scale = screen != nullptr ? screen->devicePixelRatio() : 1.0;
#endif
    const auto frameGeometry =
        snow_shot::presentation::recording::screenRecordingAreaFrameGeometry(logicalRegion, scale);
    m_frameRect = frameGeometry.frameRect;
    m_selectionRect = frameGeometry.selectionRect;
    m_paddingWidth = frameGeometry.paddingWidth;
    setGeometry(frameGeometry.windowGeometry);
    m_physicalInsets = QMarginsF(m_selectionRect.left() * scale, m_selectionRect.top() * scale,
                                 (width() - m_selectionRect.right()) * scale,
                                 (height() - m_selectionRect.bottom()) * scale);
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (QGuiApplication::platformName() == QStringLiteral("windows") && screen != nullptr) {
        // Qt owns even the initial native placement. Its integer logical geometry can
        // add a physical pixel of outer padding at fractional DPI; keep that padding
        // outside the exact screenshot selection instead of competing with Qt via SetWindowPos.
        const QPoint logicalOrigin = screen->geometry().topLeft();
        const QPoint physicalOrigin =
            ScreenshotGeometryMapper::physicalRectForScreen(*screen).topLeft();
        const QPoint windowOrigin(physicalOrigin.x() + qRound((x() - logicalOrigin.x()) * scale),
                                  physicalOrigin.y() + qRound((y() - logicalOrigin.y()) * scale));
        const QPoint inset = region.topLeft() - windowOrigin;
        m_physicalInsets =
            QMarginsF(inset.x(), inset.y(), qRound(width() * scale) - inset.x() - region.width(),
                      qRound(height() * scale) - inset.y() - region.height());
    }
#endif
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
    cancelRegionInteraction();
    applyInputMode();
}

void ScreenRecordingAreaWindow::setInputMode(InputMode mode) {
    if (m_inputMode == mode) {
        applyInputMode();
        return;
    }
    m_inputMode = mode;
    cancelRegionInteraction();
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
    cancelRegionInteraction();
    m_gestureInProgress = false;
    applyInputMode();
}

bool ScreenRecordingAreaWindow::drawingBlocked() const {
    return m_drawingBlocked;
}

void ScreenRecordingAreaWindow::startCountdown(int seconds) {
    if (m_countdownOverlay == nullptr) {
        m_countdownOverlay =
            new snow_shot::presentation::recording::RecordingCountdownOverlay(this);
    }
    m_countdownOverlay->start(seconds);
    layoutCountdownOverlay();
    // The indicator must stay above the annotation canvas.
    m_countdownOverlay->raise();
    update();
}

void ScreenRecordingAreaWindow::updateCountdown(qint64 remainingMilliseconds) {
    if (m_countdownOverlay == nullptr) {
        return;
    }
    m_countdownOverlay->setRemainingMilliseconds(remainingMilliseconds);
}

void ScreenRecordingAreaWindow::clearCountdown() {
    if (m_countdownOverlay == nullptr) {
        return;
    }
    m_countdownOverlay->clear();
    update();
}

bool ScreenRecordingAreaWindow::countdownActive() const {
    return m_countdownOverlay != nullptr && m_countdownOverlay->active();
}

void ScreenRecordingAreaWindow::layoutCountdownOverlay() {
    if (m_countdownOverlay == nullptr) {
        return;
    }
    const QPointF center = m_selectionRect.center();
    const int side = snow_shot::presentation::recording::screenRecordingCountdownIndicatorSize;
    m_countdownOverlay->setGeometry(qRound(center.x()) - side / 2, qRound(center.y()) - side / 2,
                                    side, side);
}

QColor ScreenRecordingAreaWindow::inputSurfaceColor() const {
    const bool interactive =
        (m_inputMode == InputMode::Drawing && !m_drawingBlocked) || regionEditingEnabled();
    // Windows passes mouse input through zero-alpha pixels in layered windows.
    return interactive ? QColor(0, 0, 0, 2) : QColor(Qt::transparent);
}

SnowCanvasWidget* ScreenRecordingAreaWindow::canvas() const {
    return m_canvas;
}

QRect ScreenRecordingAreaWindow::canvasGeometry() const {
    return m_canvas != nullptr ? m_canvas->geometry() : QRect();
}

bool ScreenRecordingAreaWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == this && event != nullptr && event->type() == QEvent::Hide) {
        cancelRegionInteraction();
    }
#ifdef Q_OS_MACOS
    if ((watched == this || watched == m_canvas) && event != nullptr && regionEditingEnabled()) {
        if (event->type() == QEvent::Enter) {
            updateRegionCursor(mapFromGlobal(static_cast<QEnterEvent*>(event)->globalPosition()));
        } else if (event->type() == QEvent::MouseButtonPress ||
                   event->type() == QEvent::MouseButtonDblClick) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                m_regionDragOrigin = mouse->globalPosition().toPoint();
                m_regionDragRect = m_recordingRegion;
                m_regionDragEdges = resizeEdgesAt(mapFromGlobal(m_regionDragOrigin));
                beginRegionInteraction();
                return true;
            }
        } else if (event->type() == QEvent::MouseMove) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (m_regionInteractionActive) {
                const QPoint delta = mouse->globalPosition().toPoint() - m_regionDragOrigin;
                QRect next = m_regionDragRect;
                if (!m_regionDragEdges)
                    next.translate(delta);
                if (m_regionDragEdges.testFlag(Qt::LeftEdge))
                    next.setLeft(qMin(next.right() - 1, next.left() + delta.x()));
                if (m_regionDragEdges.testFlag(Qt::RightEdge))
                    next.setRight(qMax(next.left() + 1, next.right() + delta.x()));
                if (m_regionDragEdges.testFlag(Qt::TopEdge))
                    next.setTop(qMin(next.bottom() - 1, next.top() + delta.y()));
                if (m_regionDragEdges.testFlag(Qt::BottomEdge))
                    next.setBottom(qMax(next.top() + 1, next.bottom() + delta.y()));
                const QPoint offset = m_selectionRect.topLeft().toPoint();
                setGeometry(
                    QRect(next.topLeft() - offset,
                          next.size() +
                              QSize(qRound(m_physicalInsets.left() + m_physicalInsets.right()),
                                    qRound(m_physicalInsets.top() + m_physicalInsets.bottom()))));
                synchronizeWindowGeometry();
                return true;
            }
            updateRegionCursor(mapFromGlobal(mouse->globalPosition().toPoint()));
        } else if (event->type() == QEvent::MouseButtonRelease && m_regionInteractionActive) {
            finishRegionInteraction();
            return true;
        }
    }
#endif
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
    SNOW_SHOT_RECORDING_PERF_MILESTONE("area.show_event");
    applyInputMode();
    scheduleGeometrySynchronization();
}

void ScreenRecordingAreaWindow::applyInputMode() {
    m_canvas->clearCursorForLayer(SnowCanvasCursorLayer::Host);
    unsetCursor();
    if (windowHandle() != nullptr) {
        windowHandle()->unsetCursor();
    }
    const bool drawing = m_inputMode == InputMode::Drawing && !m_drawingBlocked;
    const bool interactive = drawing || regionEditingEnabled();
    setAttribute(Qt::WA_TransparentForMouseEvents, !interactive);
    setAttribute(Qt::WA_ShowWithoutActivating, !interactive);
    setFocusPolicy(interactive ? Qt::StrongFocus : Qt::NoFocus);
    if (m_canvas != nullptr) {
        m_canvas->setInteractionEnabled(drawing);
        m_canvas->setFocusPolicy(drawing ? Qt::StrongFocus : Qt::NoFocus);
        m_canvas->update();
    }
    applyNativePassThrough(!interactive);
    update();
    if (!drawing && m_canvas != nullptr) {
        m_canvas->clearFocus();
    }
    activateInput();
}

bool ScreenRecordingAreaWindow::activateInput() {
    const bool drawing = m_inputMode == InputMode::Drawing && !m_drawingBlocked;
    if (!isVisible() || (!drawing && !regionEditingEnabled())) {
        return false;
    }
    activateWindow();
    if (drawing && m_canvas != nullptr) {
        m_canvas->setFocus(Qt::OtherFocusReason);
    } else {
        setFocus(Qt::OtherFocusReason);
    }
    return true;
}

void ScreenRecordingAreaWindow::updateRegionCursor(const QPointF& position) {
    const auto edges = resizeEdgesAt(position);
    const QCursor cursor(
        (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge))
            ? Qt::SizeFDiagCursor
        : (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge))
            ? Qt::SizeBDiagCursor
        : edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge) ? Qt::SizeHorCursor
        : edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge) ? Qt::SizeVerCursor
                                                                        : Qt::SizeAllCursor);
    // The canvas retains its tool cursor even with drawing disabled. Match pinned
    // windows by owning the host layer and the native surface during region editing.
    m_canvas->setCursorForLayer(SnowCanvasCursorLayer::Host, cursor);
    setCursor(cursor);
    if (windowHandle() != nullptr) {
        windowHandle()->setCursor(cursor);
    }
}

bool ScreenRecordingAreaWindow::regionEditingEnabled() const {
    return m_inputMode == InputMode::RegionEditing &&
           m_state == ScreenshotToolPalette::RecordingState::Idle && !m_drawingBlocked;
}

Qt::Edges ScreenRecordingAreaWindow::resizeEdgesAt(const QPointF& position) const {
    if (!regionEditingEnabled() || !rect().contains(position.toPoint())) {
        return {};
    }
    Qt::Edges edges;
    const qreal horizontalHitWidth = qMin(kResizeHitWidth, m_selectionRect.width() / 4.0);
    const qreal verticalHitWidth = qMin(kResizeHitWidth, m_selectionRect.height() / 4.0);
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

void ScreenRecordingAreaWindow::beginRegionInteraction() {
    if (!regionEditingEnabled() || m_regionInteractionActive) {
        return;
    }
    activateInput();
    m_regionInteractionActive = true;
    emit regionInteractionStarted();
}

void ScreenRecordingAreaWindow::cancelRegionInteraction() {
    if (!m_regionInteractionActive) {
        return;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        SendMessageW(reinterpret_cast<HWND>(internalWinId()), WM_CANCELMODE, 0, 0);
        // The system loop owns completion, including cancellation. Keep the toolbar
        // hidden until WM_EXITSIZEMOVE, after any final geometry has been applied.
        return;
    }
#endif
    finishRegionInteraction();
}

void ScreenRecordingAreaWindow::finishRegionInteraction() {
    if (!m_regionInteractionActive) {
        return;
    }
    synchronizeWindowGeometry();
    m_regionInteractionActive = false;
    emit regionInteractionFinished();
}

void ScreenRecordingAreaWindow::layoutSelection() {
#ifdef Q_OS_MACOS
    const qreal scale = 1.0;
#else
    const qreal scale = devicePixelRatioF();
#endif
    QRectF selection = QRectF(rect()).marginsRemoved(
        QMarginsF(m_physicalInsets.left() / scale, m_physicalInsets.top() / scale,
                  m_physicalInsets.right() / scale, m_physicalInsets.bottom() / scale));
    const qreal frameInset = kFrameInset / scale;
    QRectF frame = selection.adjusted(-frameInset, -frameInset, frameInset, frameInset);
    const qreal padding = 1.0 / scale;
    const auto nativeGeometry = snow_shot::presentation::recording::screenRecordingObservedGeometry(
        nativeClientGeometry(*this), scale, m_physicalInsets.toMargins());
    if (nativeGeometry.recordingRegion.isValid()) {
        selection = nativeGeometry.selectionRect;
        frame = nativeGeometry.frameRect;
    }
    if (m_selectionRect == selection && m_frameRect == frame && m_paddingWidth == padding) {
        return;
    }
    m_selectionRect = selection;
    m_frameRect = frame;
    m_paddingWidth = padding;
    m_canvas->setGeometry(m_selectionRect.toAlignedRect());
    layoutCountdownOverlay();
    update();
}

void ScreenRecordingAreaWindow::scheduleGeometrySynchronization() {
    if (m_geometrySyncPending) {
        return;
    }
    m_geometrySyncPending = true;
    QMetaObject::invokeMethod(
        this,
        [this]() {
            m_geometrySyncPending = false;
            SNOW_SHOT_RECORDING_PERF_MILESTONE("area.geometry_sync_entered");
            synchronizeWindowGeometry();
            SNOW_SHOT_RECORDING_PERF_MILESTONE("area.geometry_synchronized");
        },
        Qt::QueuedConnection);
}

void ScreenRecordingAreaWindow::synchronizeWindowGeometry() {
    if (m_settingRegion || !m_recordingRegion.isValid()) {
        return;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    // QWidget delivers its initial move/resize before applying geometry to a hidden HWND.
    // The deferred observation after Show/WM_WINDOWPOSCHANGED sees the committed client pixels.
    if (QGuiApplication::platformName() == QStringLiteral("windows") &&
        !IsWindowVisible(reinterpret_cast<HWND>(internalWinId()))) {
        return;
    }
#endif
    layoutSelection();
#ifdef Q_OS_MACOS
    const qreal scale = 1.0;
#else
    const qreal scale = devicePixelRatioF();
#endif
#ifdef Q_OS_MACOS
    Q_UNUSED(scale);
    const QRect region((QPointF(mapToGlobal(QPoint())) + m_selectionRect.topLeft()).toPoint(),
                       m_selectionRect.size().toSize());
#else
    const QScreen* currentScreen = screen();
    const QPoint logicalOrigin =
        currentScreen != nullptr ? currentScreen->geometry().topLeft() : QPoint();
    const QPoint physicalOrigin =
        currentScreen != nullptr
            ? ScreenshotGeometryMapper::physicalRectForScreen(*currentScreen).topLeft()
            : QPoint();
    const QPointF selectionOrigin = QPointF(mapToGlobal(QPoint())) + m_selectionRect.topLeft();
    QRect region(physicalOrigin.x() + qRound((selectionOrigin.x() - logicalOrigin.x()) * scale),
                 physicalOrigin.y() + qRound((selectionOrigin.y() - logicalOrigin.y()) * scale),
                 qRound(m_selectionRect.width() * scale), qRound(m_selectionRect.height() * scale));
    const auto nativeGeometry = snow_shot::presentation::recording::screenRecordingObservedGeometry(
        nativeClientGeometry(*this), scale, m_physicalInsets.toMargins());
    if (nativeGeometry.recordingRegion.isValid()) {
        region = nativeGeometry.recordingRegion;
    }
#endif
    if (region.width() >= 2 && region.height() >= 2 && region != m_recordingRegion) {
        m_recordingRegion = region;
        emit recordingRegionChanged(region);
    }
}

bool ScreenRecordingAreaWindow::event(QEvent* event) {
    const bool handled = QWidget::event(event);
    // A translucent backing-store flush can round the native surface by one pixel
    // after the last resize event, without delivering another logical resize.
    if (event->type() == QEvent::UpdateRequest || event->type() == QEvent::DevicePixelRatioChange ||
        event->type() == QEvent::ScreenChangeInternal) {
        synchronizeWindowGeometry();
    }
    return handled;
}

void ScreenRecordingAreaWindow::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    synchronizeWindowGeometry();
}

void ScreenRecordingAreaWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    synchronizeWindowGeometry();
}

void ScreenRecordingAreaWindow::closeEvent(QCloseEvent* event) {
    event->ignore();
    emit closeRequested();
}

bool ScreenRecordingAreaWindow::nativeEvent(const QByteArray& eventType, void* message,
                                            qintptr* result) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const auto* msg = static_cast<MSG*>(message);
    if (msg != nullptr && result != nullptr) {
        if (msg->message == WM_WINDOWPOSCHANGED) {
            scheduleGeometrySynchronization();
        }
        if (msg->message == WM_SYSCOMMAND && (!regionEditingEnabled() || !isVisible()) &&
            ((msg->wParam & 0xfff0) == SC_MOVE || (msg->wParam & 0xfff0) == SC_SIZE)) {
            *result = 0;
            return true;
        }
        if (msg->message == WM_ENTERSIZEMOVE) {
            beginRegionInteraction();
        } else if (msg->message == WM_EXITSIZEMOVE) {
            finishRegionInteraction();
        } else if (msg->message == WM_GETMINMAXINFO && regionEditingEnabled()) {
            auto* limits = reinterpret_cast<MINMAXINFO*>(msg->lParam);
            limits->ptMinTrackSize = {
                qRound(m_physicalInsets.left() + m_physicalInsets.right()) + 2,
                qRound(m_physicalInsets.top() + m_physicalInsets.bottom()) + 2};
            *result = 0;
            return true;
        } else if (msg->message == WM_NCHITTEST && !regionEditingEnabled()) {
            *result =
                m_inputMode == InputMode::Drawing && !m_drawingBlocked ? HTCLIENT : HTTRANSPARENT;
            return true;
        } else if (msg->message == WM_NCHITTEST && regionEditingEnabled()) {
            POINT point{static_cast<short>(LOWORD(msg->lParam)),
                        static_cast<short>(HIWORD(msg->lParam))};
            ScreenToClient(msg->hwnd, &point);
            const Qt::Edges edges = resizeEdgesAt(
                QPointF(point.x / devicePixelRatioF(), point.y / devicePixelRatioF()));
            const bool left = edges.testFlag(Qt::LeftEdge);
            const bool right = edges.testFlag(Qt::RightEdge);
            const bool top = edges.testFlag(Qt::TopEdge);
            const bool bottom = edges.testFlag(Qt::BottomEdge);
            *result = top      ? (left    ? HTTOPLEFT
                                  : right ? HTTOPRIGHT
                                          : HTTOP)
                      : bottom ? (left    ? HTBOTTOMLEFT
                                  : right ? HTBOTTOMRIGHT
                                          : HTBOTTOM)
                      : left   ? HTLEFT
                      : right  ? HTRIGHT
                               : HTCAPTION;
            return true;
        } else if (msg->message == WM_NCLBUTTONDOWN && regionEditingEnabled()) {
            Qt::Edges edges;
            switch (msg->wParam) {
            case HTLEFT:
                edges = Qt::LeftEdge;
                break;
            case HTRIGHT:
                edges = Qt::RightEdge;
                break;
            case HTTOP:
                edges = Qt::TopEdge;
                break;
            case HTBOTTOM:
                edges = Qt::BottomEdge;
                break;
            case HTTOPLEFT:
                edges = Qt::TopEdge | Qt::LeftEdge;
                break;
            case HTTOPRIGHT:
                edges = Qt::TopEdge | Qt::RightEdge;
                break;
            case HTBOTTOMLEFT:
                edges = Qt::BottomEdge | Qt::LeftEdge;
                break;
            case HTBOTTOMRIGHT:
                edges = Qt::BottomEdge | Qt::RightEdge;
                break;
            default:
                break;
            }
            if (windowHandle() != nullptr && (edges != Qt::Edges() || msg->wParam == HTCAPTION)) {
                const bool started = edges != Qt::Edges() ? windowHandle()->startSystemResize(edges)
                                                          : windowHandle()->startSystemMove();
                if (!started) {
                    finishRegionInteraction();
                }
                *result = 0;
                return true;
            }
        }
    }
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

void ScreenRecordingAreaWindow::applyNativePassThrough(bool enabled) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (QGuiApplication::platformName() != QStringLiteral("windows")) {
        return;
    }
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
#elif defined(Q_OS_MACOS)
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;
    NSView* view = reinterpret_cast<NSView*>(winId());
    NSWindow* window = view.window;
    window.ignoresMouseEvents = enabled;
    snow_shot::platform::configureScreenshotOverlayWindow(this);
#else
    Q_UNUSED(enabled);
#endif
}

void ScreenRecordingAreaWindow::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    SNOW_SHOT_RECORDING_PERF_MILESTONE("area.first_paint_begin");
    SNOW_SHOT_RECORDING_PERF_COUNTER("area.paints", 1);
    QColor color = kIdleColor;
    if (countdownActive()) {
        // The waiting state reuses the paused border colour until the delayed
        // start swaps it for the recording colour.
        color = kPausedColor;
    } else if (m_state == ScreenshotToolPalette::RecordingState::Recording) {
        color = kRecordingColor;
    } else if (m_state == ScreenshotToolPalette::RecordingState::Paused) {
        color = kPausedColor;
    }

    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    // The clear above already left the selection fully transparent. Repainting it
    // with a zero-alpha surface colour would rewrite most of the window for no
    // visible change, which dominates the first paint on large regions.
    const QColor surface = inputSurfaceColor();
    if (surface.alpha() != 0) {
        painter.fillRect(m_selectionRect, surface);
    }
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const auto border = snow_shot::presentation::recording::screenRecordingAreaBorderGeometry(
        m_frameRect, m_selectionRect, m_paddingWidth);
    painter.fillRect(border.top, color);
    painter.fillRect(border.bottom, color);
    painter.fillRect(border.left, color);
    painter.fillRect(border.right, color);
    SNOW_SHOT_RECORDING_PERF_MILESTONE("area.first_paint_end");
}

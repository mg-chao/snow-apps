#include "snow_shot/shortcuts/shortcutbinding.h"
#include "snow_shot/presentation/windowcloseshortcut.h"
#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/screenshotstylebinding.h"

#include <numbers>

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "recordingcountdownoverlay.h"
#include "screenrecordinggeometry.h"
#include "../resizegeometry.h"
#include "recordingregiondraghandle.h"
#include <QApplication>
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
#include <QRegion>
#include <QtMath>
#include <functional>
#include <utility>

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
constexpr int kMoveButtonGap = 8;
} // namespace

// Observe pointer input without creating another native surface. On Windows the
// low-level callback runs before hit testing, so even opaque annotations pass
// clicks through when the pointer is outside the controls.
class RecordingRegionInputRouter final {
  public:
    explicit RecordingRegionInputRouter(std::function<void(const QPoint&)> changed)
        : m_changed(std::move(changed)) {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() != QStringLiteral("windows"))
            return;
        s_observers.append(this);
        if (!s_mouseHook) {
            s_mouseHook =
                SetWindowsHookExW(WH_MOUSE_LL, mouseCallback, GetModuleHandleW(nullptr), 0);
            if (!s_mouseHook)
                qWarning("Failed to observe recording region pointer input: %lu", GetLastError());
        }
#elif defined(Q_OS_MACOS)
        if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
            return;
        constexpr NSEventMask mask = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged |
                                     NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged |
                                     NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown |
                                     NSEventMaskOtherMouseDown | NSEventMaskLeftMouseUp |
                                     NSEventMaskRightMouseUp | NSEventMaskOtherMouseUp;
        m_localMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                               handler:^NSEvent*(NSEvent* event) {
                                                                 m_changed(QCursor::pos());
                                                                 return event;
                                                               }];
        m_globalMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:mask
                                                                 handler:^(NSEvent*) {
                                                                   m_changed(QCursor::pos());
                                                                 }];
#endif
    }

    ~RecordingRegionInputRouter() {
#ifdef Q_OS_WIN
        s_observers.removeAll(this);
        if (s_observers.isEmpty() && s_mouseHook) {
            UnhookWindowsHookEx(s_mouseHook);
            s_mouseHook = nullptr;
        }
#elif defined(Q_OS_MACOS)
        if (m_localMonitor)
            [NSEvent removeMonitor:m_localMonitor];
        if (m_globalMonitor)
            [NSEvent removeMonitor:m_globalMonitor];
#endif
    }

  private:
    std::function<void(const QPoint&)> m_changed;
#ifdef Q_OS_WIN
    static LRESULT CALLBACK mouseCallback(int code, WPARAM message, LPARAM data) {
        if (code == HC_ACTION) {
            const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
            for (RecordingRegionInputRouter* observer : s_observers)
                observer->m_changed(QPoint(mouse->pt.x, mouse->pt.y));
        }
        return CallNextHookEx(s_mouseHook, code, message, data);
    }
    static inline QList<RecordingRegionInputRouter*> s_observers;
    static inline HHOOK s_mouseHook = nullptr;
#elif defined(Q_OS_MACOS)
    id m_localMonitor = nil;
    id m_globalMonitor = nil;
#endif
};

namespace {
QPoint regionPointer(const QPointF& logicalPosition) {
#if defined(Q_OS_WIN)
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        POINT point{};
        if (GetCursorPos(&point))
            return QPoint(point.x, point.y);
    }
#endif
#ifdef Q_OS_MACOS
    return logicalPosition.toPoint();
#else
    QScreen* screen = QGuiApplication::screenAt(logicalPosition.toPoint());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    return screen
               ? ScreenshotGeometryMapper::physicalRectForScreen(*screen).topLeft() +
                     ((logicalPosition - screen->geometry().topLeft()) * screen->devicePixelRatio())
                         .toPoint()
               : logicalPosition.toPoint();
#endif
}

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
#ifdef Q_OS_MACOS
    m_canvas->setCommandKeyResolver(
        [](const QKeyEvent& event) { return snow_shot::shortcuts::commandKey(event); });
#endif
    snow_shot::presentation::installWindowCloseShortcut(this, [this] { close(); });
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setMouseTracking(true);
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
    connect(m_canvas, &SnowCanvasWidget::angleAdjustmentTargetChanged, this,
            [this]() { m_angleWheelSteps.reset(); });
    connect(m_canvas, &SnowCanvasWidget::activeToolChanged, this,
            [this]() { m_angleWheelSteps.reset(); });
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

ScreenRecordingAreaWindow::~ScreenRecordingAreaWindow() {
    m_regionDragActive = false;
    m_regionInputRouter.reset();
    m_canvas->removeEventFilter(this);
    if (m_regionDragHandle) {
        disconnect(m_regionDragHandle, nullptr, this, nullptr);
        m_regionDragHandle->stopDragging();
    }
    if (QWidget::mouseGrabber() == this)
        releaseMouse();
}

void ScreenRecordingAreaWindow::setTrimming(bool enabled) {
    if (m_trimming == enabled)
        return;
    cancelRegionInteraction();
    m_trimming = enabled;
    if (enabled)
        setFixedSize(size());
    else {
        setMinimumSize(0, 0);
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    }
    m_canvas->setVisible(!enabled);
    if (!enabled)
        m_previewFrame = {};
    applyInputMode();
}

void ScreenRecordingAreaWindow::setPreviewFrame(const QImage& frame) {
    if (!m_trimming)
        return;
    if (m_previewFrame.constBits() == frame.constBits() && m_previewFrame.size() == frame.size())
        return;
    m_previewFrame = frame;
    update(m_selectionRect.toAlignedRect());
}

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

void ScreenRecordingAreaWindow::setRecordingRegion(const QRect& requestedRegion) {
    cancelRegionInteraction();
    const QRect region =
        snow_shot::presentation::recording::screenRecordingNormalizedRegion(requestedRegion);
    if (!region.isValid() || region.isEmpty()) {
        return;
    }
    const bool opensNewRegion = m_recordingRegion.isValid() && m_recordingRegion != region;
    placeRecordingRegion(region);
    if (opensNewRegion && m_canvasRuntime != nullptr)
        static_cast<void>(m_canvasRuntime->clearDocumentPreservingViewports());
}

void ScreenRecordingAreaWindow::placeRecordingRegion(const QRect& region) {
    const QScopedValueRollback<bool> settingRegion(m_settingRegion, true);
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
    if (m_trimming)
        setFixedSize(frameGeometry.windowGeometry.size());
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
    layoutCountdownOverlay();
    layoutRegionDragHandle();
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
    m_angleWheelSteps.reset();
    cancelRegionInteraction();
    m_gestureButton = Qt::NoButton;
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
    m_gestureButton = Qt::NoButton;
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

void ScreenRecordingAreaWindow::layoutRegionDragHandle() {
    const QPoint corner(qCeil(m_selectionRect.left()), qCeil(m_selectionRect.top()));
    const QRect bounds = QRect(corner, QSize(qFloor(m_selectionRect.right()) - corner.x(),
                                             qFloor(m_selectionRect.bottom()) - corner.y()))
                             .intersected(rect());
    const bool editable =
        regionEditingEnabled() && isVisible() && m_recordingRegion.isValid() && !bounds.isEmpty();
    if (m_trimming || !editable) {
        if (m_regionDragHandle)
            m_regionDragHandle->hide();
        m_regionInputRouter.reset();
        return;
    }
    if (!m_regionDragHandle) {
        m_regionDragHandle = new RecordingRegionDragHandle(this);
        connect(m_regionDragHandle, &RecordingRegionDragHandle::dragStarted, this,
                [this](const QPointF& point) { beginRegionDrag(regionPointer(point), {}); });
        connect(m_regionDragHandle, &RecordingRegionDragHandle::dragMoved, this,
                [this](const QPointF& point) { updateRegionDrag(regionPointer(point)); });
        connect(m_regionDragHandle, &RecordingRegionDragHandle::dragFinished, this,
                [this](const QPointF& point) {
                    updateRegionDrag(regionPointer(point));
                    finishRegionInteraction();
                });
        connect(m_regionDragHandle, &RecordingRegionDragHandle::dragCancelled, this,
                &ScreenRecordingAreaWindow::cancelRegionInteraction);
    }
    const int side =
        qMin(RecordingRegionDragHandle::controlSize, qMin(bounds.width(), bounds.height()));
    const int inset =
        qMin(kMoveButtonGap, qMax(0, (qMin(bounds.width(), bounds.height()) - side) / 2));
    m_regionDragHandle->setFixedSize(side, side);
    m_regionDragHandle->move(bounds.right() + 1 - side - inset, bounds.bottom() + 1 - side - inset);
    m_regionDragHandle->show();
    m_regionDragHandle->raise();
    if (!m_regionInputRouter) {
        m_regionInputRouter = std::make_unique<RecordingRegionInputRouter>(
            [this](const QPoint& point) { updateNativeMouseRouting(point); });
    }
    updateNativeMouseRouting(regionPointer(QCursor::pos()));
}

QRegion ScreenRecordingAreaWindow::regionInteractionRegion() const {
    if (m_trimming)
        return QRegion(rect());
    if (!regionEditingEnabled())
        return {};
    const qreal horizontalHitWidth = qMin(kResizeHitWidth, m_selectionRect.width() / 4.0);
    const qreal verticalHitWidth = qMin(kResizeHitWidth, m_selectionRect.height() / 4.0);
    const QRect interior(QPoint(qFloor(m_selectionRect.left() + horizontalHitWidth) + 1,
                                qFloor(m_selectionRect.top() + verticalHitWidth) + 1),
                         QPoint(qCeil(m_selectionRect.right() - horizontalHitWidth) - 1,
                                qCeil(m_selectionRect.bottom() - verticalHitWidth) - 1));
    QRegion input = QRegion(rect()) - QRegion(interior);
    if (m_regionDragHandle && m_regionDragHandle->isVisible() && m_regionDragHandle->isEnabled())
        input |= m_regionDragHandle->geometry();
    return input;
}

void ScreenRecordingAreaWindow::updateNativeMouseRouting(const QPoint& desktopPosition) {
    if (!regionEditingEnabled() || !isVisible())
        return;
#ifdef Q_OS_WIN
    const QRect client = nativeClientGeometry(*this);
    const QPointF local = QPointF(desktopPosition - client.topLeft()) / devicePixelRatioF();
#else
    const QPointF local = mapFromGlobal(desktopPosition);
#endif
    // Keep annotations visible without letting their nonzero alpha intercept
    // interior clicks. Native mouse routing changes before the next input is delivered.
    applyNativePassThrough(!m_regionDragActive &&
                           !regionInteractionRegion().contains(local.toPoint()));
}

QColor ScreenRecordingAreaWindow::inputSurfaceColor() const {
    const bool interactive = m_inputMode == InputMode::Drawing && !m_drawingBlocked;
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
    if (!event || !watched || !watched->isWidgetType())
        return QWidget::eventFilter(watched, event);
    if (watched == m_canvas && regionEditingEnabled()) {
        if (event->type() == QEvent::Enter) {
            updateRegionCursor(mapFromGlobal(static_cast<QEnterEvent*>(event)->globalPosition()));
        } else if (event->type() == QEvent::MouseMove) {
            updateRegionCursor(mapFromGlobal(static_cast<QMouseEvent*>(event)->globalPosition()));
        } else if (event->type() == QEvent::MouseButtonPress ||
                   event->type() == QEvent::MouseButtonDblClick) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            const auto edges = resizeEdgesAt(mapFromGlobal(mouse->globalPosition()));
            if (mouse->button() == Qt::LeftButton && edges) {
                beginRegionDrag(regionPointer(mouse->globalPosition()), edges);
                return true;
            }
        }
    }
    if (watched != m_canvas || m_inputMode != InputMode::Drawing || m_drawingBlocked) {
        return QWidget::eventFilter(watched, event);
    }

    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (m_gestureButton == Qt::NoButton &&
            (mouse->button() == Qt::LeftButton ||
             (mouse->button() == Qt::RightButton &&
              m_canvas->hasQuickSelectionTargetAt(mouse->position(), Qt::RightButton)))) {
            m_gestureButton = mouse->button();
        }
        break;
    }
    case QEvent::MouseButtonRelease: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == m_gestureButton) {
            m_gestureButton = Qt::NoButton;
        }
        break;
    }
    case QEvent::Wheel: {
        if (m_canvas->hasActiveTextEditing() || m_canvas->canvasTool() == SnowCanvasTool::Text ||
            m_canvas->canvasTool() == SnowCanvasTool::SerialNumber)
            return false;
        auto* wheel = static_cast<QWheelEvent*>(event);
        if (snow_shot::presentation::angleWheelTarget(*m_canvas)) {
            if ((wheel->modifiers() & ~Qt::ShiftModifier) != Qt::NoModifier) {
                m_angleWheelSteps.reset();
                return false;
            }
            const int steps = m_angleWheelSteps.consume(*wheel);
            if (steps != 0)
                static_cast<void>(m_canvas->adjustAngleValue(
                    steps * (wheel->modifiers().testFlag(Qt::ShiftModifier) ? 0.1 : 1.0) *
                    std::numbers::pi / 180.0));
            wheel->accept();
            return true;
        }
        m_angleWheelSteps.reset();
        if (wheel->modifiers() != Qt::NoModifier)
            return false;
        const int delta =
            !wheel->pixelDelta().isNull() ? wheel->pixelDelta().y() : wheel->angleDelta().y();
        if (delta != 0) {
            emit drawingWheelRequested(delta > 0 ? 1 : -1);
        }
        wheel->accept();
        return true;
    }
    case QEvent::KeyPress: {
        m_angleWheelSteps.reset();
        auto* key = static_cast<QKeyEvent*>(event);
        if (snow_shot::shortcuts::commandKey(*key) != Qt::Key_Escape || key->isAutoRepeat()) {
            break;
        }
        if (m_canvas->hasActiveTextEditing()) {
            return false;
        }
        if (m_gestureButton != Qt::NoButton) {
            m_gestureButton = Qt::NoButton;
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
    layoutRegionDragHandle();
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
    const Qt::Edges edges = resizeEdgesAt(position);
    if (edges) {
        setRegionCursor(edges);
    } else {
        m_canvas->clearCursorForLayer(SnowCanvasCursorLayer::Host);
        unsetCursor();
        if (windowHandle())
            windowHandle()->unsetCursor();
    }
}

void ScreenRecordingAreaWindow::setRegionCursor(Qt::Edges edges) {
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
    return m_trimming ||
           (m_inputMode == InputMode::RegionEditing &&
            m_state == ScreenshotToolPalette::RecordingState::Idle && !m_drawingBlocked);
}

Qt::Edges ScreenRecordingAreaWindow::resizeEdgesAt(const QPointF& position) const {
    if (m_trimming || !regionEditingEnabled() || !rect().contains(position.toPoint())) {
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

int ScreenRecordingAreaWindow::minimumRegionExtent() const {
#ifdef Q_OS_MACOS
    return snow_shot::presentation::recording::screenRecordingMinimumExtent(devicePixelRatioF());
#else
    return snow_shot::presentation::recording::screenRecordingMinimumExtent();
#endif
}

void ScreenRecordingAreaWindow::beginRegionDrag(const QPoint& pointer, Qt::Edges edges) {
    if (!regionEditingEnabled() || m_regionDragActive)
        return;
    if (!m_trimming && !edges && (!m_regionDragHandle || !m_regionDragHandle->isDragging()))
        return;
    activateInput();
    m_regionDragOrigin = pointer;
    m_regionDragRect = m_recordingRegion;
    m_regionDragEdges = edges;
    m_regionEffectiveEdges = edges;
    m_regionDragActive = true;
    m_regionDragSource =
        (edges || (m_trimming && (!m_regionDragHandle || !m_regionDragHandle->isDragging())))
            ? static_cast<QWidget*>(this)
            : m_regionDragHandle;
    applyNativePassThrough(false);
    if (m_regionDragSource == this)
        grabMouse();
    setRegionCursor(edges);
    emit regionInteractionStarted();
}

void ScreenRecordingAreaWindow::updateRegionDrag(const QPoint& pointer) {
    if (!m_regionDragActive)
        return;
    const QPoint delta = pointer - m_regionDragOrigin;
    QRect next = m_regionDragRect.translated(delta);
    if (m_regionDragEdges) {
        namespace geometry = snow_shot::presentation::resize_geometry;
        const auto drag = geometry::dragGeometry(m_regionDragRect, m_regionDragEdges, delta,
                                                 m_regionEffectiveEdges);
        m_regionEffectiveEdges = drag.edges;
        int minimum = minimumRegionExtent();
#ifdef Q_OS_MACOS
        const QRect candidate =
            geometry::anchoredRect(m_regionDragRect, m_regionDragEdges, drag.edges,
                                   drag.requestedSize.expandedTo(QSize(1, 1)));
        if (QScreen* target = QGuiApplication::screenAt(candidate.center()))
            minimum = snow_shot::presentation::recording::screenRecordingMinimumExtent(
                target->devicePixelRatio());
#endif
        next = geometry::anchoredRect(m_regionDragRect, m_regionDragEdges, drag.edges,
                                      drag.requestedSize.expandedTo(QSize(minimum, minimum)));
    }
    applyRegionDragGeometry(next);
    setRegionCursor(m_regionEffectiveEdges);
}

void ScreenRecordingAreaWindow::applyRegionDragGeometry(const QRect& region) {
    const QRect previous = m_recordingRegion;
#ifdef Q_OS_MACOS
    // Moving a tiny region onto a lower-density display must retain the physical minimum.
    placeRecordingRegion(
        snow_shot::presentation::recording::screenRecordingNormalizedRegion(region));
#else
    placeRecordingRegion(region);
#endif
    if (previous != m_recordingRegion)
        emit recordingRegionChanged(m_recordingRegion);
}

void ScreenRecordingAreaWindow::cancelRegionInteraction() {
    if (!m_regionDragActive || m_cancellingRegionInteraction)
        return;
    const QScopedValueRollback<bool> cancelling(m_cancellingRegionInteraction, true);
    applyRegionDragGeometry(m_regionDragRect);
    finishRegionInteraction();
}

void ScreenRecordingAreaWindow::finishRegionInteraction() {
    if (!m_regionDragActive)
        return;
    synchronizeWindowGeometry();
    m_regionDragActive = false;
    m_regionDragSource = nullptr;
    if (m_regionDragHandle)
        m_regionDragHandle->stopDragging();
    if (QWidget::mouseGrabber() == this)
        releaseMouse();
    emit regionInteractionFinished();
    updateNativeMouseRouting(regionPointer(QCursor::pos()));
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
    layoutRegionDragHandle();
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
    if (region.width() >= minimumRegionExtent() && region.height() >= minimumRegionExtent() &&
        region != m_recordingRegion) {
        m_recordingRegion = region;
        emit recordingRegionChanged(region);
    }
}

bool ScreenRecordingAreaWindow::event(QEvent* event) {
    if (m_regionEscapeRelease && event->type() == QEvent::KeyRelease &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        m_regionEscapeRelease = false;
        return true;
    }
    if (m_regionDragActive &&
        (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        if (event->type() == QEvent::KeyPress) {
            m_regionEscapeRelease = true;
            cancelRegionInteraction();
        }
        event->accept();
        return true;
    }
    if (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate ||
        (event->type() == QEvent::UngrabMouse && m_regionDragSource == this))
        cancelRegionInteraction();
    if (event->type() == QEvent::Hide) {
        if (m_regionDragHandle)
            m_regionDragHandle->hide();
        m_regionInputRouter.reset();
    }
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
    layoutRegionDragHandle();
}

void ScreenRecordingAreaWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    synchronizeWindowGeometry();
    layoutRegionDragHandle();
}

void ScreenRecordingAreaWindow::closeEvent(QCloseEvent* event) {
    event->ignore();
    emit closeRequested();
}

void ScreenRecordingAreaWindow::enterEvent(QEnterEvent* event) {
    if (regionEditingEnabled())
        updateRegionCursor(event->position());
    QWidget::enterEvent(event);
}

void ScreenRecordingAreaWindow::mousePressEvent(QMouseEvent* event) {
    const auto edges = resizeEdgesAt(event->position());
    if (event->button() == Qt::LeftButton && (edges || m_trimming)) {
        beginRegionDrag(regionPointer(event->globalPosition()), edges);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void ScreenRecordingAreaWindow::mouseMoveEvent(QMouseEvent* event) {
    if (m_regionDragActive && m_regionDragSource == this) {
        if (event->buttons().testFlag(Qt::LeftButton))
            updateRegionDrag(regionPointer(event->globalPosition()));
        else
            cancelRegionInteraction();
        event->accept();
        return;
    }
    if (regionEditingEnabled())
        updateRegionCursor(event->position());
    QWidget::mouseMoveEvent(event);
}

void ScreenRecordingAreaWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (m_regionDragActive && m_regionDragSource == this && event->button() == Qt::LeftButton) {
        updateRegionDrag(regionPointer(event->globalPosition()));
        finishRegionInteraction();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

bool ScreenRecordingAreaWindow::nativeEvent(const QByteArray& eventType, void* message,
                                            qintptr* result) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const auto* msg = static_cast<MSG*>(message);
    if (msg != nullptr && result != nullptr) {
        if (m_regionDragActive &&
            (msg->message == WM_CANCELMODE || (msg->message == WM_CAPTURECHANGED &&
                                               reinterpret_cast<HWND>(msg->lParam) != msg->hwnd))) {
            cancelRegionInteraction();
        }
        if (m_regionDragActive && msg->message == WM_NCHITTEST) {
            *result = HTCLIENT;
            return true;
        }
        if (msg->message == WM_WINDOWPOSCHANGED) {
            scheduleGeometrySynchronization();
        }
        if (msg->message == WM_NCHITTEST) {
            POINT point{static_cast<short>(LOWORD(msg->lParam)),
                        static_cast<short>(HIWORD(msg->lParam))};
            ScreenToClient(msg->hwnd, &point);
            const QPointF local(point.x / devicePixelRatioF(), point.y / devicePixelRatioF());
            const bool interactive = m_inputMode == InputMode::Drawing && !m_drawingBlocked;
            *result = interactive || regionInteractionRegion().contains(local.toPoint())
                          ? HTCLIENT
                          : HTTRANSPARENT;
            return true;
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
    const bool focusable =
        (m_inputMode == InputMode::Drawing && !m_drawingBlocked) || regionEditingEnabled();
    next = focusable ? next & ~WS_EX_NOACTIVATE : next | WS_EX_NOACTIVATE;
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
    snow_shot::platform::configureScreenRecordingAreaWindow(this);
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
    } else if (regionEditingEnabled()) {
        painter.save();
        painter.setClipRegion(regionInteractionRegion());
        painter.fillRect(rect(), QColor(0, 0, 0, 2));
        painter.restore();
    }
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    if (m_trimming) {
        painter.fillRect(m_selectionRect, Qt::black);
        if (!m_previewFrame.isNull()) {
            const QSizeF fitted =
                QSizeF(m_previewFrame.size()).scaled(m_selectionRect.size(), Qt::KeepAspectRatio);
            const QRectF target(m_selectionRect.center() -
                                    QPointF(fitted.width() / 2, fitted.height() / 2),
                                fitted);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(target, m_previewFrame);
        }
    }
    painter.setRenderHint(QPainter::Antialiasing, false);
    const auto border = snow_shot::presentation::recording::screenRecordingAreaBorderGeometry(
        m_frameRect, m_selectionRect, m_paddingWidth);
    painter.fillRect(border.top, color);
    painter.fillRect(border.bottom, color);
    painter.fillRect(border.left, color);
    painter.fillRect(border.right, color);
    SNOW_SHOT_RECORDING_PERF_MILESTONE("area.first_paint_end");
}

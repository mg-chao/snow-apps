#include "pinnedwindowhost.h"
#include "pinnedimagepresenter.h"
#include "snow_draw_engine_qt/snow_canvas_view.h"

#include <QApplication>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QInputMethod>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPlatformSurfaceEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QTimer>
#include <QScopeGuard>
#include <QtMath>
#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace snow_shot::presentation {
class PinnedNativeWindow final : public QWindow {
  public:
    explicit PinnedNativeWindow(PinnedWindowHost& owner) : m_owner(owner) {
        setSurfaceType(QSurface::RasterSurface);
        QSurfaceFormat surfaceFormat;
        surfaceFormat.setAlphaBufferSize(8);
        setFormat(surfaceFormat);
    }
    QObject* focusObject() const override {
        return m_owner.m_view ? static_cast<QObject*>(m_owner.m_view.data()) : &m_owner;
    }

  protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::PlatformSurface &&
            static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() ==
                QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            m_owner.m_presenter->reset();
        if (m_owner.event(event))
            return true;
        return QWindow::event(event);
    }
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override {
#if defined(Q_OS_WIN)
        const auto* native = static_cast<const MSG*>(message);
        if (native->message == WM_WINDOWPOSCHANGED && !m_owner.nativePresentationInProgress()) {
            RECT client{};
            POINT origin{};
            if (GetClientRect(native->hwnd, &client) && ClientToScreen(native->hwnd, &origin))
                m_owner.m_presenter->observePosition(
                    reinterpret_cast<WId>(native->hwnd),
                    QRect(origin.x, origin.y, client.right, client.bottom));
        }
#endif
        return m_owner.nativeEvent(type, message, result);
    }

  private:
    PinnedWindowHost& m_owner;
};

class PinnedWidgetWindow final : public QWidget {
  public:
    PinnedWidgetWindow(PinnedWindowHost& owner, QWidget* parent)
        : QWidget(parent), m_owner(owner) {}

  protected:
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override {
        return m_owner.nativeEvent(type, message, result);
    }

  private:
    PinnedWindowHost& m_owner;
};

PinnedWindowHost::PinnedWindowHost(QWidget* parent) : QObject(parent) {
    m_widget = new PinnedWidgetWindow(*this, parent);
#if defined(Q_OS_WIN)
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        m_presenter =
            std::make_unique<PinnedImagePresenter>(createWindowsPinnedImagePresentationBackend());
        m_native = std::make_unique<PinnedNativeWindow>(*this);
    }
#endif
    m_widget->installEventFilter(this);
}
PinnedWindowHost::~PinnedWindowHost() {
    if (m_widget)
        m_widget->removeEventFilter(this);
    m_native.reset();
    delete m_widget.data();
}
bool PinnedWindowHost::usesNativeImagePresentation() const {
    return bool(m_native);
}
QWidget* PinnedWindowHost::widgetHost() const {
    return m_widget;
}
QWindow* PinnedWindowHost::windowHandle() const {
    return m_native ? m_native.get() : m_widget->windowHandle();
}
QScreen* PinnedWindowHost::screen() const {
    return m_native ? m_native->screen() : m_widget->screen();
}
void PinnedWindowHost::setScreen(QScreen* screen) {
    if (m_native)
        m_native->setScreen(screen);
    else
        m_widget->setScreen(screen);
}
WId PinnedWindowHost::winId() {
    if (!m_native)
        return m_widget->winId();
    const WId id = m_native->winId();
    m_widget->winId();
    m_widget->windowHandle()->setTransientParent(m_native.get());
    return id;
}
WId PinnedWindowHost::internalWinId() const {
    return m_native ? (m_native->handle() ? m_native->winId() : 0) : m_widget->internalWinId();
}
QRect PinnedWindowHost::geometry() const {
    return m_native ? m_native->geometry() : m_widget->geometry();
}
QRect PinnedWindowHost::frameGeometry() const {
    return m_native ? m_native->frameGeometry() : m_widget->frameGeometry();
}
QSize PinnedWindowHost::size() const {
    return geometry().size();
}
QRect PinnedWindowHost::rect() const {
    return QRect(QPoint(), size());
}
int PinnedWindowHost::width() const {
    return size().width();
}
int PinnedWindowHost::height() const {
    return size().height();
}
QPoint PinnedWindowHost::pos() const {
    return geometry().topLeft();
}
qreal PinnedWindowHost::devicePixelRatioF() const {
    return m_native ? m_native->devicePixelRatio() : m_widget->devicePixelRatioF();
}
void PinnedWindowHost::resize(const QSize& size) {
    if (m_native)
        m_native->resize(size);
    else
        m_widget->resize(size);
}
void PinnedWindowHost::resize(int width, int height) {
    resize(QSize(width, height));
}
void PinnedWindowHost::move(const QPoint& point) {
    if (m_native)
        m_native->setPosition(point);
    else
        m_widget->move(point);
}
void PinnedWindowHost::setGeometry(const QRect& geometry) {
    if (m_native)
        m_native->setGeometry(geometry);
    else
        m_widget->setGeometry(geometry);
}
void PinnedWindowHost::setWindowFlags(Qt::WindowFlags flags) {
    if (m_native)
        m_native->setFlags(flags);
    else
        m_widget->setWindowFlags(flags);
}
void PinnedWindowHost::overrideWindowFlags(Qt::WindowFlags flags) {
    if (m_native)
        m_native->setFlags(flags);
    else
        m_widget->overrideWindowFlags(flags);
}
Qt::WindowFlags PinnedWindowHost::windowFlags() const {
    return m_native ? m_native->flags() : m_widget->windowFlags();
}
void PinnedWindowHost::setAttribute(Qt::WidgetAttribute attribute, bool enabled) {
    if (attribute == Qt::WA_DeleteOnClose) {
        m_deleteOnClose = enabled;
        return;
    }
    if (m_native && attribute == Qt::WA_ShowWithoutActivating)
        m_native->setProperty("_q_showWithoutActivating", enabled);
    m_widget->setAttribute(attribute, enabled);
}
bool PinnedWindowHost::testAttribute(Qt::WidgetAttribute attribute) const {
    return attribute == Qt::WA_DeleteOnClose ? m_deleteOnClose : m_widget->testAttribute(attribute);
}
void PinnedWindowHost::setMinimumSize(int width, int height) {
    if (m_native)
        m_native->setMinimumSize(QSize(width, height));
    else
        m_widget->setMinimumSize(width, height);
}
void PinnedWindowHost::setFocusPolicy(Qt::FocusPolicy policy) {
    m_widget->setFocusPolicy(policy);
}
void PinnedWindowHost::setMouseTracking(bool enabled) {
    m_widget->setMouseTracking(enabled);
}
void PinnedWindowHost::setAcceptDrops(bool enabled) {
    m_widget->setAcceptDrops(enabled);
}
void PinnedWindowHost::setWindowTitle(const QString& title) {
    if (m_native)
        m_native->setTitle(title);
    else
        m_widget->setWindowTitle(title);
}
void PinnedWindowHost::setWindowOpacity(qreal opacity) {
    m_opacity = qBound(qreal(0), opacity, qreal(1));
    if (m_native) {
        if (m_presenter->hasFrame() && !nativePresentationInProgress())
            static_cast<void>(publishNativeFrame());
        else
            update();
    } else {
        m_widget->setWindowOpacity(m_opacity);
    }
}
qreal PinnedWindowHost::windowOpacity() const {
    return m_opacity;
}
void PinnedWindowHost::ensurePolished() {
    m_widget->ensurePolished();
}
QLayout* PinnedWindowHost::layout() const {
    return m_widget->layout();
}
QFont PinnedWindowHost::font() const {
    return m_widget->font();
}
QPalette PinnedWindowHost::palette() const {
    return m_widget->palette();
}
QPoint PinnedWindowHost::mapFromGlobal(const QPoint& p) const {
    return m_native ? m_native->mapFromGlobal(p) : m_widget->mapFromGlobal(p);
}
QPointF PinnedWindowHost::mapFromGlobal(const QPointF& p) const {
    return m_native ? m_native->mapFromGlobal(p) : m_widget->mapFromGlobal(p);
}
QPoint PinnedWindowHost::mapToGlobal(const QPoint& p) const {
    return m_native ? m_native->mapToGlobal(p) : m_widget->mapToGlobal(p);
}
QPointF PinnedWindowHost::mapToGlobal(const QPointF& p) const {
    return m_native ? m_native->mapToGlobal(p) : m_widget->mapToGlobal(p);
}
void PinnedWindowHost::setFocus(Qt::FocusReason reason) {
    if (m_native)
        m_native->requestActivate();
    else
        m_widget->setFocus(reason);
}
void PinnedWindowHost::clearFocus() {
    if (!m_native) {
        m_widget->clearFocus();
        return;
    }
#if defined(Q_OS_WIN)
    if (GetFocus() == reinterpret_cast<HWND>(internalWinId()))
        SetFocus(nullptr);
#endif
}
bool PinnedWindowHost::hasFocus() const {
    return m_native ? QGuiApplication::focusWindow() == m_native.get() : m_widget->hasFocus();
}
bool PinnedWindowHost::isActiveWindow() const {
    return isVisible() && (m_native ? m_native->isActive() : m_widget->isActiveWindow());
}
bool PinnedWindowHost::isVisible() const {
    return m_native ? m_native->isVisible() : m_widget->isVisible();
}
bool PinnedWindowHost::isHidden() const {
    return !isVisible();
}
void PinnedWindowHost::setVisible(bool visible) {
    if (visible)
        show();
    else
        hide();
}
void PinnedWindowHost::activateWindow() {
    if (m_native)
        m_native->requestActivate();
    else
        m_widget->activateWindow();
}
void PinnedWindowHost::raise() {
    if (m_native)
        m_native->raise();
    else
        m_widget->raise();
}
void PinnedWindowHost::setCursor(const QCursor& cursor) {
    if (m_native)
        m_native->setCursor(cursor);
    else
        m_widget->setCursor(cursor);
}
void PinnedWindowHost::unsetCursor() {
    if (m_native)
        m_native->unsetCursor();
    else
        m_widget->unsetCursor();
}
QCursor PinnedWindowHost::cursor() const {
    return m_native ? m_native->cursor() : m_widget->cursor();
}
void PinnedWindowHost::grabMouse() {
    if (m_native)
        m_native->setMouseGrabEnabled(true);
    else
        m_widget->grabMouse();
}
void PinnedWindowHost::releaseMouse() {
    if (m_native)
        m_native->setMouseGrabEnabled(false);
    else
        m_widget->releaseMouse();
}
void PinnedWindowHost::show() {
    if (!m_native) {
        m_widget->show();
        return;
    }
    m_native->show();
    const auto auxiliaries = std::exchange(m_hiddenAuxiliaries, {});
    for (const auto& widget : auxiliaries)
        if (widget)
            widget->show();
}
void PinnedWindowHost::hide() {
    if (!m_native) {
        m_widget->hide();
        return;
    }
    for (QWidget* child : m_widget->findChildren<QWidget*>()) {
        if (child->isWindow() && child->isVisible()) {
            m_hiddenAuxiliaries.append(child);
            child->hide();
        }
    }
    m_native->hide();
}
bool PinnedWindowHost::close() {
    if (!m_native)
        return m_widget->close();
    if (m_native->handle())
        return m_native->close();
    QCloseEvent closeEvent;
    QCoreApplication::sendEvent(this, &closeEvent);
    return closeEvent.isAccepted();
}
void PinnedWindowHost::closeEvent(QCloseEvent* event) {
    event->accept();
    hide();
    if (m_deleteOnClose)
        deleteLater();
}

void PinnedWindowHost::setCanvasView(SnowCanvasView* view) {
    m_view = view;
    if (m_native) {
        emit m_native->focusObjectChanged(view);
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
    }
}
void PinnedWindowHost::attachAuxiliary(QWidget* widget, bool sharesOpacity) {
    if (!m_native || !widget)
        return;
    widget->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    widget->winId();
    widget->windowHandle()->setTransientParent(m_native.get());
    if (sharesOpacity) {
        if (!m_frameAuxiliaries.contains(widget))
            m_frameAuxiliaries.append(widget);
        widget->setWindowOpacity(m_opacity);
    }
}
void PinnedWindowHost::update() {
    // Rounded QWindow dimensions can omit the final physical row or column.
    update(QRegion(m_native && m_view ? m_view->rect() : rect()));
}
void PinnedWindowHost::update(const QRegion& dirty) {
    if (!m_native) {
        m_widget->update(dirty);
        return;
    }
    m_dirty += dirty;
    if (m_frameScheduled)
        return;
    m_frameScheduled = true;
    QTimer::singleShot(0, this, [this] {
        m_frameScheduled = false;
        if (isVisible())
            static_cast<void>(publishNativeFrame());
    });
}
void PinnedWindowHost::repaint() {
    if (m_native)
        static_cast<void>(publishNativeFrame());
    else
        m_widget->repaint();
}
bool PinnedWindowHost::publishNativeFrame() {
#if defined(Q_OS_WIN)
    if (!m_native || !m_view || m_publishing || !internalWinId())
        return false;
    RECT client{};
    POINT origin{};
    const HWND hwnd = reinterpret_cast<HWND>(internalWinId());
    if (!GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin))
        return false;
    const QRect pixels(origin.x, origin.y, client.right, client.bottom);
    m_publishing = true;
    const auto publishingGuard = qScopeGuard([this] { m_publishing = false; });
    m_view->setSurfaceMetrics(pixels.size(), devicePixelRatioF());
    const QRegion dirty = m_dirty.isEmpty() ? QRegion(m_view->rect()) : std::exchange(m_dirty, {});
    const bool success =
        m_presenter->present(internalWinId(), pixels, devicePixelRatioF(),
                             static_cast<quint8>(qBound(0, qRound(m_opacity * 255), 255)), dirty,
                             [this](QPainter& painter, const QRegion& region) {
                                 return paintNativeFrame(painter, region);
                             });
    if (success) {
        m_committedOpacity = m_opacity;
        for (const auto& auxiliary : m_frameAuxiliaries)
            if (auxiliary)
                auxiliary->setWindowOpacity(m_opacity);
        nativeFramePublished();
    } else {
        m_opacity = m_committedOpacity;
        m_dirty += dirty;
        // USER32 retains the last published bitmap on failure. Restore its
        // rectangle as well if a system resize already changed the HWND.
        if (m_presenter->hasFrame() && !m_applyingGeometry) {
            const QRect committed = m_presenter->committedGeometry();
            SetWindowPos(hwnd, nullptr, committed.x(), committed.y(), committed.width(),
                         committed.height(), SWP_NOACTIVATE | SWP_NOZORDER);
            m_view->setSurfaceMetrics(committed.size(), devicePixelRatioF());
            nativeFrameRejected();
        }
    }
    return success;
#else
    return false;
#endif
}

bool PinnedWindowHost::applyNativeGeometry(const QRect& pixels) {
#if defined(Q_OS_WIN)
    if (!m_native || !internalWinId() || pixels.isEmpty() || m_applyingGeometry)
        return false;
    m_applyingGeometry = true;
    const auto guard = qScopeGuard([this] { m_applyingGeometry = false; });
    if (m_presenter->hasFrame() && pixels.size() == m_presenter->committedGeometry().size())
        return m_presenter->move(internalWinId(), pixels);
    const HWND hwnd = reinterpret_cast<HWND>(internalWinId());
    RECT previous{};
    if (!GetWindowRect(hwnd, &previous) ||
        !SetWindowPos(hwnd, nullptr, pixels.x(), pixels.y(), pixels.width(), pixels.height(),
                      SWP_NOACTIVATE | SWP_NOZORDER))
        return false;
    // Initial placement precedes canvas initialization. Its caller publishes
    // the complete first frame before showing the window.
    if (!m_presenter->hasFrame())
        return true;
    m_dirty = QRegion(QRect(QPoint(), QSize(qCeil(pixels.width() / devicePixelRatioF()),
                                            qCeil(pixels.height() / devicePixelRatioF()))));
    if (publishNativeFrame())
        return true;
    SetWindowPos(hwnd, nullptr, previous.left, previous.top, previous.right - previous.left,
                 previous.bottom - previous.top, SWP_NOACTIVATE | SWP_NOZORDER);
    m_view->setSurfaceMetrics(QSize(previous.right - previous.left, previous.bottom - previous.top),
                              devicePixelRatioF());
    nativeFrameRejected();
    return false;
#else
    Q_UNUSED(pixels);
    return false;
#endif
}

bool PinnedWindowHost::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_widget &&
        (!m_native || event->type() == QEvent::LanguageChange ||
         event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange))
        return QCoreApplication::sendEvent(this, event);
    return QObject::eventFilter(watched, event);
}
bool PinnedWindowHost::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::Paint:
        paintEvent(static_cast<QPaintEvent*>(event));
        return true;
    case QEvent::Close:
        closeEvent(static_cast<QCloseEvent*>(event));
        return false;
    case QEvent::Resize:
        if (m_native)
            m_widget->setGeometry(geometry());
        resizeEvent(static_cast<QResizeEvent*>(event));
        update();
        return false;
    case QEvent::Move:
        if (m_native)
            m_widget->setGeometry(geometry());
        moveEvent(static_cast<QMoveEvent*>(event));
        return false;
    case QEvent::Show:
        showEvent(static_cast<QShowEvent*>(event));
        return false;
    case QEvent::ContextMenu:
        contextMenuEvent(static_cast<QContextMenuEvent*>(event));
        return event->isAccepted();
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease:
    case QEvent::Wheel:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::InputMethod:
    case QEvent::InputMethodQuery:
    case QEvent::Enter:
    case QEvent::Leave:
    case QEvent::FocusOut:
        if (m_native && m_view && QCoreApplication::sendEvent(m_view, event))
            return true;
        if (event->type() == QEvent::MouseButtonPress)
            mousePressEvent(static_cast<QMouseEvent*>(event));
        else if (event->type() == QEvent::MouseButtonDblClick)
            mouseDoubleClickEvent(static_cast<QMouseEvent*>(event));
        else if (event->type() == QEvent::MouseButtonRelease)
            mouseReleaseEvent(static_cast<QMouseEvent*>(event));
        else if (event->type() == QEvent::Wheel)
            wheelEvent(static_cast<QWheelEvent*>(event));
        return false;
    case QEvent::DragEnter:
        dragEnterEvent(static_cast<QDragEnterEvent*>(event));
        return true;
    case QEvent::DragMove:
        dragMoveEvent(static_cast<QDragMoveEvent*>(event));
        return true;
    case QEvent::DragLeave:
        dragLeaveEvent(static_cast<QDragLeaveEvent*>(event));
        return true;
    case QEvent::Drop:
        dropEvent(static_cast<QDropEvent*>(event));
        return true;
    case QEvent::LanguageChange:
    case QEvent::PaletteChange:
    case QEvent::FontChange:
        changeEvent(event);
        return false;
    case QEvent::Expose:
        if (!m_presenter || !m_presenter->hasFrame() || !m_dirty.isEmpty())
            update();
        return false;
    default:
        return QObject::event(event);
    }
}
} // namespace snow_shot::presentation

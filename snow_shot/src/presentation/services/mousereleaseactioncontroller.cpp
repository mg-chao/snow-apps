#include "snow_shot/presentation/mousereleaseactioncontroller.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QWindow>

#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace snow_shot::presentation {

namespace {
QWindow* releaseSurface(QObject* object) {
    if (auto* widget = qobject_cast<QWidget*>(object))
        return widget->windowHandle();
    return qobject_cast<QWindow*>(object);
}
WId releaseWindowId(QObject* object) {
    if (auto* widget = qobject_cast<QWidget*>(object))
        return widget->winId();
    if (auto* surface = releaseSurface(object))
        return surface->winId();
    return 0;
}
bool releaseScopeVisible(QObject* object) {
    if (auto* widget = qobject_cast<QWidget*>(object))
        return widget->isVisible();
    if (auto* surface = releaseSurface(object))
        return surface->isVisible();
    return false;
}
void releaseSetCapture(QObject* object, bool capture) {
    if (auto* widget = qobject_cast<QWidget*>(object)) {
        if (capture)
            widget->grabMouse();
        else
            widget->releaseMouse();
    } else if (auto* surface = releaseSurface(object)) {
        surface->setMouseGrabEnabled(capture);
    }
}
} // namespace

MouseReleaseActionController::MouseReleaseActionController(QObject* parent) : QObject(parent) {
    qApp->installEventFilter(this);
}

MouseReleaseActionController::~MouseReleaseActionController() {
    cancel();
    qApp->removeEventFilter(this);
}

bool MouseReleaseActionController::arm(QObject* scopeWindow, Qt::MouseButton button,
                                       std::function<void()> action) {
    if (pending()) {
        return false;
    }
    if (!scopeWindow || !releaseScopeVisible(scopeWindow) || button == Qt::NoButton || !action) {
        return false;
    }
    auto* widgetScope = qobject_cast<QWidget*>(scopeWindow);
    QObject* scope = widgetScope ? static_cast<QObject*>(widgetScope->window()) : scopeWindow;
    QWidget* grabber = QWidget::mouseGrabber();
    if (grabber && grabber->window() != scope) {
        return false;
    }
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        const HWND capture = GetCapture();
        const HWND root = reinterpret_cast<HWND>(releaseWindowId(scope));
        if (capture && capture != root && !IsChild(root, capture)) {
            return false;
        }
    }
#endif
    m_scope = scope;
    m_capture = grabber ? grabber : scope;
    m_ownsCapture = grabber == nullptr;
    if (m_ownsCapture) {
        releaseSetCapture(m_capture, true);
    }
    bool captured = widgetScope ? QWidget::mouseGrabber() == m_capture : true;
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        captured = captured && GetCapture() == reinterpret_cast<HWND>(releaseWindowId(m_capture));
    }
#endif
    if (!captured) {
        cancel();
        return false;
    }
    ++m_revision;
    m_button = button;
    m_action = std::move(action);
    m_scopeDestroyed = connect(scope, &QObject::destroyed, this, [this] { cancel(); });
    return true;
}

bool MouseReleaseActionController::pending() const {
    return m_button != Qt::NoButton || m_finishing;
}

void MouseReleaseActionController::releaseCapture() {
    const auto capture = std::exchange(m_capture, {});
    if (std::exchange(m_ownsCapture, false) && capture &&
        (!qobject_cast<QWidget*>(capture.data()) || QWidget::mouseGrabber() == capture)) {
        releaseSetCapture(capture, false);
    }
}

void MouseReleaseActionController::cancel() {
    disconnect(m_scopeDestroyed);
    m_scopeDestroyed = {};
    ++m_revision;
    m_button = Qt::NoButton;
    m_finishing = false;
    m_action = {};
    m_scope.clear();
    releaseCapture();
}

void MouseReleaseActionController::finish() {
    if (m_button == Qt::NoButton) {
        return;
    }
    m_button = Qt::NoButton;
    m_finishing = true;
    auto action = std::exchange(m_action, {});
    const auto revision = m_revision;
    // Clear capture ownership before ReleaseCapture emits synchronous messages.
    releaseCapture();
    QMetaObject::invokeMethod(
        this,
        [this, revision, action = std::move(action)] {
            if (revision != m_revision || !m_finishing || !m_scope ||
                !releaseScopeVisible(m_scope)) {
                return;
            }
            cancel();
            action(); // May destroy both the scope and this controller.
        },
        Qt::QueuedConnection);
}

bool MouseReleaseActionController::eventFilter(QObject* watched, QEvent* event) {
    // Let QWidgetWindow finish routing and release its implicit mouse capture
    // before consuming the QWidget delivery of the terminating event.
    if (!pending() || (qobject_cast<QWindow*>(watched) && watched != m_scope)) {
        return false;
    }
    if (event->type() == QEvent::ApplicationDeactivate ||
        (watched == m_scope && (event->type() == QEvent::Hide || event->type() == QEvent::Destroy ||
                                event->type() == QEvent::WindowDeactivate)) ||
        (watched == m_capture && event->type() == QEvent::UngrabMouse)) {
        cancel();
        return false;
    }
    if (event->type() == QEvent::ContextMenu) {
        auto* context = static_cast<QContextMenuEvent*>(event);
        auto* widget = qobject_cast<QWidget*>(watched);
        if (context->reason() == QContextMenuEvent::Mouse && widget &&
            widget->window() == m_scope) {
            event->accept();
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonDblClick) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == m_button) {
            event->accept();
            if (event->type() == QEvent::MouseButtonRelease) {
                finish();
            }
            return true;
        }
    }
    return false;
}

bool MouseReleaseActionController::handleNativeEvent(void* message, qintptr* result) {
#ifdef Q_OS_WIN
    if (!pending() || !m_scope) {
        return false;
    }
    auto* native = static_cast<MSG*>(message);
    if (!native) {
        return false;
    }
    if (native->message == WM_CAPTURECHANGED && m_capture &&
        native->hwnd == reinterpret_cast<HWND>(releaseWindowId(m_capture)) &&
        reinterpret_cast<HWND>(native->lParam) !=
            reinterpret_cast<HWND>(releaseWindowId(m_capture))) {
        cancel();
        return false;
    }
    if (native->hwnd != reinterpret_cast<HWND>(releaseWindowId(m_scope))) {
        return false;
    }
    const Qt::MouseButton released = native->message == WM_NCLBUTTONUP   ? Qt::LeftButton
                                     : native->message == WM_NCRBUTTONUP ? Qt::RightButton
                                     : native->message == WM_NCMBUTTONUP ? Qt::MiddleButton
                                                                         : Qt::NoButton;
    if (released != Qt::NoButton && released == m_button) {
        finish();
        if (result) {
            *result = 0;
        }
        return true;
    }
#else
    Q_UNUSED(message)
    Q_UNUSED(result)
#endif
    return false;
}

} // namespace snow_shot::presentation

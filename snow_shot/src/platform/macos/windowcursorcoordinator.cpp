#include "windowcursorcoordinator_p.h"

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <utility>

namespace snow_shot::platform::macos::detail {
WindowCursorCoordinator::WindowCursorCoordinator(Backend backend, QObject* parent)
    : QObject(parent), m_backend(std::move(backend)) {
    qApp->installEventFilter(this);
    connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock, this,
            &WindowCursorCoordinator::observeOverrideCursor);
}

void WindowCursorCoordinator::addWindow(QWidget* window) {
    if (!window || m_roots.contains(window))
        return;
    m_roots.append(window);
    observeWindow(window->windowHandle());
    connect(window, &QObject::destroyed, this, [this] {
        m_roots.removeAll(nullptr);
        invalidate();
    });
    invalidate();
}

void WindowCursorCoordinator::observeWindow(QWindow* window) {
    if (!window || m_observed.contains(window))
        return;
    m_observed.insert(window);
    connect(window, &QWindow::transientParentChanged, this, &WindowCursorCoordinator::invalidate);
    connect(window, &QObject::destroyed, this, [this, window] {
        m_observed.remove(window);
        m_blocked.remove(window);
        m_destroying.remove(window);
        invalidate();
    });
}

bool WindowCursorCoordinator::manages(QWidget* widget) const {
    QWidget* window = widget->window();
    if (m_roots.contains(window))
        return true;
    QSet<QWindow*> visited;
    QWindow* surface = window->windowHandle();
    for (QWindow* owner = surface; owner && !visited.contains(owner);
         owner = owner->transientParent()) {
        visited.insert(owner);
        for (const auto& root : m_roots) {
            if (root && root->windowHandle() == owner)
                return true;
        }
    }
    // A pooled tool's live transient owner takes precedence over a QObject
    // parent retained only for storage/lifetime ownership.
    if (surface && surface->transientParent())
        return false;
    for (QWidget* owner = window->parentWidget(); owner; owner = owner->parentWidget()) {
        if (m_roots.contains(owner))
            return true;
    }
    return false;
}

void WindowCursorCoordinator::invalidate() {
    if (m_queued || m_roots.isEmpty())
        return;
    m_queued = true;
    // Event filters run before the receiver. Apply only after Qt has resolved
    // child-widget/tool cursors and committed input transparency for this event.
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_queued = false;
            refresh();
        },
        Qt::QueuedConnection);
}

void WindowCursorCoordinator::clearInteraction() {
    m_pressedWindow.clear();
    m_pressedSurface.clear();
    m_buttons = Qt::NoButton;
}

void WindowCursorCoordinator::observeOverrideCursor() {
    const QCursor* cursor = QGuiApplication::overrideCursor();
    const std::optional<QCursor> current = cursor ? std::optional(*cursor) : std::nullopt;
    if (current != m_override) {
        m_override = current;
        invalidate();
    }
}

void WindowCursorCoordinator::refresh() {
    if (!m_backend.active()) {
        clearInteraction();
        return;
    }
    if (m_backend.suspended())
        return;

    const bool physicalPress = m_backend.buttonsPressed && m_backend.buttonsPressed();
    if (m_backend.buttonsPressed && !physicalPress)
        clearInteraction();
    QWidget* target = m_backend.mouseGrabber();
    // A native gesture that never entered Qt has no managed implicit owner.
    if (!target && physicalPress && m_buttons == Qt::NoButton)
        return;
    const bool grabbed = target || m_buttons != Qt::NoButton;
    if (!target) {
        if (grabbed)
            target = m_pressedSurface ? m_pressedWindow.data() : nullptr;
        else
            target = m_backend.hoverTarget();
    }
    if (!target || !manages(target))
        return;
    QWidget* window = target->window();
    QWindow* surface = window->windowHandle();
    if (!window->isVisible() || !window->isEnabled() ||
        window->testAttribute(Qt::WA_TransparentForMouseEvents) || !surface || !surface->handle() ||
        surface->flags().testFlag(Qt::WindowTransparentForInput) || m_blocked.contains(surface) ||
        m_destroying.contains(surface) || !m_backend.inputEnabled(target))
        return;
    // A Qt popup's grab must not let the underlying canvas replace its cursor.
    if (QWidget* popup = QApplication::activePopupWidget(); popup && popup->window() != window)
        return;
    m_backend.apply(target, grabbed);
}

bool WindowCursorCoordinator::eventFilter(QObject* watched, QEvent* event) {
    auto* widget = qobject_cast<QWidget*>(watched);
    auto* surface = qobject_cast<QWindow*>(watched);
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove:
        if (widget) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (event->type() == QEvent::MouseButtonPress ||
                event->type() == QEvent::MouseButtonDblClick) {
                if (m_buttons == Qt::NoButton) {
                    m_pressedWindow = widget->window();
                    m_pressedSurface = m_pressedWindow->windowHandle();
                }
                m_buttons = mouse->buttons() | mouse->button();
            } else {
                m_buttons = mouse->buttons();
                if (m_buttons == Qt::NoButton)
                    clearInteraction();
            }
        }
        invalidate();
        break;
    case QEvent::ApplicationDeactivate:
        clearInteraction();
        invalidate();
        break;
    case QEvent::WindowBlocked:
    case QEvent::WindowUnblocked:
        if (surface) {
            observeWindow(surface);
            if (event->type() == QEvent::WindowBlocked)
                m_blocked.insert(surface);
            else
                m_blocked.remove(surface);
        }
        invalidate();
        break;
    case QEvent::PlatformSurface:
        if (surface) {
            observeWindow(surface);
            if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() ==
                QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
                m_destroying.insert(surface);
                if (m_pressedSurface == surface) {
                    m_pressedWindow.clear();
                    m_pressedSurface.clear();
                }
            } else
                m_destroying.remove(surface);
        }
        invalidate();
        break;
    case QEvent::Show:
    case QEvent::WinIdChange:
        observeWindow(surface ? surface : widget ? widget->windowHandle() : nullptr);
        invalidate();
        break;
    case QEvent::Hide:
        if ((widget && widget == m_pressedWindow) || (surface && surface == m_pressedSurface)) {
            // A replacement/pooled surface cannot inherit an unfinished gesture.
            // Retain held buttons so hover cannot take over before its release.
            m_pressedWindow.clear();
            m_pressedSurface.clear();
        }
        invalidate();
        break;
    case QEvent::Enter:
    case QEvent::Leave:
    case QEvent::CursorChange:
    case QEvent::GrabMouse:
    case QEvent::UngrabMouse:
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::ZOrderChange:
    case QEvent::ParentChange:
    case QEvent::EnabledChange:
    case QEvent::WindowActivate:
    case QEvent::WindowDeactivate:
    case QEvent::ApplicationActivate:
    case QEvent::ApplicationStateChange:
    case QEvent::DevicePixelRatioChange:
    case QEvent::ScreenChangeInternal:
    case QEvent::Expose:
        invalidate();
        break;
    default:
        break;
    }
    return false;
}
} // namespace snow_shot::platform::macos::detail

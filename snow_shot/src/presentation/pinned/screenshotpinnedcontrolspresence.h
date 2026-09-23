#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDCONTROLSPRESENCE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDCONTROLSPRESENCE_H

#include <QEvent>
#include <QTimer>

#include <functional>
#include <optional>
#include <utility>

// Event-driven presence for the entire pin, including its child controls.
// Events invalidate the observation; only the live cursor query decides presence.
class ScreenshotPinnedControlsPresence final : public QObject {
  public:
    ScreenshotPinnedControlsPresence(QObject* parent, std::function<std::optional<bool>()> resolve,
                                     std::function<void()> changed)
        : QObject(parent), m_resolve(std::move(resolve)), m_changed(std::move(changed)) {
        m_hideTimer.setSingleShot(true);
        m_hideTimer.setTimerType(Qt::PreciseTimer);
        m_hideTimer.setInterval(100);
        connect(&m_hideTimer, &QTimer::timeout, this, [this] {
            if (!m_active || !std::exchange(m_hidePending, false))
                return;
            // Unknown is not evidence of an exit (e.g. a native surface is
            // being recreated). A later valid observation starts a new delay.
            const auto inside = m_resolve();
            if (inside.has_value())
                setInside(*inside);
        });
    }

    void setActive(bool active) {
        m_active = active;
        if (active) {
            refresh();
        } else {
            cancelHide();
            setInside(false);
        }
    }

    void refresh() {
        if (!m_active)
            return;
        const auto inside = m_resolve();
        if (!inside.has_value()) {
            cancelHide();
        } else if (*inside) {
            cancelHide();
            setInside(true);
        } else if (m_inside && !m_hidePending) {
            m_hidePending = true;
            m_hideTimer.start();
        }
    }

    [[nodiscard]] bool inside() const {
        return m_inside;
    }

    [[nodiscard]] static bool isPointerEvent(QEvent::Type type) {
        switch (type) {
        case QEvent::Enter:
        case QEvent::Leave:
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease:
        case QEvent::NonClientAreaMouseMove:
        case QEvent::NonClientAreaMouseButtonRelease:
        case QEvent::UngrabMouse:
        case QEvent::DragEnter:
        case QEvent::DragMove:
        case QEvent::DragLeave:
        case QEvent::Drop:
            return true;
        default:
            return false;
        }
    }

  private:
    friend class ScreenshotPinnedWindowTestAccess;

    void cancelHide() {
        m_hidePending = false;
        m_hideTimer.stop();
    }

    void setInside(bool inside) {
        if (std::exchange(m_inside, inside) != inside)
            m_changed();
    }

    QTimer m_hideTimer;
    std::function<std::optional<bool>()> m_resolve;
    std::function<void()> m_changed;
    bool m_active = false;
    bool m_inside = false;
    bool m_hidePending = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDCONTROLSPRESENCE_H

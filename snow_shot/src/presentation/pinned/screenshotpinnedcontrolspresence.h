#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDCONTROLSPRESENCE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDCONTROLSPRESENCE_H

#include <QSize>
#include <QTimer>

#include <functional>
#include <utility>

// Owns the action-panel visibility policy. The window reports pointer entry and
// exit from Qt and native non-client events; layout never determines hover.
class ScreenshotPinnedControlsPresence final : public QObject {
  public:
    struct Presentation {
        bool windowVisible = false;
        bool thumbnail = false;
        bool editing = false;
        bool clickThrough = false;
        QSize nativeSize;

        [[nodiscard]] bool allowsControls() const {
            constexpr int minimumNativeDimension = 383;
            return windowVisible && !thumbnail && !editing && !clickThrough &&
                   nativeSize.width() >= minimumNativeDimension &&
                   nativeSize.height() >= minimumNativeDimension;
        }
    };

    ScreenshotPinnedControlsPresence(QObject* parent, std::function<void(bool)> visibilityChanged)
        : QObject(parent), m_visibilityChanged(std::move(visibilityChanged)) {
        m_hideTimer.setSingleShot(true);
        m_hideTimer.setTimerType(Qt::PreciseTimer);
        m_hideTimer.setInterval(100);
        connect(&m_hideTimer, &QTimer::timeout, this, [this] {
            if (m_active && m_exitPending) {
                m_exitPending = false;
                m_inside = false;
                publishVisibility();
            }
        });
    }

    void setActive(bool active) {
        m_active = active;
        if (!active) {
            m_hideTimer.stop();
            m_exitPending = false;
            m_inside = false;
        }
        publishVisibility();
    }

    void setPresentation(const Presentation& presentation) {
        m_presentation = presentation;
        publishVisibility();
    }

    void enter() {
        if (!m_active)
            return;
        m_hideTimer.stop();
        m_exitPending = false;
        m_inside = true;
        publishVisibility();
    }

    void leave() {
        if (!m_active || !m_inside || m_exitPending)
            return;
        m_exitPending = true;
        m_hideTimer.start();
    }

    [[nodiscard]] bool inside() const {
        return m_active && m_inside;
    }

  private:
    friend class ScreenshotPinnedWindowTestAccess;

    void publishVisibility() {
        const bool visible = inside() && m_presentation.allowsControls();
        if (std::exchange(m_visible, visible) != visible)
            m_visibilityChanged(visible);
    }

    QTimer m_hideTimer;
    std::function<void(bool)> m_visibilityChanged;
    Presentation m_presentation;
    bool m_active = false;
    bool m_inside = false;
    bool m_exitPending = false;
    bool m_visible = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDCONTROLSPRESENCE_H

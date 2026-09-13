#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDPOINTERPRESENCE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDPOINTERPRESENCE_H

#include <QTimer>

#include <functional>
#include <optional>
#include <utility>

// Reveal immediately; delay hiding for both the pinned controls and the top handle.
class ScreenshotPinnedPointerPresence final : public QObject {
  public:
    ScreenshotPinnedPointerPresence(QObject* parent, std::function<std::optional<bool>()> resolve,
                                    std::function<void(bool)> changed)
        : QObject(parent), m_resolve(std::move(resolve)), m_changed(std::move(changed)) {
        m_timer.setSingleShot(true);
        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.setInterval(100);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            if (const auto current = m_resolve(); current && *current) {
                update(*current);
                return;
            }
            m_inside = false;
            m_changed(m_inside);
        });
    }

    void update(bool inside) {
        if (inside) {
            m_timer.stop();
            if (!m_inside) {
                m_inside = true;
                m_changed(true);
            }
        } else if (m_inside && !m_timer.isActive()) {
            m_timer.start();
        }
    }

    void reset() {
        m_timer.stop();
        m_inside = false;
    }

    QTimer& timer() {
        return m_timer;
    }

  private:
    QTimer m_timer;
    std::function<std::optional<bool>()> m_resolve;
    std::function<void(bool)> m_changed;
    bool m_inside = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDPOINTERPRESENCE_H

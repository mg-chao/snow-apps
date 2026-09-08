#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGAUTOSCROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGAUTOSCROLLER_H

#include "snow_shot/presentation/screenshotscrollingtypes.h"

#include <QObject>
#include <QRect>
#include <QTimer>

#include <functional>
#include <utility>

namespace snow_shot::capture_detail {

class ScreenshotScrollingAutoScroller final : public QObject {
  public:
    using ScrollStep = std::function<void(const QRect&, const QPoint&)>;

    explicit ScreenshotScrollingAutoScroller(ScrollStep scrollStep)
        : m_scrollStep(std::move(scrollStep)) {
        m_timer.setParent(this);
        m_timer.setInterval(200);
        m_timer.setTimerType(Qt::PreciseTimer);
        connect(&m_timer, &QTimer::timeout, this, [this]() {
            if (m_timer.isActive()) {
                m_scrollStep(m_selection, m_mode == ScreenshotScrollingRecognitionMode::Horizontal
                                              ? QPoint(120, 0)
                                              : QPoint(0, -120));
            }
        });
    }

    void start(const QRect& physicalSelection, ScreenshotScrollingRecognitionMode mode) {
        stop();
        m_selection = physicalSelection;
        m_mode = mode;
    }

    void setMode(ScreenshotScrollingRecognitionMode mode) {
        m_mode = mode;
    }

    void setEnabled(bool enabled) {
        m_enabled = enabled && !m_selection.isEmpty();
        updateTimer();
    }

    void setPaused(bool paused) {
        m_paused = paused;
        updateTimer();
    }

    void stop() {
        m_timer.stop();
        m_selection = {};
        m_enabled = false;
        m_paused = false;
    }

  private:
    void updateTimer() {
        if (m_enabled && !m_paused) {
            if (!m_timer.isActive()) {
                m_timer.start();
            }
        } else {
            m_timer.stop();
        }
    }

    QTimer m_timer;
    ScrollStep m_scrollStep;
    QRect m_selection;
    ScreenshotScrollingRecognitionMode m_mode = ScreenshotScrollingRecognitionMode::Vertical;
    bool m_enabled = false;
    bool m_paused = false;
};

} // namespace snow_shot::capture_detail

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGAUTOSCROLLER_H

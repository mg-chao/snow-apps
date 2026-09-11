#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGAUTOSCROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGAUTOSCROLLER_H

#include "snow_shot/presentation/screenshotscrollingtypes.h"
#include "snow_shot/platform/windows/scrollinput.h"

#include <QCoreApplication>
#include <QObject>
#include <QRect>
#include <QTimer>

#include <functional>
#include <utility>

namespace snow_shot::capture_detail {

inline QString autoScrollFailureMessage(platform::windows::ScrollInputResult result) {
    using Status = platform::windows::ScrollInputResult::Status;
    if (result.status == Status::PermissionDenied) {
        return QCoreApplication::translate(
            "ScreenshotScrollingCaptureController",
            "Auto-scroll requires Accessibility access. Allow Snow Shot in System Settings > "
            "Privacy & Security > Accessibility, then turn Auto-scroll on again.");
    }
    if (result.status == Status::TargetNotFound) {
        return QCoreApplication::translate(
            "ScreenshotScrollingCaptureController",
            "No application window was found under the selection. Select the target window and "
            "try Auto-scroll again.");
    }
    return QCoreApplication::translate(
               "ScreenshotScrollingCaptureController",
               "Auto-scroll failed (error %1). You can continue scrolling manually.")
        .arg(result.error);
}

class ScreenshotScrollingAutoScroller final : public QObject {
  public:
    using ScrollStep =
        std::function<platform::windows::ScrollInputResult(const QRect&, const QPoint&)>;
    using FailureHandler = std::function<void(platform::windows::ScrollInputResult)>;

    explicit ScreenshotScrollingAutoScroller(ScrollStep scrollStep, FailureHandler failure = {})
        : m_scrollStep(std::move(scrollStep)), m_failure(std::move(failure)) {
        m_timer.setParent(this);
        m_timer.setInterval(200);
        m_timer.setTimerType(Qt::PreciseTimer);
        connect(&m_timer, &QTimer::timeout, this, [this]() {
            if (m_timer.isActive()) {
                const auto result = m_scrollStep(
                    m_selection, m_mode == ScreenshotScrollingRecognitionMode::Horizontal
                                     ? QPoint(120, 0)
                                     : QPoint(0, -120));
                if (result.status != platform::windows::ScrollInputResult::Status::Posted) {
                    setEnabled(false);
                    if (m_failure)
                        m_failure(result);
                }
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
    FailureHandler m_failure;
    QRect m_selection;
    ScreenshotScrollingRecognitionMode m_mode = ScreenshotScrollingRecognitionMode::Vertical;
    bool m_enabled = false;
    bool m_paused = false;
};

} // namespace snow_shot::capture_detail

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGAUTOSCROLLER_H

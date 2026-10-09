#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSMARTSELECTIONTRANSITION_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSMARTSELECTIONTRANSITION_H

#include <QEasingCurve>
#include <QRectF>
#include <QtGlobal>

#include <functional>

class ScreenshotSmartSelectionTransition final {
  public:
    static constexpr int kDurationMs = 101;
    static constexpr QEasingCurve::Type kEasingCurve = QEasingCurve::OutQuad;

    using UpdateCallback = std::function<void(const QRectF&)>;

    explicit ScreenshotSmartSelectionTransition(UpdateCallback update);
    ScreenshotSmartSelectionTransition(const ScreenshotSmartSelectionTransition&) = delete;
    ScreenshotSmartSelectionTransition&
    operator=(const ScreenshotSmartSelectionTransition&) = delete;

    void setEnabled(bool enabled);
    [[nodiscard]] bool enabled() const;
    // Both entry points use the presentation scheduler's monotonic clock in milliseconds.
    [[nodiscard]] bool update(const QRectF& selection, bool smartFraming, qint64 nowMs);
    // Returns whether the displayed geometry changed; no autonomous animation timer runs.
    [[nodiscard]] bool advance(qint64 nowMs);

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] QRectF displayedSelection() const;

  private:
    [[nodiscard]] bool presentDirectly(const QRectF& selection);
    void notifyUpdate();

    UpdateCallback m_update;
    QRectF m_displayedSelection;
    QRectF m_startSelection;
    QRectF m_targetSelection;
    qint64 m_startedAtMs = 0;
    qint64 m_lastAdvancedAtMs = 0;
    bool m_running = false;
    bool m_hasPresentedSmartSelection = false;
    bool m_enabled = true;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSMARTSELECTIONTRANSITION_H

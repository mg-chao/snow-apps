#ifndef SNOW_SHOT_PRESENTATION_RECORDING_RECORDINGCOUNTDOWNOVERLAY_H
#define SNOW_SHOT_PRESENTATION_RECORDING_RECORDINGCOUNTDOWNOVERLAY_H

#include <QElapsedTimer>
#include <QWidget>

class QTimer;

namespace snow_shot::presentation::recording {

inline constexpr int screenRecordingCountdownIndicatorSize = 88;
inline constexpr int screenRecordingCountdownIndicatorRadius = 20;

// Whole seconds left in a countdown of totalMs after elapsedMs. Never drops
// below 1: the owner removes the overlay the moment the countdown completes,
// so the final second keeps painting until then.
[[nodiscard]] int screenRecordingCountdownRemainingSeconds(qint64 totalMs, qint64 elapsedMs);

// Indicator visibility inside the current second: fully opaque at the second
// boundary, transparent at its midpoint, and opaque again at the next boundary.
[[nodiscard]] qreal screenRecordingCountdownOpacity(qint64 elapsedWithinSecondMs);

// Semi-transparent rounded square shown in the middle of the recording area
// while a start delay counts down. Purely visual: the controller owns the
// countdown timing and clears the overlay when recording begins.
class RecordingCountdownOverlay final : public QWidget {
    Q_OBJECT

  public:
    explicit RecordingCountdownOverlay(QWidget* parent = nullptr);
    ~RecordingCountdownOverlay() override;

    void start(int seconds);
    void clear();
    [[nodiscard]] bool active() const {
        return m_totalMilliseconds > 0;
    }
    [[nodiscard]] int remainingSeconds() const;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QElapsedTimer m_elapsed;
    QTimer* m_animationTimer = nullptr;
    qint64 m_totalMilliseconds = 0;
};

} // namespace snow_shot::presentation::recording

#endif // SNOW_SHOT_PRESENTATION_RECORDING_RECORDINGCOUNTDOWNOVERLAY_H

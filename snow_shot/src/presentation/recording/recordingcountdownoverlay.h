#ifndef SNOW_SHOT_PRESENTATION_RECORDING_RECORDINGCOUNTDOWNOVERLAY_H
#define SNOW_SHOT_PRESENTATION_RECORDING_RECORDINGCOUNTDOWNOVERLAY_H

#include <QWidget>

namespace snow_shot::presentation::recording {

inline constexpr int screenRecordingCountdownIndicatorSize = 88;
inline constexpr qint64 screenRecordingCountdownDigitPopDurationMs = 180;

// Whole seconds left for remainingMs of countdown time. Never drops below 1:
// the owner removes the overlay the moment the countdown completes, so the
// final second keeps painting until then.
[[nodiscard]] int screenRecordingCountdownRemainingSeconds(qint64 remainingMs);

// Remaining fraction of the whole countdown: 1 at the start, 0 at the end.
[[nodiscard]] qreal screenRecordingCountdownProgress(qint64 totalMs, qint64 remainingMs);

// Entrance progress of the currently displayed digit: 0 at its second
// boundary, 1 once its pop-in animation has finished.
[[nodiscard]] qreal screenRecordingCountdownDigitEntrance(qint64 totalMs, qint64 remainingMs);

// Semi-transparent circular indicator centered on the recording area while a
// start delay counts down. Purely visual: the controller owns the countdown
// clock and pushes the remaining time, so the digits it paints share the clock
// that starts the recording.
class RecordingCountdownOverlay final : public QWidget {
    Q_OBJECT

  public:
    explicit RecordingCountdownOverlay(QWidget* parent = nullptr);
    ~RecordingCountdownOverlay() override;

    void start(int seconds);
    void setRemainingMilliseconds(qint64 remainingMs);
    void clear();
    [[nodiscard]] bool active() const {
        return m_totalMilliseconds > 0;
    }
    [[nodiscard]] int remainingSeconds() const;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    qint64 m_totalMilliseconds = 0;
    qint64 m_remainingMilliseconds = 0;
};

} // namespace snow_shot::presentation::recording

#endif // SNOW_SHOT_PRESENTATION_RECORDING_RECORDINGCOUNTDOWNOVERLAY_H

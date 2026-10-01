#ifndef SNOW_SHOT_PRESENTATION_RECORDING_RECORDINGAUDIOGAINPOPOVER_H
#define SNOW_SHOT_PRESENTATION_RECORDING_RECORDINGAUDIOGAINPOPOVER_H

#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>

class QWidget;

namespace adqt::widgets {
class AdButton;
class AdPopover;
class AdSlider;
} // namespace adqt::widgets

// The controller supplies processed peaks and owns capture/preview lifetime.
class RecordingAudioGainPopover final : public QObject {
    Q_OBJECT

  public:
    enum class Source { SystemAudio, Microphone };
    Q_ENUM(Source)
    enum class LevelStatus { Live, AudioOff, Unavailable, Starting, PermissionRequired };
    Q_ENUM(LevelStatus)
    using SurfaceShowGuard = std::function<bool(QWidget*)>;

    explicit RecordingAudioGainPopover(adqt::widgets::AdButton* trigger, Source source,
                                       QObject* parent = nullptr);
    ~RecordingAudioGainPopover() override;

    adqt::widgets::AdButton* trigger() const;
    adqt::widgets::AdPopover* popover() const;
    int gainDb() const;
    void setGainDb(int gainDb);
    void setLevel(double peak, bool clipping, LevelStatus status = LevelStatus::Live);
    void setAvailable(bool available);
    void setAudioEnabled(bool enabled);
    void openAndFocus();
    void close();
    QWidget* prepareSurface();
    void setRetainNativeSurfaceOnHide(bool retain);
    void setSurfaceShowGuard(SurfaceShowGuard guard);

  signals:
    void gainChanged(int gainDb);
    void visibleChanged(bool visible);
    void surfaceVisibilityChanged(bool visible);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    QWidget* createContent();
    void refreshText();
    void refreshLevel();
    void refreshMeterStyle();
    void focusSliderWhenVisible();

    QPointer<adqt::widgets::AdButton> m_trigger;
    adqt::widgets::AdPopover* m_popover = nullptr;
    QPointer<QWidget> m_content;
    QPointer<adqt::widgets::AdSlider> m_slider;
    Source m_source;
    LevelStatus m_status = LevelStatus::Live;
    int m_gainDb = 0;
    double m_peak = 0.0;
    bool m_clipping = false;
    bool m_available = true;
    bool m_audioEnabled = true;
    bool m_focusWhenVisible = false;
    bool m_closing = false;
};

#endif

#ifndef SNOW_SHOT_RECORDINGEFFECTPREVIEW_H
#define SNOW_SHOT_RECORDINGEFFECTPREVIEW_H

#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"
#include "recordingeffectssource.h"
#include <QColor>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

class CanvasStatusReadout;
class RecordingStatusPrompt;
class ScreenRecordingAreaWindow;

class RecordingEffectPreview final : public QObject, public SnowCanvasCustomRenderer {
  public:
    RecordingEffectPreview(ScreenRecordingAreaWindow& area,
                           std::unique_ptr<RecordingEffectsSource> source = {});
    ~RecordingEffectPreview() override;
    void configure(const QRect& capture, const QSize& output, const QColor& trail,
                   const QColor& click, bool keyboard, int trailDurationMs = 500,
                   const QColor& keyboardBackground = QColor(0, 0, 0, 204),
                   const QColor& keyboardForeground = QColor(Qt::white), int keyboardSize = 64,
                   const QColor& highlight = QColor(0, 0, 0, 0), bool recordMouseClicks = false,
                   int highlightRadius = 41, uint32_t playbackOverlay = 0,
                   const QColor& progressBarColor = QColor(22, 119, 255),
                   SnowWebcam* webcam = nullptr, int webcamSize = 150, uint32_t webcamShape = 0,
                   const QPoint& webcamOffset = QPoint(48, 48), bool recordMouseScrolling = false,
                   uint32_t webcamOrientation = 0, bool recordNumberKeys = true,
                   bool recordLettersAndSymbols = true, bool recordControlKeys = true,
                   bool preserveKeyboardShortcuts = true);
    void setEligible(bool eligible);
    void setStatusPrompts(bool enabled, const QString& format, bool microphone, bool speaker);
    void stopAndClear(bool present = false);
    void prepareForSessionEnd();
    [[nodiscard]] bool active() const;
    [[nodiscard]] quint64 generation() const;
    [[nodiscard]] bool hasFrame() const;
    std::function<void(const QString&)> reportError;
    void renderBeforeCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override;
    void renderAfterCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void synchronize();
    void receiveFrame();
    void clearFrame();
    void clearPlaybackFrame();
    void updateReadout();
    void updateWebcamControl();
    [[nodiscard]] QTransform outputToCanvas(const QSize& output) const;
    [[nodiscard]] QRegion frameRegion(const RecordingEffectsFrame& frame) const;
    ScreenRecordingAreaWindow& m_area;
    std::unique_ptr<RecordingEffectsSource> m_source;
    CanvasStatusReadout* m_readout;
    RecordingStatusPrompt* m_statusPrompt;
    bool m_showStatusPrompts = true;
    bool m_previewVisible = false;
    QTimer m_configurationTimer;
    QRect m_capture;
    QSize m_output;
    QColor m_trail;
    QColor m_click;
    QColor m_highlight{0, 0, 0, 0};
    int m_highlightRadius = 41;
    bool m_recordMouseClicks = false;
    bool m_recordMouseScrolling = false;
    int m_trailDurationMs = 500;
    QColor m_keyboardBackground{0, 0, 0, 204};
    QColor m_keyboardForeground{Qt::white};
    int m_keyboardSize = 64;
    uint32_t m_playbackOverlay = 0;
    QColor m_progressBarColor{22, 119, 255};
    std::shared_ptr<RecordingEffectsFrame> m_playbackFrame;
    bool m_playbackDirty = true;
    bool m_keyboard = false;
    bool m_recordNumberKeys = true;
    bool m_recordLettersAndSymbols = true;
    bool m_recordControlKeys = true;
    bool m_preserveKeyboardShortcuts = true;
    SnowWebcam* m_webcam = nullptr;
    int m_webcamSize = 150;
    QPoint m_webcamOffset{48, 48};
    uint32_t m_webcamShape = 0;
    uint32_t m_webcamOrientation = 0;
    bool m_eligible = false;
    bool m_running = false;
    bool m_sessionEnding = false;
    bool m_failed = false;
    bool m_configurationDirty = true;
    bool m_webcamLayoutDirty = false;
    bool m_windowBlocked = false;
    quint64 m_generation = 0;
    std::atomic<bool> m_notificationPending = false;
    std::shared_ptr<RecordingEffectsFrame> m_frame;
};

#endif // SNOW_SHOT_RECORDINGEFFECTPREVIEW_H

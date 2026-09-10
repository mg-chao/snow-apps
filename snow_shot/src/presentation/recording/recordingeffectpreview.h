#ifndef SNOW_SHOT_RECORDINGEFFECTPREVIEW_H
#define SNOW_SHOT_RECORDINGEFFECTPREVIEW_H

#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"
#include "snow_recording_effects.h"
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
class ScreenRecordingAreaWindow;

struct RecordingEffectsFrame {
    quint64 generation = 0;
    quint64 revision = 0;
    QSize output;
    struct Tile {
        QRect rect;
        QImage image;
        QSize coordinateSize;
    };
    // Images borrow the native frame. Destroy images before releasing the owner.
    std::shared_ptr<void> lease;
    std::vector<Tile> tiles;
    QString error;
};

// Injectable observation boundary. A stopped source must join before returning.
class RecordingEffectsSource {
  public:
    virtual ~RecordingEffectsSource() = default;
    virtual bool start(const SnowRecordingEffectsConfig& config, std::function<void()> notify,
                       QString& error) = 0;
    virtual bool configure(const SnowRecordingEffectsConfig& config, QString& error) = 0;
    virtual void stop() = 0;
    virtual std::shared_ptr<RecordingEffectsFrame> acquire() = 0;
};

class RecordingEffectPreview final : public QObject, public SnowCanvasCustomRenderer {
  public:
    RecordingEffectPreview(ScreenRecordingAreaWindow& area,
                           std::unique_ptr<RecordingEffectsSource> source = {});
    ~RecordingEffectPreview() override;
    void configure(const QRect& capture, const QSize& output, const QColor& trail,
                   const QColor& click, bool keyboard, int trailDurationMs = 500,
                   const QColor& keyboardBackground = QColor(0, 0, 0, 204),
                   const QColor& keyboardForeground = QColor(Qt::white), int keyboardSize = 64);
    void setEligible(bool eligible);
    void stopAndClear(bool present = false);
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
    void updateReadout();
    [[nodiscard]] QTransform outputToCanvas(const QSize& output) const;
    [[nodiscard]] QRegion frameRegion(const RecordingEffectsFrame& frame) const;
    ScreenRecordingAreaWindow& m_area;
    std::unique_ptr<RecordingEffectsSource> m_source;
    CanvasStatusReadout* m_readout;
    QTimer m_configurationTimer;
    QRect m_capture;
    QSize m_output;
    QColor m_trail;
    QColor m_click;
    int m_trailDurationMs = 500;
    QColor m_keyboardBackground{0, 0, 0, 204};
    QColor m_keyboardForeground{Qt::white};
    int m_keyboardSize = 64;
    bool m_keyboard = false;
    bool m_eligible = false;
    bool m_running = false;
    bool m_failed = false;
    bool m_configurationDirty = true;
    bool m_windowBlocked = false;
    quint64 m_generation = 0;
    std::atomic<bool> m_notificationPending = false;
    std::shared_ptr<RecordingEffectsFrame> m_frame;
};

#endif // SNOW_SHOT_RECORDINGEFFECTPREVIEW_H

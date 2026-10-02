#ifndef SNOW_SHOT_RECORDINGTRIMTOOLBAR_H
#define SNOW_SHOT_RECORDINGTRIMTOOLBAR_H

#include "snow_shot/presentation/screenshottoolbarmainpanel.h"
#include <functional>

class QSlider;

class RecordingTrimToolbar final : public ScreenshotToolbarPanel,
                                   public adqt::widgets::AdControlScaleParticipant {
  public:
    explicit RecordingTrimToolbar(QWidget* parent = nullptr);
    void setTimeline(int frames, quint64 duration, std::function<quint64(int)> boundary);
    void setPosition(quint64 position);
    [[nodiscard]] int firstFrame() const;
    [[nodiscard]] int endFrame() const;
    QSize sizeHint() const override;
    void prepareControlScale(const adqt::widgets::AdControlScaleContext&) override {}
    void commitControlScale(const adqt::widgets::AdControlScaleContext& context) override;
    std::function<void()> replayRequested;
    std::function<void(int)> seekRequested;
    std::function<void(int, int, int)> rangeChanged;

  protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void changeEvent(QEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;

  private:
    void retranslate();
    void layoutHandles();
    void moveHandle(QSlider* handle, const QPointF& position);
    [[nodiscard]] qreal xForFrame(int frame) const;
    [[nodiscard]] int frameAt(qreal x) const;
    [[nodiscard]] QRectF trackRect() const;
    adqt::widgets::AdButton* m_replay = nullptr;
    QSlider* m_first = nullptr;
    QSlider* m_end = nullptr;
    QSlider* m_dragging = nullptr;
    QSlider* m_hovered = nullptr;
    std::function<quint64(int)> m_boundary;
    int m_frames = 1;
    quint64 m_duration = 1;
    quint64 m_position = 0;
    qreal m_scale = 1.0;
    qreal m_dragOffset = 0.0;
};

#endif

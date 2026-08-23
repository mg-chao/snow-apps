#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASCOLORSAMPLERWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASCOLORSAMPLERWINDOW_H

#include <QColor>
#include <QCursor>
#include <QImage>
#include <QPoint>
#include <QWidget>

class QPaintEvent;

class ScreenshotCanvasColorSamplerWindow final : public QWidget {
  public:
    explicit ScreenshotCanvasColorSamplerWindow(QWidget* parent = nullptr);

    void beginSampling();
    void updateSample(const QImage& previewImage, const QPoint& globalCursorPosition);
    void endSampling();

    [[nodiscard]] static QCursor samplingCursor();
    [[nodiscard]] QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    void updatePosition(const QPoint& globalCursorPosition);

    QImage m_previewImage;
    QColor m_currentColor;
    QColor m_panelBackground;
    QColor m_previewBorder;
    QColor m_textColor;
    QColor m_secondaryTextColor;
    bool m_sampling = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASCOLORSAMPLERWINDOW_H

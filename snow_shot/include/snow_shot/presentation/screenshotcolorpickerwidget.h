#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCOLORPICKERWIDGET_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCOLORPICKERWIDGET_H

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QWidget>

class QGraphicsOpacityEffect;
class QPaintEvent;

class ScreenshotColorPickerWidget final : public QWidget {
  public:
    explicit ScreenshotColorPickerWidget(QWidget* parent = nullptr);

    void resetForNewCapture();
    void setCaptureImage(const QImage& image, const QRect& physicalRect);
    void updatePicker(const QPoint& physicalPoint, const QPointF& overlayLocalPosition,
                      qreal opacity);
    void hidePicker();
    void setCenterGuideLineColor(const QColor& color);
    [[nodiscard]] QColor centerGuideLineColor() const;
    void cycleColorFormat();
    QString currentColorText() const;
    QColor currentColor() const;
    bool hasCurrentColor() const;

    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    enum class ColorFormat {
        Hex,
        Rgb,
        Hsl,
    };

    [[nodiscard]] bool updatePreview(const QPoint& physicalPoint);
    [[nodiscard]] bool updatePosition(const QPointF& overlayLocalPosition);
    QString formatColor(const QColor& color) const;
    QRectF panelRect() const;
    QRectF previewRect() const;
    QRectF positionTextRect() const;
    QRectF colorTextRect() const;

    QImage m_captureImage;
    QRect m_physicalRect;
    QImage m_previewImage;
    QPoint m_currentPhysicalPoint;
    QColor m_currentColor;
    QColor m_panelBackground;
    QColor m_panelTextColor;
    QColor m_centerGuideLineColor = QColor(0, 0, 0, 0);
    QGraphicsOpacityEffect* m_opacityEffect = nullptr;
    ColorFormat m_colorFormat = ColorFormat::Hex;
    bool m_hasCurrentColor = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCOLORPICKERWIDGET_H

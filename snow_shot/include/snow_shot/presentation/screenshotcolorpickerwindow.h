#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCOLORPICKERWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCOLORPICKERWINDOW_H

#include "snow_shot/presentation/screenshotselectiondisplayunit.h"

#include <optional>
#include <QColor>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QWidget>

class QGraphicsOpacityEffect;
class QPaintEvent;

class ScreenshotColorPickerWindow final : public QWidget {
  public:
    explicit ScreenshotColorPickerWindow(QWidget* parent = nullptr);

    void setOwnerWindow(QWidget* owner);
    void prepareNativeSurface();
    void resetForNewCapture();
    void setCaptureImage(const QImage& image, const QRect& physicalRect);
    void
    updatePicker(const QPoint& physicalPoint, const QPointF& overlayLocalPosition, qreal opacity,
                 std::optional<ScreenshotCoordinateDisplayValues> displayValues = std::nullopt);
    void hidePicker();
    void setCenterGuideLineColor(const QColor& color);
    void cycleColorFormat();
    QString currentColorText() const;
    QString currentPositionText() const;
    bool hasCurrentColor() const;

    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    enum class ColorFormat {
        Hex,
        HexWithoutHash,
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
    ScreenshotCoordinateDisplayValues m_displayValues;
    QColor m_currentColor;
    QColor m_panelBackground;
    QColor m_panelTextColor;
    QColor m_centerGuideLineColor = QColor(0, 0, 0, 0);
    QGraphicsOpacityEffect* m_opacityEffect = nullptr;
    ColorFormat m_colorFormat = ColorFormat::Hex;
    bool m_hasCurrentColor = false;
    qreal m_preparedSurfaceDevicePixelRatio = 0.0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCOLORPICKERWINDOW_H

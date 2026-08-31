#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTESTYLEMODEL_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTESTYLEMODEL_H

#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <QColor>
#include <QVector>

#include <optional>

class ScreenshotToolPaletteRectangleStyleModel final {
  public:
    explicit ScreenshotToolPaletteRectangleStyleModel(double minimumStrokeWidth = 1.0);

    void reset();
    [[nodiscard]] SnowCanvasShapeStyle rectangleStyle() const;
    void setRectangleStyle(const SnowCanvasShapeStyle& style);

    [[nodiscard]] double strokeWidth() const;
    [[nodiscard]] const QColor& strokeColor() const;
    [[nodiscard]] SnowCanvasStrokeStyle strokeStyle() const;
    [[nodiscard]] const QColor& fillColor() const;
    [[nodiscard]] SnowCanvasFillStyle fillStyle() const;
    [[nodiscard]] int cornerRadius() const;
    [[nodiscard]] double opacity() const;
    [[nodiscard]] SnowCanvasHighlightShape highlightShape() const;
    [[nodiscard]] SnowCanvasRectangleShape shape() const;
    [[nodiscard]] const QVector<double>& strokeWidthValues() const;
    [[nodiscard]] const QVector<QColor>& strokeColorValues() const;
    [[nodiscard]] const QVector<QColor>& fillColorValues() const;

    [[nodiscard]] bool stepStrokeWidth(int direction);
    [[nodiscard]] bool setStrokeWidth(double strokeWidth);
    [[nodiscard]] bool cycleStrokeWidth();
    [[nodiscard]] bool setStrokeColor(const QColor& color);
    [[nodiscard]] bool cycleStrokeColor();
    [[nodiscard]] bool setStrokeStyle(SnowCanvasStrokeStyle strokeStyle);
    [[nodiscard]] bool setFillColor(const QColor& color);
    [[nodiscard]] bool setFillStyle(SnowCanvasFillStyle fillStyle);
    [[nodiscard]] bool stepCornerRadius(int direction);
    [[nodiscard]] bool setCornerRadius(int cornerRadius);
    [[nodiscard]] bool setOpacity(double opacity);
    [[nodiscard]] bool setHighlightShape(SnowCanvasHighlightShape shape);
    [[nodiscard]] bool setShape(SnowCanvasRectangleShape shape);

  private:
    [[nodiscard]] double clampedStrokeWidth(double strokeWidth) const;
    [[nodiscard]] static double clampedCornerRadius(double cornerRadius);

    QVector<double> m_strokeWidthValues;
    QVector<QColor> m_strokeColorValues;
    QVector<QColor> m_fillColorValues;
    double m_minimumStrokeWidth = 1.0;
    double m_strokeWidth = 2.0;
    QColor m_strokeColor;
    SnowCanvasStrokeStyle m_strokeStyle = SnowCanvasStrokeStyle::Solid;
    QColor m_fillColor;
    SnowCanvasFillStyle m_fillStyle = SnowCanvasFillStyle::Solid;
    SnowCanvasCornerRadii m_cornerRadii;
    double m_opacity = 1.0;
    SnowCanvasHighlightShape m_highlightShape = SnowCanvasHighlightShape::Rectangle;
    SnowCanvasRectangleShape m_shape = SnowCanvasRectangleShape::Rectangle;
};

class ScreenshotToolPaletteTextStyleModel final {
  public:
    ScreenshotToolPaletteTextStyleModel();

    void reset();
    [[nodiscard]] const SnowCanvasTextStyle& textStyle() const;
    void setTextStyle(const SnowCanvasTextStyle& style);

    [[nodiscard]] const QVector<double>& fontSizeValues() const;
    [[nodiscard]] const QVector<double>& strokeWidthValues() const;
    [[nodiscard]] const QVector<QColor>& colorValues() const;
    [[nodiscard]] const QVector<QColor>& fillColorValues() const;

    [[nodiscard]] bool setColor(const QColor& color);
    [[nodiscard]] bool setFontSize(double fontSize);
    [[nodiscard]] bool stepFontSize(int direction);
    [[nodiscard]] bool cycleFontSize();
    [[nodiscard]] bool setFontFamily(const QString& fontFamily);
    [[nodiscard]] bool setStrokeColor(const QColor& color);
    [[nodiscard]] bool setStrokeWidth(double strokeWidth);
    [[nodiscard]] bool stepStrokeWidth(int direction);
    [[nodiscard]] bool setFillColor(const QColor& color);
    [[nodiscard]] bool setFillStyle(SnowCanvasFillStyle fillStyle);
    [[nodiscard]] bool setCornerRadius(int cornerRadius);
    [[nodiscard]] bool stepCornerRadius(int direction);
    [[nodiscard]] bool setHorizontalAlign(SnowCanvasTextHorizontalAlign alignment);

  private:
    [[nodiscard]] static double clampedFontSize(double fontSize);
    [[nodiscard]] static double clampedStrokeWidth(double strokeWidth);
    [[nodiscard]] static double clampedCornerRadius(double cornerRadius);

    QVector<double> m_fontSizeValues;
    QVector<double> m_strokeWidthValues;
    QVector<QColor> m_colorValues;
    QVector<QColor> m_fillColorValues;
    SnowCanvasTextStyle m_style;
};

struct ScreenshotToolPaletteStyleState {
    ScreenshotToolPaletteStyleState() = default;
    explicit ScreenshotToolPaletteStyleState(const SnowCanvasStyleDefaults& defaults);

    void reset(const SnowCanvasStyleDefaults& defaults);

    ScreenshotToolPaletteRectangleStyleModel m_creationRectangleStyle;
    ScreenshotToolPaletteRectangleStyleModel m_rectangleStyle;
    ScreenshotToolPaletteRectangleStyleModel m_creationLineStyle;
    ScreenshotToolPaletteRectangleStyleModel m_lineStyle;
    ScreenshotToolPaletteRectangleStyleModel m_creationFreeDrawStyle;
    ScreenshotToolPaletteRectangleStyleModel m_freeDrawStyle;
    ScreenshotToolPaletteRectangleStyleModel m_creationHighlightStyle{0.0};
    ScreenshotToolPaletteRectangleStyleModel m_highlightStyle{0.0};
    SnowCanvasShapeStyle m_creationPenHighlightStyle;
    SnowCanvasShapeStyle m_penHighlightStyle;
    SnowCanvasArrowStyle m_creationArrowStyle;
    SnowCanvasArrowStyle m_arrowStyle;
    ScreenshotToolPaletteTextStyleModel m_creationTextStyle;
    ScreenshotToolPaletteTextStyleModel m_textStyle;
    SnowCanvasSerialNumberStyle m_creationSerialNumberStyle;
    SnowCanvasSerialNumberStyle m_serialNumberStyle;
    SnowCanvasFilterStyle creationRectangleFilterStyle;
    SnowCanvasFilterStyle rectangleFilterStyle;
    SnowCanvasFilterStyle creationPenFilterStyle;
    SnowCanvasFilterStyle penFilterStyle;
    SnowCanvasWatermarkConfig m_watermarkConfig;
    SnowCanvasSpotlightConfig spotlightConfig;
    bool m_arrowControlsActive = false;
    bool m_lineControlsActive = false;
    bool m_freeDrawControlsActive = false;
    bool m_highlightControlsActive = false;
    bool m_penHighlightControlsActive = false;
    bool m_textControlsActive = false;
    bool m_showingSelectedStyle = false;
    bool m_showingSelectedTextStyle = false;
    quint32 m_selectedStyleMixed = 0;
    quint32 m_textStyleMixed = 0;
    quint32 m_serialNumberStyleMixed = 0;
    std::optional<SnowCanvasStyleToolbarSource> m_styleSource;
    quint32 filterStyleMixed = 0;
    std::optional<SnowCanvasStyleToolbarSource> filterStyleSource;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTESTYLEMODEL_H

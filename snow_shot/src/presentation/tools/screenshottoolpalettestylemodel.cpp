#include "screenshottoolpalettestylemodel.h"

#include "snow_shot/presentation/screenshotdefaultstyles.h"

#include <algorithm>
#include <cmath>

ScreenshotToolPaletteStyleState::ScreenshotToolPaletteStyleState(
    const SnowCanvasStyleDefaults& defaults) {
    reset(defaults);
}

void ScreenshotToolPaletteStyleState::reset(const SnowCanvasStyleDefaults& defaults) {
    m_creationRectangleStyle.setRectangleStyle(defaults.rectangle);
    m_rectangleStyle = m_creationRectangleStyle;
    m_creationLineStyle.setRectangleStyle(defaults.line);
    m_lineStyle = m_creationLineStyle;
    m_creationFreeDrawStyle.setRectangleStyle(defaults.freeDraw);
    m_freeDrawStyle = m_creationFreeDrawStyle;
    m_creationHighlightStyle.setRectangleStyle(defaults.rectangleHighlight);
    m_highlightStyle = m_creationHighlightStyle;
    m_creationPenHighlightStyle = defaults.penHighlight;
    m_penHighlightStyle = m_creationPenHighlightStyle;
    m_creationArrowStyle = SnowCanvasArrowStyle{
        defaults.arrow.stroke,       defaults.arrow.strokeWidth, defaults.arrow.startArrowhead,
        defaults.arrow.endArrowhead, defaults.arrow.strokeStyle, defaults.arrow.arrowType,
    };
    m_arrowStyle = m_creationArrowStyle;
    m_creationTextStyle.setTextStyle(defaults.text);
    m_textStyle.setTextStyle(defaults.text);
    m_creationSerialNumberStyle = defaults.serialNumber;
    m_serialNumberStyle = defaults.serialNumber;
    creationRectangleFilterStyle = defaults.rectangleFilter;
    rectangleFilterStyle = creationRectangleFilterStyle;
    creationPenFilterStyle = defaults.penFilter;
    penFilterStyle = creationPenFilterStyle;
    m_watermarkConfig = defaults.watermark;
    spotlightConfig = defaults.spotlight;
    m_showingSelectedStyle = false;
    m_showingSelectedTextStyle = false;
    m_selectedStyleMixed = 0;
    m_textStyleMixed = 0;
    m_serialNumberStyleMixed = 0;
    m_styleSource.reset();
    filterStyleMixed = 0;
    filterStyleSource.reset();
}

namespace {
constexpr double kMinRectangleStrokeWidth = 1.0;
constexpr double kMaxRectangleStrokeWidth = 72.0;
constexpr int kMinRectangleCornerRadius = 0;
constexpr int kMaxRectangleCornerRadius = 83;
constexpr double kMinTextFontSize = 6.0;
constexpr double kMaxTextFontSize = 256.0;
constexpr double kMinTextStrokeWidth = 0.0;
constexpr double kMaxTextStrokeWidth = 72.0;
constexpr int kMinTextCornerRadius = 0;
constexpr int kMaxTextCornerRadius = 83;

bool fuzzyEqual(double left, double right) {
    return qFuzzyCompare(left + 1.0, right + 1.0);
}

bool hasUniformCornerRadius(const SnowCanvasCornerRadii& cornerRadii, double cornerRadius) {
    return fuzzyEqual(cornerRadii.topLeft, cornerRadius) &&
           fuzzyEqual(cornerRadii.topRight, cornerRadius) &&
           fuzzyEqual(cornerRadii.bottomRight, cornerRadius) &&
           fuzzyEqual(cornerRadii.bottomLeft, cornerRadius);
}

bool containsStrokeWidth(const QVector<double>& values, double strokeWidth) {
    return std::any_of(values.cbegin(), values.cend(), [strokeWidth](double value) {
        return qFuzzyCompare(value + 1.0, strokeWidth + 1.0);
    });
}

QVector<double> strokeWidthStepValues(const QVector<double>& presetValues,
                                      double minimumStrokeWidth) {
    QVector<double> values;
    for (double presetValue : presetValues) {
        const double clampedValue =
            std::clamp(presetValue, minimumStrokeWidth, kMaxRectangleStrokeWidth);
        if (!containsStrokeWidth(values, clampedValue)) {
            values.push_back(clampedValue);
        }
    }
    std::sort(values.begin(), values.end());
    return values;
}

template <typename T> int indexOfValue(const QVector<T>& values, const T& value) {
    for (int index = 0; index < values.size(); ++index) {
        if (values.at(index) == value) {
            return index;
        }
    }
    return 0;
}

SnowCanvasTextStyle defaultTextStyle() {
    return snow_shot::presentation::screenshotCanvasStyleDefaults().text;
}
} // namespace

ScreenshotToolPaletteRectangleStyleModel::ScreenshotToolPaletteRectangleStyleModel(
    double minimumStrokeWidth
)
    : m_strokeWidthValues{
          snow_shot::presentation::screenshotCanvasStyleDefaults().rectangle.strokeWidth,
          4.0,
          8.0,
      }
    , m_strokeColorValues{
          snow_shot::presentation::screenshotCanvasStyleDefaults().rectangle.stroke,
          QColor(QStringLiteral("#52c41a")),
          QColor(QStringLiteral("#1677ff")),
          QColor(QStringLiteral("#fadb14")),
          QColor(QStringLiteral("#000000")),
      }
    , m_fillColorValues{
          snow_shot::presentation::screenshotCanvasStyleDefaults().rectangle.fill,
          QColor(QStringLiteral("#ffccc7")),
          QColor(QStringLiteral("#d9f7be")),
          QColor(QStringLiteral("#bae0ff")),
          QColor(QStringLiteral("#fff1b8")),
      }
    , m_minimumStrokeWidth(std::clamp(
          minimumStrokeWidth,
          0.0,
          kMaxRectangleStrokeWidth
      ))
{
    reset();
}

void ScreenshotToolPaletteRectangleStyleModel::reset() {
    setRectangleStyle(snow_shot::presentation::screenshotCanvasStyleDefaults().rectangle);
}

SnowCanvasShapeStyle ScreenshotToolPaletteRectangleStyleModel::rectangleStyle() const {
    SnowCanvasShapeStyle style = snow_shot::presentation::screenshotCanvasStyleDefaults().rectangle;
    style.strokeWidth = m_strokeWidth;
    style.stroke = m_strokeColor;
    style.stroke.setAlpha(255);
    style.strokeStyle = m_strokeStyle;
    style.fill = m_fillColor;
    style.fillStyle = m_fillStyle;
    style.cornerRadii = m_cornerRadii;
    style.opacity = m_opacity;
    style.highlightShape = m_highlightShape;
    style.shape = m_shape;
    return style;
}

void ScreenshotToolPaletteRectangleStyleModel::setRectangleStyle(
    const SnowCanvasShapeStyle& style) {
    m_strokeWidth = clampedStrokeWidth(style.strokeWidth);
    m_strokeColor = style.stroke;
    if (m_strokeColor.alpha() == 0) {
        m_strokeColor.setAlpha(255);
    }
    m_strokeStyle = style.strokeStyle;
    m_fillColor = style.fill;
    m_fillStyle = style.fillStyle;
    m_cornerRadii = SnowCanvasCornerRadii{
        clampedCornerRadius(style.cornerRadii.topLeft),
        clampedCornerRadius(style.cornerRadii.topRight),
        clampedCornerRadius(style.cornerRadii.bottomRight),
        clampedCornerRadius(style.cornerRadii.bottomLeft),
    };
    m_opacity = std::clamp(style.opacity, 0.0, 1.0);
    m_highlightShape = style.highlightShape;
    m_shape = style.shape;
}

SnowCanvasRectangleShape ScreenshotToolPaletteRectangleStyleModel::shape() const {
    return m_shape;
}

bool ScreenshotToolPaletteRectangleStyleModel::setShape(SnowCanvasRectangleShape shape) {
    if (m_shape == shape) {
        return false;
    }
    m_shape = shape;
    return true;
}

double ScreenshotToolPaletteRectangleStyleModel::strokeWidth() const {
    return m_strokeWidth;
}

const QColor& ScreenshotToolPaletteRectangleStyleModel::strokeColor() const {
    return m_strokeColor;
}

SnowCanvasStrokeStyle ScreenshotToolPaletteRectangleStyleModel::strokeStyle() const {
    return m_strokeStyle;
}

const QColor& ScreenshotToolPaletteRectangleStyleModel::fillColor() const {
    return m_fillColor;
}

SnowCanvasFillStyle ScreenshotToolPaletteRectangleStyleModel::fillStyle() const {
    return m_fillStyle;
}

int ScreenshotToolPaletteRectangleStyleModel::cornerRadius() const {
    return static_cast<int>(std::lround(m_cornerRadii.topLeft));
}

const QVector<double>& ScreenshotToolPaletteRectangleStyleModel::strokeWidthValues() const {
    return m_strokeWidthValues;
}

const QVector<QColor>& ScreenshotToolPaletteRectangleStyleModel::strokeColorValues() const {
    return m_strokeColorValues;
}

const QVector<QColor>& ScreenshotToolPaletteRectangleStyleModel::fillColorValues() const {
    return m_fillColorValues;
}

bool ScreenshotToolPaletteRectangleStyleModel::stepStrokeWidth(int direction) {
    if (direction == 0) {
        return false;
    }

    const double nextStrokeWidth = clampedStrokeWidth(m_strokeWidth + (direction > 0 ? 1.0 : -1.0));
    if (qFuzzyCompare(nextStrokeWidth + 1.0, m_strokeWidth + 1.0)) {
        return true;
    }

    return setStrokeWidth(nextStrokeWidth);
}

bool ScreenshotToolPaletteRectangleStyleModel::setStrokeWidth(double strokeWidth) {
    strokeWidth = clampedStrokeWidth(strokeWidth);
    if (qFuzzyCompare(m_strokeWidth + 1.0, strokeWidth + 1.0)) {
        return false;
    }

    m_strokeWidth = strokeWidth;
    return true;
}

bool ScreenshotToolPaletteRectangleStyleModel::cycleStrokeWidth() {
    const QVector<double> stepValues =
        strokeWidthStepValues(m_strokeWidthValues, m_minimumStrokeWidth);
    if (stepValues.isEmpty()) {
        return false;
    }

    for (double stepValue : stepValues) {
        if (stepValue > m_strokeWidth) {
            return setStrokeWidth(stepValue);
        }
    }

    return setStrokeWidth(stepValues.first());
}

bool ScreenshotToolPaletteRectangleStyleModel::setStrokeColor(const QColor& color) {
    if (!color.isValid() || m_strokeColor == color) {
        return false;
    }

    m_strokeColor = color;
    return true;
}

bool ScreenshotToolPaletteRectangleStyleModel::setStrokeStyle(
    SnowCanvasStrokeStyle strokeStyle) {
    if (m_strokeStyle == strokeStyle) {
        return false;
    }

    m_strokeStyle = strokeStyle;
    return true;
}

bool ScreenshotToolPaletteRectangleStyleModel::setFillColor(const QColor& color) {
    if (!color.isValid() || m_fillColor == color) {
        return false;
    }

    m_fillColor = color;
    return true;
}

bool ScreenshotToolPaletteRectangleStyleModel::setFillStyle(SnowCanvasFillStyle fillStyle) {
    if (m_fillStyle == fillStyle) {
        return false;
    }

    m_fillStyle = fillStyle;
    return true;
}

bool ScreenshotToolPaletteRectangleStyleModel::stepCornerRadius(int direction) {
    if (direction == 0) {
        return false;
    }

    return setCornerRadius(cornerRadius() + (direction > 0 ? 1 : -1));
}

bool ScreenshotToolPaletteRectangleStyleModel::setCornerRadius(int cornerRadius) {
    const double clampedRadius = clampedCornerRadius(static_cast<double>(cornerRadius));
    if (hasUniformCornerRadius(m_cornerRadii, clampedRadius)) {
        return false;
    }

    m_cornerRadii = SnowCanvasCornerRadii{
        clampedRadius,
        clampedRadius,
        clampedRadius,
        clampedRadius,
    };
    return true;
}

double ScreenshotToolPaletteRectangleStyleModel::clampedStrokeWidth(double strokeWidth) const {
    return std::clamp(strokeWidth, m_minimumStrokeWidth, kMaxRectangleStrokeWidth);
}

double ScreenshotToolPaletteRectangleStyleModel::clampedCornerRadius(double cornerRadius) {
    if (!std::isfinite(cornerRadius)) {
        return snow_shot::presentation::screenshotCanvasStyleDefaults()
            .rectangle.cornerRadii.topLeft;
    }

    return std::clamp(cornerRadius, static_cast<double>(kMinRectangleCornerRadius),
                      static_cast<double>(kMaxRectangleCornerRadius));
}

ScreenshotToolPaletteTextStyleModel::ScreenshotToolPaletteTextStyleModel()
    : m_fontSizeValues{24.0, 30.0, 42.0, 54.0}, m_strokeWidthValues{2.0, 4.0, 8.0},
      m_colorValues{
          QColor(QStringLiteral("#f5222d")), QColor(QStringLiteral("#52c41a")),
          QColor(QStringLiteral("#1677ff")), QColor(QStringLiteral("#fadb14")),
          QColor(QStringLiteral("#000000")),
      },
      m_fillColorValues{
          QColor(0, 0, 0, 0),
          QColor(QStringLiteral("#ffccc7")),
          QColor(QStringLiteral("#d9f7be")),
          QColor(QStringLiteral("#bae0ff")),
          QColor(QStringLiteral("#fff1b8")),
      } {
    reset();
}

void ScreenshotToolPaletteTextStyleModel::reset() {
    setTextStyle(defaultTextStyle());
}

const SnowCanvasTextStyle& ScreenshotToolPaletteTextStyleModel::textStyle() const {
    return m_style;
}

void ScreenshotToolPaletteTextStyleModel::setTextStyle(const SnowCanvasTextStyle& style) {
    m_style = style;
    if (!m_style.color.isValid()) {
        m_style.color = defaultTextStyle().color;
    }
    if (!m_style.stroke.isValid()) {
        m_style.stroke = defaultTextStyle().stroke;
    }
    if (!m_style.fill.isValid()) {
        m_style.fill = defaultTextStyle().fill;
    }
    m_style.fontSize = clampedFontSize(style.fontSize);
    m_style.fontFamily = style.fontFamily.trimmed();
    m_style.strokeWidth = clampedStrokeWidth(style.strokeWidth);
    m_style.cornerRadii.topLeft = clampedCornerRadius(style.cornerRadii.topLeft);
    m_style.cornerRadii.topRight = clampedCornerRadius(style.cornerRadii.topRight);
    m_style.cornerRadii.bottomRight = clampedCornerRadius(style.cornerRadii.bottomRight);
    m_style.cornerRadii.bottomLeft = clampedCornerRadius(style.cornerRadii.bottomLeft);
}

const QVector<double>& ScreenshotToolPaletteTextStyleModel::fontSizeValues() const {
    return m_fontSizeValues;
}

const QVector<double>& ScreenshotToolPaletteTextStyleModel::strokeWidthValues() const {
    return m_strokeWidthValues;
}

const QVector<QColor>& ScreenshotToolPaletteTextStyleModel::colorValues() const {
    return m_colorValues;
}

const QVector<QColor>& ScreenshotToolPaletteTextStyleModel::fillColorValues() const {
    return m_fillColorValues;
}

bool ScreenshotToolPaletteTextStyleModel::setColor(const QColor& color) {
    if (!color.isValid() || m_style.color == color) {
        return false;
    }
    m_style.color = color;
    return true;
}

bool ScreenshotToolPaletteTextStyleModel::setFontSize(double fontSize) {
    fontSize = clampedFontSize(fontSize);
    if (fuzzyEqual(m_style.fontSize, fontSize)) {
        return false;
    }
    m_style.fontSize = fontSize;
    return true;
}

bool ScreenshotToolPaletteTextStyleModel::stepFontSize(int direction) {
    if (direction == 0) {
        return false;
    }
    return setFontSize(m_style.fontSize + (direction > 0 ? 1.0 : -1.0));
}

bool ScreenshotToolPaletteTextStyleModel::cycleFontSize() {
    for (double value : m_fontSizeValues) {
        if (value > m_style.fontSize + 0.001) {
            return setFontSize(value);
        }
    }
    return setFontSize(m_fontSizeValues.front());
}

bool ScreenshotToolPaletteTextStyleModel::setFontFamily(const QString& fontFamily) {
    const QString trimmed = fontFamily.trimmed();
    if (m_style.fontFamily == trimmed) {
        return false;
    }
    m_style.fontFamily = trimmed;
    return true;
}

bool ScreenshotToolPaletteTextStyleModel::setStrokeColor(const QColor& color) {
    if (!color.isValid() || m_style.stroke == color) {
        return false;
    }
    m_style.stroke = color;
    return true;
}

bool ScreenshotToolPaletteTextStyleModel::setStrokeWidth(double strokeWidth) {
    strokeWidth = clampedStrokeWidth(strokeWidth);
    if (fuzzyEqual(m_style.strokeWidth, strokeWidth)) {
        return false;
    }
    m_style.strokeWidth = strokeWidth;
    return true;
}

bool ScreenshotToolPaletteTextStyleModel::stepStrokeWidth(int direction) {
    if (direction == 0) {
        return false;
    }
    return setStrokeWidth(m_style.strokeWidth + (direction > 0 ? 1.0 : -1.0));
}

bool ScreenshotToolPaletteTextStyleModel::setFillColor(const QColor& color) {
    if (!color.isValid() || m_style.fill == color) {
        return false;
    }
    m_style.fill = color;
    return true;
}

bool ScreenshotToolPaletteTextStyleModel::setFillStyle(SnowCanvasFillStyle fillStyle) {
    if (m_style.fillStyle == fillStyle) {
        return false;
    }
    m_style.fillStyle = fillStyle;
    return true;
}

bool ScreenshotToolPaletteTextStyleModel::setCornerRadius(int cornerRadius) {
    const double value = clampedCornerRadius(static_cast<double>(cornerRadius));
    const SnowCanvasCornerRadii radii{value, value, value, value};
    if (fuzzyEqual(m_style.cornerRadii.topLeft, value) &&
        fuzzyEqual(m_style.cornerRadii.topRight, value) &&
        fuzzyEqual(m_style.cornerRadii.bottomRight, value) &&
        fuzzyEqual(m_style.cornerRadii.bottomLeft, value)) {
        return false;
    }
    m_style.cornerRadii = radii;
    return true;
}

bool ScreenshotToolPaletteTextStyleModel::stepCornerRadius(int direction) {
    if (direction == 0) {
        return false;
    }
    return setCornerRadius(qRound(m_style.cornerRadii.topLeft) + (direction > 0 ? 1 : -1));
}

bool ScreenshotToolPaletteTextStyleModel::setHorizontalAlign(
    SnowCanvasTextHorizontalAlign alignment) {
    if (m_style.horizontalAlign == alignment) {
        return false;
    }
    m_style.horizontalAlign = alignment;
    return true;
}

double ScreenshotToolPaletteTextStyleModel::clampedFontSize(double fontSize) {
    if (!std::isfinite(fontSize)) {
        return defaultTextStyle().fontSize;
    }
    return std::clamp(fontSize, kMinTextFontSize, kMaxTextFontSize);
}

double ScreenshotToolPaletteTextStyleModel::clampedStrokeWidth(double strokeWidth) {
    if (!std::isfinite(strokeWidth)) {
        return kMinTextStrokeWidth;
    }
    return std::clamp(strokeWidth, kMinTextStrokeWidth, kMaxTextStrokeWidth);
}

double ScreenshotToolPaletteTextStyleModel::clampedCornerRadius(double cornerRadius) {
    if (!std::isfinite(cornerRadius)) {
        return kMinTextCornerRadius;
    }
    return std::clamp(cornerRadius, static_cast<double>(kMinTextCornerRadius),
                      static_cast<double>(kMaxTextCornerRadius));
}

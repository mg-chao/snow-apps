#include "screenshottoolpalettestylepresets.h"

#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"

namespace snow_shot::presentation::style_presets {
namespace {
namespace custom_outlined_icons = snow_shot::presentation::icons::custom::outlined;

QVector<QColor> defaultAnchoredColors(const QColor& anchor) {
    return QVector<QColor>{
        anchor,
        QColor(QStringLiteral("#52c41a")),
        QColor(QStringLiteral("#1677ff")),
        QColor(QStringLiteral("#fadb14")),
        QColor(QStringLiteral("#000000")),
    };
}
} // namespace

const QVector<QColor>& strokeColors() {
    static const QVector<QColor> values = defaultAnchoredColors(
        snow_shot::presentation::screenshotCanvasStyleDefaults().rectangle.stroke);
    return values;
}

const QVector<QColor>& shapeFillColors() {
    static const QVector<QColor> values{
        snow_shot::presentation::screenshotCanvasStyleDefaults().rectangle.fill,
        QColor(QStringLiteral("#ffccc7")),
        QColor(QStringLiteral("#d9f7be")),
        QColor(QStringLiteral("#bae0ff")),
        QColor(QStringLiteral("#fff1b8")),
    };
    return values;
}

const QVector<QColor>& textColors() {
    static const QVector<QColor> values{
        QColor(QStringLiteral("#f5222d")), QColor(QStringLiteral("#52c41a")),
        QColor(QStringLiteral("#1677ff")), QColor(QStringLiteral("#fadb14")),
        QColor(QStringLiteral("#000000")),
    };
    return values;
}

const QVector<QColor>& textFillColors() {
    static const QVector<QColor> values{
        QColor(0, 0, 0, 0),
        QColor(QStringLiteral("#ffccc7")),
        QColor(QStringLiteral("#d9f7be")),
        QColor(QStringLiteral("#bae0ff")),
        QColor(QStringLiteral("#fff1b8")),
    };
    return values;
}

const QVector<double>& shapeStrokeWidths() {
    static const QVector<double> values{
        snow_shot::presentation::screenshotCanvasStyleDefaults().rectangle.strokeWidth,
        4.0,
        8.0,
    };
    return values;
}

const QVector<double>& strokePresetWidths() {
    static const QVector<double> values{2.0, 4.0, 8.0};
    return values;
}

const QVector<double>& sizePresetValues() {
    static const QVector<double> values{24.0, 30.0, 42.0, 54.0};
    return values;
}

const QStringList& sizePresetLabels() {
    static const QStringList labels{
        QStringLiteral("S"),
        QStringLiteral("M"),
        QStringLiteral("L"),
        QStringLiteral("XL"),
    };
    return labels;
}

adqt::icons::IconRef sizePresetIcon(int index) {
    switch (index) {
    case 0:
        return custom_outlined_icons::FontSizeSmall();
    case 1:
        return custom_outlined_icons::FontSizeMedium();
    case 2:
        return custom_outlined_icons::FontSizeLarge();
    case 3:
        return custom_outlined_icons::FontSizeVeryLarge();
    default:
        return custom_outlined_icons::FontSizeMedium();
    }
}

const QVector<double>& fontSizes() {
    // The font size presets intentionally use the same S/M/L/XL values as the
    // pen width presets; both render through sizePresetIcon/sizePresetLabels.
    return sizePresetValues();
}

const QVector<double>& watermarkFontSizes() {
    static const QVector<double> values{12.0, 16.0, 24.0, 30.0};
    return values;
}

ScreenshotToolPaletteTranslationText sizePresetTooltip(const char* pattern, int index,
                                                       double value) {
    return ScreenshotToolPaletteTranslationText(pattern)
        .arg(sizePresetLabels().value(index))
        .arg(value, 0, 'g', 3);
}

} // namespace snow_shot::presentation::style_presets

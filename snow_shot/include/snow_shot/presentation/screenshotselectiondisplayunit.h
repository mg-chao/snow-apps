#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONDISPLAYUNIT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONDISPLAYUNIT_H

#include <QCoreApplication>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <cmath>
#include <optional>

enum class ScreenshotSelectionDisplayUnit { PhysicalPixels, LogicalPixels };

#ifdef Q_OS_MACOS
inline constexpr auto kDefaultScreenshotSelectionDisplayUnit =
    ScreenshotSelectionDisplayUnit::LogicalPixels;
#else
inline constexpr auto kDefaultScreenshotSelectionDisplayUnit =
    ScreenshotSelectionDisplayUnit::PhysicalPixels;
#endif

inline QString screenshotSelectionDisplayUnitId(ScreenshotSelectionDisplayUnit unit) {
    return unit == ScreenshotSelectionDisplayUnit::LogicalPixels
               ? QStringLiteral("logical_pixels")
               : QStringLiteral("physical_pixels");
}

inline ScreenshotSelectionDisplayUnit screenshotSelectionDisplayUnitFromId(const QString& id) {
    if (id == QStringLiteral("logical_pixels"))
        return ScreenshotSelectionDisplayUnit::LogicalPixels;
    if (id == QStringLiteral("physical_pixels"))
        return ScreenshotSelectionDisplayUnit::PhysicalPixels;
    return kDefaultScreenshotSelectionDisplayUnit;
}

inline QString screenshotSelectionDisplayValue(qreal value) {
    const qreal rounded = std::round(value);
    return rounded == 0.0 ? QStringLiteral("0") : QString::number(rounded, 'f', 0);
}

struct ScreenshotCoordinateDisplayValues {
    QPointF position;
    ScreenshotSelectionDisplayUnit unit = ScreenshotSelectionDisplayUnit::PhysicalPixels;
    bool canvasUsesPoints = false;
    std::optional<QPointF> relativePosition = std::nullopt;
    bool operator==(const ScreenshotCoordinateDisplayValues&) const = default;
};

struct ScreenshotSelectionDisplayValues {
    QPointF position;
    QSizeF size{0, 0};
    ScreenshotSelectionDisplayUnit unit = ScreenshotSelectionDisplayUnit::PhysicalPixels;
    bool canvasUsesPoints = false;
    bool operator==(const ScreenshotSelectionDisplayValues&) const = default;
};

inline QString screenshotSelectionDisplayUnitText(ScreenshotSelectionDisplayUnit unit) {
    return unit == ScreenshotSelectionDisplayUnit::PhysicalPixels
               ? QCoreApplication::translate("ScreenshotSelectionToolbarWidget", "px")
               : QCoreApplication::translate("ScreenshotSelectionToolbarWidget", "dp");
}

inline QString screenshotSelectionDisplayUnitDescription(ScreenshotSelectionDisplayUnit unit) {
    return unit == ScreenshotSelectionDisplayUnit::PhysicalPixels
               ? QCoreApplication::translate("ScreenshotSelectionToolbarWidget", "Pixels")
               : QCoreApplication::translate("ScreenshotSelectionToolbarWidget", "Logical pixels");
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONDISPLAYUNIT_H

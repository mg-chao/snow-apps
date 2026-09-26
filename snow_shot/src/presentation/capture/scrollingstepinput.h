#pragma once
#include <QPoint>
#include <QString>
#include <optional>
namespace snow_shot::capture_detail {
inline std::optional<QPoint> scrollingStepDelta(const QString& direction) {
    if (direction == QStringLiteral("up"))
        return QPoint(0, 120);
    if (direction == QStringLiteral("down"))
        return QPoint(0, -120);
    if (direction == QStringLiteral("left"))
        return QPoint(-120, 0);
    if (direction == QStringLiteral("right"))
        return QPoint(120, 0);
    return std::nullopt;
}
} // namespace snow_shot::capture_detail

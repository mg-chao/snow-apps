#include "snow_shot/storage/persistedwindowgeometry.h"

#include <QString>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>

namespace snow_shot::storage {
namespace {
constexpr int kMaximumExtent = 32767;

bool integerField(const QJsonObject& value, const QString& key, int* result) {
    const QJsonValue field = value.value(key);
    if (!field.isDouble()) {
        return false;
    }
    const double number = field.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < double(std::numeric_limits<int>::min()) ||
        number > double(std::numeric_limits<int>::max())) {
        return false;
    }
    *result = int(number);
    return true;
}

qint64 area(const QRect& rect) {
    return qint64(rect.width()) * qint64(rect.height());
}
} // namespace

std::optional<PersistedWindowGeometry> parseWindowGeometry(const QJsonObject& value) {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    if (!integerField(value, QStringLiteral("x"), &x) ||
        !integerField(value, QStringLiteral("y"), &y) ||
        !integerField(value, QStringLiteral("width"), &width) ||
        !integerField(value, QStringLiteral("height"), &height)) {
        return std::nullopt;
    }
    if (width < 1 || width > kMaximumExtent || height < 1 || height > kMaximumExtent) {
        return std::nullopt;
    }
    const QJsonValue maximized = value.value(QStringLiteral("maximized"));
    if (!maximized.isUndefined() && !maximized.isBool()) {
        return std::nullopt;
    }
    PersistedWindowGeometry geometry;
    geometry.normalGeometry = QRect(x, y, width, height);
    geometry.maximized = maximized.toBool(false);
    return geometry;
}

QJsonObject windowGeometryToJson(const QRect& normalGeometry, bool maximized) {
    return {{QStringLiteral("x"), normalGeometry.x()},
            {QStringLiteral("y"), normalGeometry.y()},
            {QStringLiteral("width"), normalGeometry.width()},
            {QStringLiteral("height"), normalGeometry.height()},
            {QStringLiteral("maximized"), maximized}};
}

std::optional<QSize> parseWindowSize(const QJsonObject& value) {
    int width = 0;
    int height = 0;
    if (!integerField(value, QStringLiteral("width"), &width) ||
        !integerField(value, QStringLiteral("height"), &height)) {
        return std::nullopt;
    }
    if (width < 1 || width > kMaximumExtent || height < 1 || height > kMaximumExtent) {
        return std::nullopt;
    }
    return QSize(width, height);
}

QJsonObject windowSizeToJson(const QSize& size) {
    return {{QStringLiteral("width"), size.width()}, {QStringLiteral("height"), size.height()}};
}

QRect clampWindowGeometryToScreens(const QRect& geometry, const QList<QRect>& availableGeometries) {
    if (availableGeometries.isEmpty() || geometry.isEmpty()) {
        return geometry;
    }
    qint64 bestIntersectionArea = 0;
    for (const QRect& available : availableGeometries) {
        bestIntersectionArea =
            std::max(bestIntersectionArea, area(available.intersected(geometry)));
    }
    if (bestIntersectionArea > 0) {
        return geometry;
    }
    const QRect& available = availableGeometries.first();
    QRect clamped = geometry;
    clamped.setSize(clamped.size().boundedTo(available.size()));
    clamped.moveLeft(
        clamped.width() >= available.width()
            ? available.left()
            : qBound(available.left(), clamped.left(), available.right() - clamped.width() + 1));
    clamped.moveTop(
        clamped.height() >= available.height()
            ? available.top()
            : qBound(available.top(), clamped.top(), available.bottom() - clamped.height() + 1));
    return clamped;
}

QSize clampWindowSize(const QSize& size, const QSize& minimum, const QSize& maximum) {
    return size.expandedTo(minimum).boundedTo(maximum);
}

PersistedWindowGeometry fitPersistedWindowGeometry(const PersistedWindowGeometry& saved,
                                                   const QSize& minimumSize,
                                                   const QList<QRect>& availableGeometries) {
    PersistedWindowGeometry fitted = saved;
    QSize clampedSize = fitted.normalGeometry.size().expandedTo(minimumSize);
    if (!availableGeometries.isEmpty()) {
        QSize largestAvailableSize(1, 1);
        for (const QRect& available : availableGeometries) {
            largestAvailableSize = largestAvailableSize.expandedTo(available.size());
        }
        clampedSize = clampedSize.boundedTo(largestAvailableSize);
    }
    fitted.normalGeometry.setSize(clampedSize);
    fitted.normalGeometry =
        clampWindowGeometryToScreens(fitted.normalGeometry, availableGeometries);
    return fitted;
}

} // namespace snow_shot::storage

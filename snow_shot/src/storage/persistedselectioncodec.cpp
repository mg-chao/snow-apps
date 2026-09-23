#include "snow_shot/storage/persistedselectioncodec.h"

#include <QColor>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <limits>

namespace snow_shot::storage {
namespace {
constexpr int kMaximumCornerRadius = 256;
constexpr int kMaximumShadowWidth = 64;

bool integer(const QJsonValue& value, int minimum, int maximum, int* result) {
    if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
        std::floor(value.toDouble()) != value.toDouble() || value.toDouble() < minimum ||
        value.toDouble() > maximum) {
        return false;
    }
    if (result != nullptr) {
        *result = value.toInt();
    }
    return true;
}

bool rectangle(const QJsonValue& value, QRect* result) {
    if (result == nullptr || !value.isObject()) {
        return false;
    }
    const QJsonObject object = value.toObject();
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    if (!integer(object.value(QStringLiteral("x")), std::numeric_limits<int>::min(),
                 std::numeric_limits<int>::max(), &x) ||
        !integer(object.value(QStringLiteral("y")), std::numeric_limits<int>::min(),
                 std::numeric_limits<int>::max(), &y) ||
        !integer(object.value(QStringLiteral("width")), 1, std::numeric_limits<int>::max(),
                 &width) ||
        !integer(object.value(QStringLiteral("height")), 1, std::numeric_limits<int>::max(),
                 &height)) {
        return false;
    }
    if (qint64(x) + width > std::numeric_limits<int>::max() ||
        qint64(y) + height > std::numeric_limits<int>::max())
        return false;
    *result = QRect(x, y, width, height);
    return true;
}
} // namespace

QJsonObject persistedSelectionToJson(const PersistedSelection& selection) {
    const QRect& rectangle = selection.rectangle;
    QJsonObject result{
        {QStringLiteral("rectangle"), QJsonObject{{QStringLiteral("x"), rectangle.x()},
                                                  {QStringLiteral("y"), rectangle.y()},
                                                  {QStringLiteral("width"), rectangle.width()},
                                                  {QStringLiteral("height"), rectangle.height()}}},
        {QStringLiteral("corner_radius"), selection.cornerRadius},
        {QStringLiteral("shadow_width"), selection.shadowWidth},
        {QStringLiteral("shadow_color"), selection.shadowColor.name(QColor::HexArgb).toUpper()},
        {QStringLiteral("lock_aspect_ratio"), selection.lockAspectRatio},
        {QStringLiteral("lock_drag_aspect_ratio"), selection.lockDragAspectRatio},
    };
    if (selection.region && selection.region->custom()) {
        result.insert(QStringLiteral("geometry"), selection.region->toJson());
    } else if (selection.region) {
        QJsonArray regions;
        for (const QRect& rect : *selection.region) {
            regions.push_back(QJsonObject{{QStringLiteral("x"), rect.x()},
                                          {QStringLiteral("y"), rect.y()},
                                          {QStringLiteral("width"), rect.width()},
                                          {QStringLiteral("height"), rect.height()}});
        }
        result.insert(QStringLiteral("regions"), regions);
    }
    return result;
}

PersistedSelectionNormalization normalizePersistedSelection(const QJsonValue& value) {
    if (!value.isObject()) {
        return {};
    }
    const QJsonObject object = value.toObject();
    PersistedSelection selection;
    int cornerRadius = 0;
    int shadowWidth = 0;
    const QJsonValue colorValue = object.value(QStringLiteral("shadow_color"));
    const QColor color(colorValue.toString());
    if (!rectangle(object.value(QStringLiteral("rectangle")), &selection.rectangle) ||
        !integer(object.value(QStringLiteral("corner_radius")), 0, kMaximumCornerRadius,
                 &cornerRadius) ||
        !integer(object.value(QStringLiteral("shadow_width")), 0, kMaximumShadowWidth,
                 &shadowWidth) ||
        !colorValue.isString() || !color.isValid() ||
        !object.value(QStringLiteral("lock_aspect_ratio")).isBool() ||
        !object.value(QStringLiteral("lock_drag_aspect_ratio")).isBool()) {
        return {};
    }
    if (object.contains(QStringLiteral("geometry"))) {
        const auto geometry =
            ScreenshotRegionGeometry::fromJson(object.value(QStringLiteral("geometry")));
        if (!geometry || geometry->isEmpty())
            return {};
        selection.region = *geometry;
        selection.rectangle = geometry->boundingRect();
    } else if (object.contains(QStringLiteral("regions"))) {
        const auto regionValue = object.value(QStringLiteral("regions"));
        if (!regionValue.isArray() || regionValue.toArray().isEmpty() ||
            regionValue.toArray().size() > 65536)
            return {};
        QRegion region;
        for (const auto& item : regionValue.toArray()) {
            QRect rect;
            if (!rectangle(item, &rect))
                return {};
            if (!region.isEmpty()) {
                const QRect previous = region.boundingRect();
                if (qint64(std::max(previous.right(), rect.right())) -
                            std::min(previous.left(), rect.left()) >=
                        std::numeric_limits<int>::max() ||
                    qint64(std::max(previous.bottom(), rect.bottom())) -
                            std::min(previous.top(), rect.top()) >=
                        std::numeric_limits<int>::max())
                    return {};
            }
            region += rect;
        }
        const QRect bounds = region.boundingRect();
        if (!bounds.isValid() || bounds.isEmpty())
            return {};
        selection.region = region;
        selection.rectangle = bounds;
    }
    selection.cornerRadius = cornerRadius;
    selection.shadowWidth = shadowWidth;
    selection.shadowColor = color;
    selection.lockAspectRatio = object.value(QStringLiteral("lock_aspect_ratio")).toBool();
    selection.lockDragAspectRatio = object.value(QStringLiteral("lock_drag_aspect_ratio")).toBool();
    const QJsonObject normalized = persistedSelectionToJson(selection);
    return {selection, true, normalized != object};
}
} // namespace snow_shot::storage

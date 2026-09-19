#ifndef SNOW_SHOT_STORAGE_PINNEDWINDOWPLACEMENT_H
#define SNOW_SHOT_STORAGE_PINNEDWINDOWPLACEMENT_H

#include <QPointF>
#include <QSize>
#include <QString>
#include <cmath>

namespace snow_shot::storage {
// Positions belong to one display, in points. Extents belong to the image, in
// backing pixels. There is deliberately no global physical desktop rectangle.
struct PinnedWindowPlacement {
    QString displayName;
    QString displaySerial;
    QPointF position;
    QSize pixelSize;

    [[nodiscard]] bool isValid() const {
        return std::isfinite(position.x()) && std::isfinite(position.y()) &&
               pixelSize.width() > 0 && pixelSize.height() > 0;
    }
    friend bool operator==(const PinnedWindowPlacement&, const PinnedWindowPlacement&) = default;
};
} // namespace snow_shot::storage
#endif

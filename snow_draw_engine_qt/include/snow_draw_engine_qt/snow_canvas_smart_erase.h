#pragma once

#include "snow_canvas_image_source_types.h"

#include <QImage>
#include <QByteArray>
#include <QList>
#include <QRectF>
#include <memory>

// Immutable, implicitly shared appearance captured with a document for worker exports.
class SnowCanvasSmartEraseSnapshot {
  public:
    struct Data;
    std::shared_ptr<const Data> data;
    // Stable while geometry and immutable reconstruction results remain unchanged.
    QByteArray cacheKey() const;
};

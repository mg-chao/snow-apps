#pragma once

#include <QImage>
#include <QList>
#include <QRectF>
#include <memory>

// Original image pixels only. coverage may restrict a layer to a subrectangle.
struct SnowCanvasBaseImageSource {
    QImage image;
    QRectF canvasRect;
    QRectF coverage;
};

// Immutable, implicitly shared appearance captured with a document for worker exports.
class SnowCanvasSmartEraseSnapshot {
  public:
    struct Data;
    std::shared_ptr<const Data> data;
};

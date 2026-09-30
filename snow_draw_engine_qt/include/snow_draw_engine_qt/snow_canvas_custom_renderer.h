#pragma once

#include <QRect>
#include <QRegion>
#include <QTransform>
#include <QtGlobal>

#include <cstdint>

class QPainter;

struct SnowCanvasRenderContext {
    QRect viewportRect;
    QRegion exposedRegion;
    QTransform canvasToViewTransform;
    qreal devicePixelRatio = 1.0;
};

class SnowCanvasCustomRenderer {
  public:
    virtual ~SnowCanvasCustomRenderer() = default;

    // Increment when renderBeforeCanvas() would produce different pixels.
    // The canvas uses this revision to invalidate its cached background tiles.
    [[nodiscard]] virtual std::uint64_t contentRevision() const;

    // Releases derived data while preserving the sources and rendered content.
    virtual void clearRenderState();

    virtual void renderBeforeCanvas(QPainter& painter, const SnowCanvasRenderContext& context);
    virtual void renderAfterCanvas(QPainter& painter, const SnowCanvasRenderContext& context);
};

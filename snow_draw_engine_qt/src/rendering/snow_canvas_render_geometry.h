#pragma once

#include <QPainterPath>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QRegion>
#include <QVector>

#include "snow_canvas_display_cache.h"
#include "snow_draw_engine.h"

#include <cstdint>

namespace snow_canvas_render_geometry {

struct ViewProjection {
    double cameraCenterX = 0.0;
    double cameraCenterY = 0.0;
    double cameraZoom = 1.0;
    double surfaceWidth = 0.0;
    double surfaceHeight = 0.0;
};

struct ViewCornerRadii {
    double topLeft = 0.0;
    double topRight = 0.0;
    double bottomRight = 0.0;
    double bottomLeft = 0.0;
};

ViewProjection sceneProjection(const SceneDisplayInfo& displayInfo);
ViewProjection overlayProjection(const OverlayDisplayInfo& displayInfo);

QPointF canvasToView(double cameraCenterX, double cameraCenterY, double cameraZoom,
                     double surfaceWidth, double surfaceHeight, double x, double y);
QPointF canvasToView(const ViewProjection& projection, double x, double y);

QPointF viewToCanvas(double cameraCenterX, double cameraCenterY, double cameraZoom,
                     double surfaceWidth, double surfaceHeight, const QPointF& position);
QPointF viewToCanvas(const ViewProjection& projection, const QPointF& position);

QRect alignedRectForBounds(const QRectF& bounds, int paddingPx = 1);
QRectF pointBounds(const QPointF& point, double radius);
QRectF rotatedRectBounds(const QPointF& center, double width, double height, double rotationRadians,
                         double strokeWidth);

QPointF rotatePoint(const QPointF& point, const QPointF& center, double radians);
bool arrowEndpointGeometry(const QVector<QPointF>& points, SnowArrowType arrowType, bool atStart,
                           QPointF* outEndpoint, QPointF* outPreviousPoint, QPointF* outDirection,
                           double* outSegmentLength);

std::uint32_t clampedArrowPointCount(std::uint32_t count);
QVector<QPointF> arrowPointsToView(double cameraCenterX, double cameraCenterY, double cameraZoom,
                                   double surfaceWidth, double surfaceHeight,
                                   const SnowArrowPoint* points, std::uint32_t pointCount);
QVector<QPointF> arrowPointsToView(const ViewProjection& projection, const SnowArrowPoint* points,
                                   std::uint32_t pointCount);
double arrowheadSize(SnowArrowhead head);
double arrowheadAngleDegrees(SnowArrowhead head);
QPainterPath arrowPathForPoints(const QVector<QPointF>& points, SnowArrowType arrowType,
                                double zoom);
QPainterPath arrowPathFromCommands(const ViewProjection& projection,
                                   const SnowArrowPathCommand* commands,
                                   std::uint32_t commandCount);

QRectF sceneItemBounds(const SceneDisplayInfo& displayInfo, const SnowSceneDisplayItem& item);
QRectF sceneItemBounds(const SceneDisplayInfo& displayInfo, const SnowCanvasSceneItem& item);
QRectF overlayItemBounds(const OverlayDisplayInfo& displayInfo, const SnowOverlayDisplayItem& item);

ViewCornerRadii toViewCornerRadii(const SnowCornerRadii& radii, double zoom, const QRectF& rect);
QPainterPath roundedRectPath(const QRectF& rect, const ViewCornerRadii& radii);

} // namespace snow_canvas_render_geometry

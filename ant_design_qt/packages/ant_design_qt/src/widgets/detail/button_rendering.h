#pragma once

#include "../button.h"

#include <QPainterPath>
#include <QPen>
#include <QRectF>
#include <QtGlobal>

#include <algorithm>

namespace adqt::widgets::detail {

struct ButtonCornerRadii {
  qreal topLeft = 0.0;
  qreal topRight = 0.0;
  qreal bottomRight = 0.0;
  qreal bottomLeft = 0.0;
};

inline QPainterPath roundedButtonPath(const QRectF& rect, qreal topLeft, qreal topRight,
                                      qreal bottomRight, qreal bottomLeft) {
  const qreal width = std::max(rect.width(), 0.0);
  const qreal height = std::max(rect.height(), 0.0);
  const qreal maxRadius = std::min(width, height) / 2.0;

  topLeft = std::clamp(topLeft, 0.0, maxRadius);
  topRight = std::clamp(topRight, 0.0, maxRadius);
  bottomRight = std::clamp(bottomRight, 0.0, maxRadius);
  bottomLeft = std::clamp(bottomLeft, 0.0, maxRadius);

  const qreal left = rect.left();
  const qreal top = rect.top();
  const qreal right = left + width;
  const qreal bottom = top + height;

  QPainterPath path;
  path.moveTo(left + topLeft, top);
  path.lineTo(right - topRight, top);
  if (topRight > 0.0) {
    path.arcTo(QRectF(right - 2.0 * topRight, top, 2.0 * topRight, 2.0 * topRight), 90.0, -90.0);
  }
  path.lineTo(right, bottom - bottomRight);
  if (bottomRight > 0.0) {
    path.arcTo(QRectF(right - 2.0 * bottomRight, bottom - 2.0 * bottomRight, 2.0 * bottomRight,
                      2.0 * bottomRight),
               0.0, -90.0);
  }
  path.lineTo(left + bottomLeft, bottom);
  if (bottomLeft > 0.0) {
    path.arcTo(QRectF(left, bottom - 2.0 * bottomLeft, 2.0 * bottomLeft, 2.0 * bottomLeft), 270.0,
               -90.0);
  }
  path.lineTo(left, top + topLeft);
  if (topLeft > 0.0) {
    path.arcTo(QRectF(left, top, 2.0 * topLeft, 2.0 * topLeft), 180.0, -90.0);
  }
  path.closeSubpath();
  return path;
}

inline QRectF joinedButtonBorderRect(const QRect& bounds, qreal borderWidth, bool joinedLeft,
                                     bool joinedRight) {
  const qreal half = std::max<qreal>(0.0, borderWidth / 2.0);
  qreal leftInset = half + 0.5;
  qreal rightInset = half + 0.5;
  if (joinedLeft) {
    leftInset = half;
  }
  if (joinedRight) {
    rightInset = half;
  }
  return QRectF(bounds).adjusted(leftInset, half + 0.5, -rightInset, -half - 0.5);
}

inline QRectF resolveButtonShapeRect(const QRectF& rect, AdButton::Shape shape) {
  if (shape != AdButton::Shape::Circle) {
    return rect;
  }

  const qreal side = std::max<qreal>(0.0, std::min(rect.width(), rect.height()));
  return QRectF(rect.center().x() - side / 2.0, rect.center().y() - side / 2.0, side, side);
}

inline qreal buttonShapeRadius(AdButton::Shape shape, const QRectF& rect, int borderRadius) {
  switch (shape) {
    case AdButton::Shape::Rectangle:
      return 0.0;
    case AdButton::Shape::Pill:
      return rect.height() / 2.0;
    case AdButton::Shape::Circle:
      return std::min(rect.width(), rect.height()) / 2.0;
    case AdButton::Shape::Rounded:
    default:
      return static_cast<qreal>(borderRadius);
  }
}

inline ButtonCornerRadii resolveButtonCorners(AdButton::Shape shape, const QRectF& rect,
                                              int borderRadius, bool joinedLeft, bool joinedRight) {
  const qreal radius = buttonShapeRadius(shape, rect, borderRadius);
  ButtonCornerRadii corners{radius, radius, radius, radius};
  if (joinedLeft) {
    corners.topLeft = 0.0;
    corners.bottomLeft = 0.0;
  }
  if (joinedRight) {
    corners.topRight = 0.0;
    corners.bottomRight = 0.0;
  }
  return corners;
}

inline QPen makeButtonBorderPen(const QColor& color, qreal width, Qt::PenStyle style) {
  QPen pen(color, width, style, Qt::SquareCap, Qt::MiterJoin);
  if (style == Qt::DashLine) {
    pen.setStyle(Qt::CustomDashLine);
    const qreal penWidth = std::max<qreal>(1.0, pen.widthF());
    pen.setDashPattern({3.0 / penWidth, 2.0 / penWidth});
    pen.setCapStyle(Qt::FlatCap);
    pen.setJoinStyle(Qt::RoundJoin);
  }
  return pen;
}

inline qreal deviceAlignedPenWidth(qreal logicalWidth, qreal devicePixelRatio) {
  if (devicePixelRatio <= 0.0) {
    return logicalWidth;
  }
  return std::max<qreal>(1.0 / devicePixelRatio,
                         qRound(logicalWidth * devicePixelRatio) / devicePixelRatio);
}

inline qreal deviceAlignedStrokeCenter(qreal logicalCoordinate, qreal logicalWidth,
                                       qreal devicePixelRatio) {
  if (devicePixelRatio <= 0.0) {
    return logicalCoordinate;
  }
  const int physicalWidth = std::max(1, qRound(logicalWidth * devicePixelRatio));
  const qreal phase = (physicalWidth % 2 == 0) ? 0.0 : 0.5;
  return (qRound(logicalCoordinate * devicePixelRatio - phase) + phase) / devicePixelRatio;
}

}  // namespace adqt::widgets::detail

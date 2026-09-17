#pragma once

#include <QColor>
#include <QMargins>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPoint>
#include <QRect>
#include <QSize>

#include <algorithm>
#include <cmath>

namespace adqt::widgets::detail {

inline QMargins antPopupShadowSecondaryMargins() { return QMargins(18, 12, 18, 24); }

inline QSize addAntPopupShadowMargins(const QSize& visualSize) {
  const QMargins margins = antPopupShadowSecondaryMargins();
  return QSize(std::max(1, visualSize.width() + margins.left() + margins.right()),
               std::max(1, visualSize.height() + margins.top() + margins.bottom()));
}

inline QSize removeAntPopupShadowMargins(const QSize& frameSize) {
  const QMargins margins = antPopupShadowSecondaryMargins();
  return QSize(std::max(1, frameSize.width() - margins.left() - margins.right()),
               std::max(1, frameSize.height() - margins.top() - margins.bottom()));
}

inline QPoint antPopupShadowFrameTopLeftForVisualTopLeft(const QPoint& visualTopLeft) {
  const QMargins margins = antPopupShadowSecondaryMargins();
  return QPoint(visualTopLeft.x() - margins.left(), visualTopLeft.y() - margins.top());
}

inline QRectF antPopupShadowVisualRect(const QRect& frameRect) {
  const QMargins margins = antPopupShadowSecondaryMargins();
  return QRectF(frameRect).adjusted(margins.left(), margins.top(), -margins.right(),
                                    -margins.bottom());
}

inline QMargins addAntPopupShadowMarginsToPadding(const QMargins& padding) {
  const QMargins margins = antPopupShadowSecondaryMargins();
  return QMargins(padding.left() + margins.left(), padding.top() + margins.top(),
                  padding.right() + margins.right(), padding.bottom() + margins.bottom());
}

inline QMargins removeAntPopupShadowMarginsFromPadding(const QMargins& padding) {
  const QMargins margins = antPopupShadowSecondaryMargins();
  return QMargins(std::max(0, padding.left() - margins.left()),
                  std::max(0, padding.top() - margins.top()),
                  std::max(0, padding.right() - margins.right()),
                  std::max(0, padding.bottom() - margins.bottom()));
}

inline QPainterPath expandedPopupShadowPath(const QPainterPath& path, qreal amount) {
  if (amount <= 0.0) {
    return path;
  }

  QPainterPathStroker stroker;
  stroker.setWidth(amount * 2.0);
  stroker.setJoinStyle(Qt::RoundJoin);
  stroker.setCapStyle(Qt::RoundCap);
  return path.united(stroker.createStroke(path));
}

inline void paintPopupShadowLayer(QPainter& painter, const QPainterPath& basePath,
                                  const QPointF& offset, qreal blur, qreal spread,
                                  const QColor& color) {
  if (color.alpha() <= 0) {
    return;
  }

  const qreal effectiveBlur = std::max<qreal>(1.0, blur + std::min<qreal>(0.0, spread));
  const qreal effectiveSpread = std::max<qreal>(0.0, spread);
  const int steps = std::max(1, static_cast<int>(std::ceil(effectiveBlur / 2.0)));
  const qreal stepAlpha = static_cast<double>(color.alphaF()) / static_cast<qreal>(steps);

  painter.save();
  painter.translate(offset);
  painter.setPen(Qt::NoPen);

  for (int step = steps; step >= 1; --step) {
    const qreal radius =
        effectiveSpread + effectiveBlur * static_cast<qreal>(step) / static_cast<qreal>(steps);
    QColor stepColor = color;
    stepColor.setAlphaF(static_cast<float>(std::clamp(stepAlpha, 0.0, 1.0)));
    painter.fillPath(expandedPopupShadowPath(basePath, radius), stepColor);
  }

  painter.restore();
}

inline void paintAntPopupBoxShadowSecondary(QPainter& painter, const QPainterPath& shapePath) {
  if (shapePath.isEmpty()) {
    return;
  }

  painter.save();
  paintPopupShadowLayer(painter, shapePath, QPointF(0.0, 6.0), 16.0, 0.0, QColor(0, 0, 0, 14));
  painter.restore();
}

}  // namespace adqt::widgets::detail

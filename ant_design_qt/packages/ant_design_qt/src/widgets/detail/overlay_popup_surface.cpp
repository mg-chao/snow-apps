#include "overlay_popup_surface.h"

#include "popup_shadow.h"

#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRegion>
#include <QResizeEvent>

#include <algorithm>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#include <windowsx.h>
#endif

namespace adqt::widgets::detail {

OverlayPopupSurface::OverlayPopupSurface(QWidget* parent) : QWidget(parent) {
  setAttribute(Qt::WA_Hover, true);
  setAutoFillBackground(false);

  bodyWidget_ = new QWidget(this);
}

QMargins OverlayPopupSurface::shadowMargins() const { return antPopupShadowSecondaryMargins(); }

void OverlayPopupSurface::setSurfaceStyle(const OverlayPopupSurfaceStyle& style) {
  const bool unchanged = style_.background == style.background &&
                         style_.borderColor == style.borderColor &&
                         style_.arrowBackground == style.arrowBackground &&
                         style_.arrowBorderColor == style.arrowBorderColor &&
                         style_.metrics.borderRadius == style.metrics.borderRadius &&
                         style_.metrics.borderWidth == style.metrics.borderWidth &&
                         style_.metrics.arrowSize == style.metrics.arrowSize;
  if (unchanged) {
    return;
  }
  style_ = style;
  invalidatePathCache();
  updateBodyGeometry();
  updateGeometry();
  update();
}

void OverlayPopupSurface::setArrowVisible(bool visible) {
  if (arrowVisible_ == visible) {
    return;
  }
  arrowVisible_ = visible;
  invalidatePathCache();
  updateBodyGeometry();
  updateGeometry();
  update();
}

void OverlayPopupSurface::setPlacement(OverlayPopupPlacement placement) {
  if (placement_ == placement) {
    return;
  }
  placement_ = placement;
  arrowSide_ = arrowSideForPlacement(placement_);
  invalidatePathCache();
  updateBodyGeometry();
  updateGeometry();
  update();
}

void OverlayPopupSurface::setArrowCenter(qreal center) {
  if (qFuzzyCompare(static_cast<double>(arrowCenter_), static_cast<double>(center))) {
    return;
  }
  arrowCenter_ = center;
  invalidatePathCache();
  update();
}

QSize OverlayPopupSurface::visualSizeHint() const {
  const QSize bodyHint = bodyWidget_ ? bodyWidget_->sizeHint() : QSize(120, 32);
  const int arrowSize = arrowProjection();
  const int widthPadding =
      (arrowSide_ == ArrowSide::Left || arrowSide_ == ArrowSide::Right) ? arrowSize : 0;
  const int heightPadding =
      (arrowSide_ == ArrowSide::Top || arrowSide_ == ArrowSide::Bottom) ? arrowSize : 0;
  return QSize(std::max(1, bodyHint.width() + widthPadding),
               std::max(1, bodyHint.height() + heightPadding));
}

QSize OverlayPopupSurface::sizeHint() const { return addAntPopupShadowMargins(visualSizeHint()); }

bool OverlayPopupSurface::containsInteractiveLocalPos(const QPointF& pos) const {
  ensurePathCache();
  return pathCache_ && pathCache_->valid && !pathCache_->interactive.isEmpty() &&
         pathCache_->interactive.contains(pos);
}

bool OverlayPopupSurface::containsInteractiveGlobalPos(const QPoint& pos) const {
  return isVisible() && containsInteractiveLocalPos(mapFromGlobal(pos));
}

bool OverlayPopupSurface::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
#if defined(Q_OS_WIN) || defined(_WIN32)
  auto* nativeMessage = static_cast<MSG*>(message);
  if (nativeMessage && nativeMessage->message == WM_NCHITTEST) {
    RECT nativeRect{};
    const bool hasNativeRect = GetWindowRect(nativeMessage->hwnd, &nativeRect) != FALSE;
    const int nativeWidth = nativeRect.right - nativeRect.left;
    const int nativeHeight = nativeRect.bottom - nativeRect.top;
    const QPointF localPos(
        hasNativeRect && nativeWidth > 0
            ? static_cast<qreal>(GET_X_LPARAM(nativeMessage->lParam) - nativeRect.left) *
                  static_cast<qreal>(width()) / static_cast<qreal>(nativeWidth)
            : -1.0,
        hasNativeRect && nativeHeight > 0
            ? static_cast<qreal>(GET_Y_LPARAM(nativeMessage->lParam) - nativeRect.top) *
                  static_cast<qreal>(height()) / static_cast<qreal>(nativeHeight)
            : -1.0);
    if (!containsInteractiveLocalPos(localPos)) {
      if (result) {
        *result = HTTRANSPARENT;
      }
      return true;
    }
  }
#else
  Q_UNUSED(eventType)
  Q_UNUSED(message)
  Q_UNUSED(result)
#endif
  return QWidget::nativeEvent(eventType, message, result);
}

void OverlayPopupSurface::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  invalidatePathCache();
  updateBodyGeometry();
}

void OverlayPopupSurface::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)

  ensurePathCache();
  ensureShadowCache();
  QPainter painter(this);
  paintSurface(painter);
}

void OverlayPopupSurface::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  pathCache_ = std::make_unique<PathCache>();
  shadowCache_ = std::make_unique<ShadowCache>();
  ensurePathCache();
  ensureShadowCache();
}

void OverlayPopupSurface::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  shadowCache_.reset();
  pathCache_.reset();
}

void OverlayPopupSurface::invalidatePathCache() const {
  if (pathCache_) {
    pathCache_->valid = false;
    pathCache_->size = QSize();
    pathCache_->bubble = QPainterPath();
    pathCache_->arrow = QPainterPath();
    pathCache_->interactive = QPainterPath();
  }
  invalidateShadowCache();
}

void OverlayPopupSurface::invalidateShadowCache() const {
  if (!shadowCache_) {
    return;
  }
  shadowCache_->valid = false;
  shadowCache_->paths.clear();
}

void OverlayPopupSurface::ensurePathCache() const {
  if (!isVisible() || !pathCache_) {
    return;
  }

  const QSize logicalSize = size();
  if (pathCache_->valid && pathCache_->size == logicalSize) {
    return;
  }

  pathCache_->valid = false;
  pathCache_->size = QSize();
  pathCache_->bubble = QPainterPath();
  pathCache_->arrow = QPainterPath();
  pathCache_->interactive = QPainterPath();
  invalidateShadowCache();
  if (logicalSize.width() <= 0 || logicalSize.height() <= 0) {
    return;
  }

  const QRectF bubbleRect = bubbleRectForPaint();
  if (!bubbleRect.isValid()) {
    return;
  }

  pathCache_->bubble.addRoundedRect(bubbleRect, style_.metrics.borderRadius,
                                     style_.metrics.borderRadius);
  const QPolygonF arrow = arrowPolygon(bubbleRect);
  if (!arrow.isEmpty()) {
    pathCache_->arrow.addPolygon(arrow);
  }

  QPainterPath interactivePath = pathCache_->bubble;
  if (!pathCache_->arrow.isEmpty()) {
    interactivePath = interactivePath.united(pathCache_->arrow);
  }
  pathCache_->interactive = interactivePath;

  pathCache_->size = logicalSize;
  pathCache_->valid = true;
}

void OverlayPopupSurface::ensureShadowCache() const {
  if (!isVisible() || !shadowCache_) {
    return;
  }
  ensurePathCache();
  if (!pathCache_ || !pathCache_->valid || pathCache_->interactive.isEmpty()) {
    return;
  }
  if (shadowCache_->valid) {
    return;
  }

  constexpr qreal kShadowBlur = 16.0;
  constexpr int kShadowSteps = 8;
  shadowCache_->paths.clear();
  shadowCache_->paths.reserve(kShadowSteps);
  for (int step = kShadowSteps; step >= 1; --step) {
    const qreal radius = kShadowBlur * static_cast<qreal>(step) / static_cast<qreal>(kShadowSteps);
    shadowCache_->paths.append(expandedPopupShadowPath(pathCache_->interactive, radius));
  }
  shadowCache_->valid = true;
}

void OverlayPopupSurface::paintSurface(QPainter& painter) const {
  painter.setRenderHint(QPainter::Antialiasing, true);

  if (!pathCache_ || !pathCache_->valid || pathCache_->bubble.isEmpty()) {
    return;
  }

  constexpr int kShadowSteps = 8;
  const QColor shadowColor(0, 0, 0, 14);
  const float stepAlpha = shadowColor.alphaF() / static_cast<float>(kShadowSteps);
  painter.save();
  painter.translate(QPointF(0.0, 6.0));
  painter.setPen(Qt::NoPen);
  if (shadowCache_ && shadowCache_->valid) {
    for (const QPainterPath& shadowPath : shadowCache_->paths) {
      QColor stepColor = shadowColor;
      stepColor.setAlphaF(std::clamp(stepAlpha, 0.0F, 1.0F));
      painter.fillPath(shadowPath, stepColor);
    }
  }
  painter.restore();

  const QColor resolvedArrowBackground =
      style_.arrowBackground.isValid() ? style_.arrowBackground : style_.background;
  const QColor resolvedArrowBorder =
      style_.arrowBorderColor.isValid() ? style_.arrowBorderColor : style_.borderColor;
  const bool unifiedArrowFill =
      pathCache_->arrow.isEmpty() || resolvedArrowBackground == style_.background;
  const bool unifiedArrowBorder =
      pathCache_->arrow.isEmpty() || resolvedArrowBorder == style_.borderColor;

  if (unifiedArrowFill) {
    QPainterPath fillPath = pathCache_->bubble;
    if (!pathCache_->arrow.isEmpty()) {
      fillPath = fillPath.united(pathCache_->arrow);
    }
    painter.fillPath(fillPath, style_.background);
  } else {
    painter.fillPath(pathCache_->bubble, style_.background);
    painter.fillPath(pathCache_->arrow, resolvedArrowBackground);
  }

  if (style_.metrics.borderWidth <= 0) {
    return;
  }

  if (unifiedArrowBorder) {
    if (style_.borderColor.alpha() <= 0) {
      return;
    }
    QPainterPath strokePath = pathCache_->bubble;
    if (!pathCache_->arrow.isEmpty()) {
      strokePath = strokePath.united(pathCache_->arrow);
    }
    QPen pen(style_.borderColor, style_.metrics.borderWidth);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(strokePath);
    return;
  }

  painter.setBrush(Qt::NoBrush);
  if (style_.borderColor.alpha() > 0) {
    QPen pen(style_.borderColor, style_.metrics.borderWidth);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.drawPath(pathCache_->bubble);
  }
  if (!pathCache_->arrow.isEmpty() && resolvedArrowBorder.alpha() > 0) {
    QPen pen(resolvedArrowBorder, style_.metrics.borderWidth);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.drawPath(pathCache_->arrow);
  }
}

OverlayPopupSurface::ArrowSide OverlayPopupSurface::arrowSideForPlacement(
    OverlayPopupPlacement placement) {
  switch (placement) {
    case OverlayPopupPlacement::Top:
    case OverlayPopupPlacement::TopLeft:
    case OverlayPopupPlacement::TopRight:
      return ArrowSide::Bottom;
    case OverlayPopupPlacement::Bottom:
    case OverlayPopupPlacement::BottomLeft:
    case OverlayPopupPlacement::BottomRight:
      return ArrowSide::Top;
    case OverlayPopupPlacement::Left:
    case OverlayPopupPlacement::LeftTop:
    case OverlayPopupPlacement::LeftBottom:
      return ArrowSide::Right;
    case OverlayPopupPlacement::Right:
    case OverlayPopupPlacement::RightTop:
    case OverlayPopupPlacement::RightBottom:
      return ArrowSide::Left;
  }
  return ArrowSide::None;
}

int OverlayPopupSurface::arrowProjection() const {
  return arrowVisible_ ? std::max(0, style_.metrics.arrowSize) : 0;
}

qreal OverlayPopupSurface::arrowBaseHalfWidth() const {
  return static_cast<qreal>(arrowProjection());
}

qreal OverlayPopupSurface::arrowBaseInsetForUnion() const {
  return std::max(1.0, static_cast<qreal>(style_.metrics.borderWidth));
}

QRectF OverlayPopupSurface::bubbleRectForPaint() const {
  const int arrow = arrowProjection();
  const QMargins margins = shadowMargins();
  QRectF rectf(rect());
  rectf.adjust(margins.left(), margins.top(), -margins.right(), -margins.bottom());
  rectf.adjust(0.5, 0.5, -0.5, -0.5);
  switch (arrowSide_) {
    case ArrowSide::Top:
      rectf.adjust(0.0, arrow, 0.0, 0.0);
      break;
    case ArrowSide::Bottom:
      rectf.adjust(0.0, 0.0, 0.0, -arrow);
      break;
    case ArrowSide::Left:
      rectf.adjust(arrow, 0.0, 0.0, 0.0);
      break;
    case ArrowSide::Right:
      rectf.adjust(0.0, 0.0, -arrow, 0.0);
      break;
    case ArrowSide::None:
      break;
  }
  return rectf;
}

qreal OverlayPopupSurface::clampedArrowCenter(const QRectF& bubbleRect) const {
  const int projection = arrowProjection();
  if (projection <= 0) {
    return 0.0;
  }
  const qreal half = arrowBaseHalfWidth();
  const qreal radius = std::max(0.0, static_cast<qreal>(style_.metrics.borderRadius));
  if (arrowSide_ == ArrowSide::Top || arrowSide_ == ArrowSide::Bottom) {
    const qreal minX = bubbleRect.left() + radius + half + 1.0;
    const qreal maxX = bubbleRect.right() - radius - half - 1.0;
    if (minX > maxX) {
      return bubbleRect.center().x();
    }
    const qreal fallback = bubbleRect.center().x();
    const qreal target =
        (arrowCenter_ > 0.0) ? static_cast<qreal>(shadowMargins().left()) + arrowCenter_ : fallback;
    return std::clamp(target, minX, maxX);
  }
  if (arrowSide_ == ArrowSide::Left || arrowSide_ == ArrowSide::Right) {
    const qreal minY = bubbleRect.top() + radius + half + 1.0;
    const qreal maxY = bubbleRect.bottom() - radius - half - 1.0;
    if (minY > maxY) {
      return bubbleRect.center().y();
    }
    const qreal fallback = bubbleRect.center().y();
    const qreal target =
        (arrowCenter_ > 0.0) ? static_cast<qreal>(shadowMargins().top()) + arrowCenter_ : fallback;
    return std::clamp(target, minY, maxY);
  }
  return 0.0;
}

QPolygonF OverlayPopupSurface::arrowPolygon(const QRectF& bubbleRect) const {
  const int projection = arrowProjection();
  if (projection <= 0) {
    return {};
  }

  const qreal center = clampedArrowCenter(bubbleRect);
  const qreal half = arrowBaseHalfWidth();
  const qreal baseInset = arrowBaseInsetForUnion();
  QPolygonF polygon;
  switch (arrowSide_) {
    case ArrowSide::Top:
      polygon << QPointF(center, bubbleRect.top() - projection)
              << QPointF(center - half, bubbleRect.top() + baseInset)
              << QPointF(center + half, bubbleRect.top() + baseInset);
      break;
    case ArrowSide::Bottom:
      polygon << QPointF(center, bubbleRect.bottom() + projection)
              << QPointF(center - half, bubbleRect.bottom() - baseInset)
              << QPointF(center + half, bubbleRect.bottom() - baseInset);
      break;
    case ArrowSide::Left:
      polygon << QPointF(bubbleRect.left() - projection, center)
              << QPointF(bubbleRect.left() + baseInset, center - half)
              << QPointF(bubbleRect.left() + baseInset, center + half);
      break;
    case ArrowSide::Right:
      polygon << QPointF(bubbleRect.right() + projection, center)
              << QPointF(bubbleRect.right() - baseInset, center - half)
              << QPointF(bubbleRect.right() - baseInset, center + half);
      break;
    case ArrowSide::None:
      break;
  }
  return polygon;
}

void OverlayPopupSurface::updateBodyGeometry() {
  if (!bodyWidget_) {
    return;
  }
  const int arrow = arrowProjection();
  const QMargins margins = shadowMargins();
  int left = margins.left();
  int top = margins.top();
  int right = margins.right();
  int bottom = margins.bottom();
  switch (arrowSide_) {
    case ArrowSide::Top:
      top += arrow;
      break;
    case ArrowSide::Bottom:
      bottom += arrow;
      break;
    case ArrowSide::Left:
      left += arrow;
      break;
    case ArrowSide::Right:
      right += arrow;
      break;
    case ArrowSide::None:
      break;
  }

  const QRect bodyRect = rect().adjusted(left, top, -right, -bottom);
  bodyWidget_->setGeometry(bodyRect);
  const int radius = std::max(0, style_.metrics.borderRadius);
  if (radius <= 0 || bodyRect.isEmpty()) {
    bodyWidget_->clearMask();
    return;
  }

  QPainterPath clipPath;
  clipPath.addRoundedRect(QRectF(QPointF(0, 0), QSizeF(bodyRect.size())), radius, radius);
  bodyWidget_->setMask(QRegion(clipPath.toFillPolygon().toPolygon()));
}

}  // namespace adqt::widgets::detail

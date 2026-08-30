#include "floating_surface.h"

#include <QCache>
#include <QMutex>
#include <QMutexLocker>
#include <QLayout>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QResizeEvent>

#include <array>
#include <algorithm>
#include <cmath>

namespace adqt::widgets {
namespace {

struct ShadowTile {
  QPixmap pixmap;
  qreal slice = 0.0;
};

QString shadowKey(qreal radius, qreal blur, const QPointF& offset, const QColor& color, qreal dpr) {
  return QStringLiteral("%1:%2:%3:%4:%5:%6")
      .arg(qRound(radius * dpr))
      .arg(qRound(blur * dpr))
      .arg(qRound(offset.x() * dpr))
      .arg(qRound(offset.y() * dpr))
      .arg(color.rgba(), 8, 16, QLatin1Char('0'))
      .arg(qRound(dpr * 1000));
}

QMargins shadowMarginsFor(qreal blur, const QPointF& offset) {
  return QMargins(std::max(0, qCeil(blur - offset.x())), std::max(0, qCeil(blur - offset.y())),
                  std::max(0, qCeil(blur + offset.x())), std::max(0, qCeil(blur + offset.y())));
}

ShadowTile createShadowTile(qreal radius, qreal blur, const QPointF& offset, const QColor& color,
                            qreal dpr) {
  const int physicalRadius = std::max(0, qRound(radius * dpr));
  const int physicalBlur = std::max(0, qRound(blur * dpr));
  const QPoint physicalOffset(qRound(offset.x() * dpr), qRound(offset.y() * dpr));
  const int physicalSlice =
      std::max(1, physicalRadius + physicalBlur +
                      std::max(std::abs(physicalOffset.x()), std::abs(physicalOffset.y())) + 2);
  const int side = physicalSlice * 2 + 1;
  QPixmap pixmap(side, side);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(Qt::NoPen);

  const int steps = std::max(1, physicalBlur);
  for (int step = steps; step >= 1; --step) {
    const qreal expansion = static_cast<qreal>(physicalBlur) * step / steps;
    QColor layer = color;
    layer.setAlpha(
        std::max(1, qRound(static_cast<qreal>(color.alpha()) / static_cast<qreal>(steps + 1))));
    const QRectF core(physicalSlice, physicalSlice, 1.0, 1.0);
    const QRectF shadowRect =
        core.adjusted(-expansion, -expansion, expansion, expansion).translated(physicalOffset);
    painter.setBrush(layer);
    painter.drawRoundedRect(shadowRect, physicalRadius + expansion, physicalRadius + expansion);
  }
  painter.setBrush(color);
  painter.drawRoundedRect(QRectF(physicalSlice, physicalSlice, 1.0, 1.0).translated(physicalOffset),
                          physicalRadius, physicalRadius);
  pixmap.setDevicePixelRatio(dpr);
  return ShadowTile{pixmap, static_cast<qreal>(physicalSlice) / dpr};
}

ShadowTile uncachedShadowTile(qreal radius, qreal blur, const QPointF& offset,
                              const QColor& color, qreal dpr) {
  return createShadowTile(radius, blur, offset, color, dpr);
}

}  // namespace

struct AdFloatingSurface::ShadowCache {
  QCache<QString, ShadowTile> tiles{16 * 1024};
  QMutex mutex;
};

namespace {

template <typename Cache>
ShadowTile cachedShadowTile(Cache& cache, qreal radius, qreal blur, const QPointF& offset,
                            const QColor& color, qreal dpr) {
  const QString key = shadowKey(radius, blur, offset, color, dpr);
  {
    QMutexLocker lock(&cache.mutex);
    if (ShadowTile* cached = cache.tiles.object(key)) return *cached;
  }
  ShadowTile rendered = createShadowTile(radius, blur, offset, color, dpr);
  QMutexLocker lock(&cache.mutex);
  if (ShadowTile* cached = cache.tiles.object(key)) return *cached;
  const int cost = std::max(1, rendered.pixmap.width() * rendered.pixmap.height() * 4 / 1024);
  cache.tiles.insert(key, new ShadowTile(rendered), cost);
  return rendered;
}

void drawNineSlice(QPainter& painter, const QRectF& target, const QMargins& targetMargins,
                   const ShadowTile& tile) {
  if (tile.pixmap.isNull() || target.isEmpty()) return;
  const qreal side = tile.pixmap.deviceIndependentSize().width();
  const qreal sourceSlice = std::min<qreal>(tile.slice, side / 2.0);
  const qreal sourceCenter =
      std::max<qreal>(1.0 / tile.pixmap.devicePixelRatio(), side - sourceSlice * 2.0);

  const auto fitSlices = [](qreal leading, qreal trailing, qreal extent) {
    const qreal total = leading + trailing;
    if (total <= extent || total <= 0.0) {
      return std::array<qreal, 2>{leading, trailing};
    }
    const qreal scale = extent / total;
    return std::array<qreal, 2>{leading * scale, trailing * scale};
  };
  const auto targetXS = fitSlices(std::max(0, targetMargins.left()),
                                  std::max(0, targetMargins.right()), target.width());
  const auto targetYS = fitSlices(std::max(0, targetMargins.top()),
                                  std::max(0, targetMargins.bottom()), target.height());
  const qreal xs[4] = {
      target.left(),
      target.left() + targetXS[0],
      target.right() - targetXS[1],
      target.right(),
  };
  const qreal ys[4] = {
      target.top(),
      target.top() + targetYS[0],
      target.bottom() - targetYS[1],
      target.bottom(),
  };
  const qreal sx[4] = {0.0, sourceSlice, sourceSlice + sourceCenter, side};
  const qreal sy[4] = {0.0, sourceSlice, sourceSlice + sourceCenter, side};
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 3; ++x) {
      const QRectF destination(xs[x], ys[y], xs[x + 1] - xs[x], ys[y + 1] - ys[y]);
      const QRectF source(sx[x], sy[y], sx[x + 1] - sx[x], sy[y + 1] - sy[y]);
      if (!destination.isEmpty() && !source.isEmpty())
        painter.drawPixmap(destination, tile.pixmap, source);
    }
  }
}

}  // namespace


AdFloatingSurface::AdFloatingSurface(QWidget* parent) : QWidget(parent) {
  setAttribute(Qt::WA_TranslucentBackground, true);
  setAutoFillBackground(false);
  body_ = new QWidget(this);
  body_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  body_->setAutoFillBackground(false);
  updateBodyGeometry();
}

AdFloatingSurface::~AdFloatingSurface() = default;

QWidget* AdFloatingSurface::contentBody() const { return body_; }
void AdFloatingSurface::setContentMargins(const QMargins& margins) {
  if (contentMargins_ == margins) return;
  contentMargins_ = margins;
  invalidateSurfaceGeometry();
}
QMargins AdFloatingSurface::contentMargins() const { return contentMargins_; }
qreal AdFloatingSurface::cornerRadius() const { return cornerRadius_; }
void AdFloatingSurface::setCornerRadius(qreal value) {
  value = std::max<qreal>(0.0, value);
  if (qFuzzyCompare(cornerRadius_ + 1.0, value + 1.0)) return;
  cornerRadius_ = value;
  update();
}
qreal AdFloatingSurface::borderWidth() const { return borderWidth_; }
void AdFloatingSurface::setBorderWidth(qreal value) {
  borderWidth_ = std::max<qreal>(0.0, value);
  update();
}
QColor AdFloatingSurface::backgroundColor() const { return backgroundColor_; }
void AdFloatingSurface::setBackgroundColor(const QColor& value) {
  backgroundColor_ = value;
  update();
}
QColor AdFloatingSurface::borderColor() const { return borderColor_; }
void AdFloatingSurface::setBorderColor(const QColor& value) {
  borderColor_ = value;
  update();
}

void AdFloatingSurface::setShadow(qreal blur, const QPointF& offset, const QColor& color) {
  blur = std::max<qreal>(0.0, blur);
  if (qFuzzyCompare(shadowBlurRadius_ + 1.0, blur + 1.0) && shadowOffset_ == offset &&
      shadowColor_ == color)
    return;
  shadowBlurRadius_ = blur;
  shadowOffset_ = offset;
  shadowColor_ = color;
  invalidateSurfaceGeometry();
}
qreal AdFloatingSurface::shadowBlurRadius() const { return shadowBlurRadius_; }
QPointF AdFloatingSurface::shadowOffset() const { return shadowOffset_; }
QColor AdFloatingSurface::shadowColor() const { return shadowColor_; }

QMargins AdFloatingSurface::shadowMargins() const {
  return shadowMarginsFor(shadowBlurRadius_, shadowOffset_);
}

QRect AdFloatingSurface::bodyRect() const {
  const QMargins margins = shadowMargins();
  return rect().adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
}

QRegion AdFloatingSurface::interactiveRegion() const {
  QPainterPath path;
  path.addRoundedRect(QRectF(bodyRect()), cornerRadius_, cornerRadius_);
  return QRegion(path.toFillPolygon().toPolygon());
}

bool AdFloatingSurface::containsInteractivePoint(const QPointF& point) const {
  QPainterPath path;
  path.addRoundedRect(QRectF(bodyRect()), cornerRadius_, cornerRadius_);
  return path.contains(point);
}

QSize AdFloatingSurface::sizeHint() const {
  const QSize contentHint = body_ && body_->layout() ? body_->layout()->sizeHint() : QSize(1, 1);
  const QMargins shadow = shadowMargins();
  return QSize(std::max(1, contentHint.width() + contentMargins_.left() + contentMargins_.right() +
                               shadow.left() + shadow.right()),
               std::max(1, contentHint.height() + contentMargins_.top() + contentMargins_.bottom() +
                               shadow.top() + shadow.bottom()));
}

void AdFloatingSurface::paintCachedShadow(QPainter& painter, const QRectF& bodyRect,
                                          qreal cornerRadius, qreal blurRadius,
                                          const QPointF& offset, const QColor& color,
                                          qreal devicePixelRatio) {
  if (bodyRect.isEmpty() || color.alpha() <= 0) return;
  const qreal normalizedBlur = std::max<qreal>(0.0, blurRadius);
  const QMargins margins = shadowMarginsFor(normalizedBlur, offset);
  const QRectF shadowRect =
      bodyRect.adjusted(-margins.left(), -margins.top(), margins.right(), margins.bottom());
  drawNineSlice(painter, shadowRect, margins,
                uncachedShadowTile(std::max<qreal>(0.0, cornerRadius), normalizedBlur, offset,
                                   color, std::max<qreal>(1.0, devicePixelRatio)));
}

void AdFloatingSurface::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  painter.fillRect(rect(), Qt::transparent);
  painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
  if (shadowColor_.alpha() > 0 && shadowBlurRadius_ >= 0.0) {
    const QRectF currentBodyRect = QRectF(bodyRect());
    const qreal normalizedBlur = std::max<qreal>(0.0, shadowBlurRadius_);
    const QMargins margins = shadowMarginsFor(normalizedBlur, shadowOffset_);
    const QRectF shadowRect = currentBodyRect.adjusted(-margins.left(), -margins.top(),
                                                       margins.right(), margins.bottom());
    if (shadowCache_) {
      drawNineSlice(
          painter, shadowRect, margins,
          cachedShadowTile(*shadowCache_, std::max<qreal>(0.0, cornerRadius_), normalizedBlur,
                           shadowOffset_, shadowColor_, std::max<qreal>(1.0, devicePixelRatioF())));
    } else {
      paintCachedShadow(painter, currentBodyRect, cornerRadius_, shadowBlurRadius_, shadowOffset_,
                        shadowColor_, devicePixelRatioF());
    }
  }
  const QRectF surfaceRect = QRectF(bodyRect())
                                 .adjusted(borderWidth_ / 2.0, borderWidth_ / 2.0,
                                           -borderWidth_ / 2.0, -borderWidth_ / 2.0);
  painter.setBrush(backgroundColor_);
  if (borderWidth_ > 0.0 && borderColor_.alpha() > 0)
    painter.setPen(QPen(borderColor_, borderWidth_));
  else
    painter.setPen(Qt::NoPen);
  painter.drawRoundedRect(surfaceRect, cornerRadius_, cornerRadius_);
}

void AdFloatingSurface::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  updateBodyGeometry();
}

void AdFloatingSurface::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  shadowCache_ = std::make_unique<ShadowCache>();
}

void AdFloatingSurface::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  shadowCache_.reset();
}

void AdFloatingSurface::updateBodyGeometry() {
  if (!body_) return;
  const QRect bodyRectWithPadding =
      bodyRect().adjusted(contentMargins_.left(), contentMargins_.top(), -contentMargins_.right(),
                          -contentMargins_.bottom());
  body_->setGeometry(bodyRectWithPadding);
}

void AdFloatingSurface::invalidateSurfaceGeometry() {
  updateBodyGeometry();
  updateGeometry();
  update();
}

}  // namespace adqt::widgets

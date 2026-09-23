#include "input_internal.h"
#include "detail/pointer_region.h"

#include <QEnterEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <QWidget>

#include <algorithm>

#include "antd_icons.h"
#include "interaction_overlay_manager.h"

namespace adqt::widgets::detail::input_internal {

InputIconButton::InputIconButton(QWidget* parent) : QToolButton(parent) {
  setAttribute(Qt::WA_Hover, true);
  setMouseTracking(true);
  setAutoRaise(false);
  setToolButtonStyle(Qt::ToolButtonIconOnly);
  setSizePolicy(QSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed));
}

void InputIconButton::setContentAlignment(Qt::Alignment alignment) {
  const Qt::Alignment normalized = alignment == 0 ? Qt::AlignCenter : alignment;
  if (contentAlignment_ == normalized) {
    return;
  }
  contentAlignment_ = normalized;
  update();
}

Qt::Alignment InputIconButton::contentAlignment() const { return contentAlignment_; }

void InputIconButton::setSlotSize(const QSize& size) {
  const QSize normalized(std::max(0, size.width()), std::max(0, size.height()));
  if (slotSize_ == normalized) {
    return;
  }
  slotSize_ = normalized;
  updateGeometry();
  update();
}

QSize InputIconButton::slotSize() const { return slotSize_; }

QSize InputIconButton::sizeHint() const { return effectiveSlotSize(); }

QSize InputIconButton::minimumSizeHint() const { return effectiveSlotSize(); }

bool InputIconButton::event(QEvent* event) {
  detail::resetWidgetHoverOnLifecycle(this, event);
  return QToolButton::event(event);
}

void InputIconButton::enterEvent(QEnterEvent* event) {
  update();
  QToolButton::enterEvent(event);
}

void InputIconButton::leaveEvent(QEvent* event) {
  update();
  QToolButton::leaveEvent(event);
}

void InputIconButton::changeEvent(QEvent* event) {
  QToolButton::changeEvent(event);
  if (event && event->type() == QEvent::EnabledChange) {
    update();
  }
}

void InputIconButton::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)
  QPainter painter(this);
  const QIcon currentIcon = icon();
  if (currentIcon.isNull()) {
    return;
  }

  const QIcon::Mode mode =
      !isEnabled() ? QIcon::Disabled
                   : (isDown() ? QIcon::Selected
                               : (detail::widgetHovered(this) ? QIcon::Active : QIcon::Normal));
  const QSize logicalSize = iconSize().isValid() ? iconSize() : QSize(16, 16);
  const QPixmap pixmap = currentIcon.pixmap(logicalSize, mode, QIcon::Off);
  if (pixmap.isNull()) {
    return;
  }

  const QSize pixmapSize = pixmap.deviceIndependentSize().toSize();
  int x = (width() - pixmapSize.width()) / 2;
  if (contentAlignment_ & Qt::AlignLeft) {
    x = 0;
  } else if (contentAlignment_ & Qt::AlignRight) {
    x = width() - pixmapSize.width();
  }

  int y = (height() - pixmapSize.height()) / 2;
  if (contentAlignment_ & Qt::AlignTop) {
    y = 0;
  } else if (contentAlignment_ & Qt::AlignBottom) {
    y = height() - pixmapSize.height();
  }

  const QPoint topLeft(std::max(0, x), std::max(0, y));
  painter.drawPixmap(topLeft, pixmap);
}

QSize InputIconButton::effectiveSlotSize() const {
  if (slotSize_.isValid()) {
    return slotSize_;
  }

  const QSize icon = iconSize().isValid() ? iconSize() : QSize(16, 16);
  return icon.expandedTo(QSize(1, 1));
}

bool iconRefsEqual(const adqt::icons::IconRef& lhs, const adqt::icons::IconRef& rhs) {
  return lhs == rhs;
}

QPoint mouseEventPos(const QMouseEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  return event ? event->position().toPoint() : QPoint();
#else
  return event ? event->pos() : QPoint();
#endif
}

bool isLeftMouseActivationEvent(const QEvent* event) {
  if (!event) {
    return false;
  }
  if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonDblClick) {
    return false;
  }
  const auto* mouseEvent = static_cast<const QMouseEvent*>(event);
  return mouseEvent->button() == Qt::LeftButton;
}

QPainterPath roundedRectPath(const QRectF& rect, qreal topLeft, qreal topRight, qreal bottomRight,
                             qreal bottomLeft) {
  const qreal w = std::max(rect.width(), 0.0);
  const qreal h = std::max(rect.height(), 0.0);
  const qreal maxRadius = std::min(w, h) / 2.0;

  topLeft = std::clamp(topLeft, 0.0, maxRadius);
  topRight = std::clamp(topRight, 0.0, maxRadius);
  bottomRight = std::clamp(bottomRight, 0.0, maxRadius);
  bottomLeft = std::clamp(bottomLeft, 0.0, maxRadius);

  const qreal left = rect.left();
  const qreal top = rect.top();
  const qreal right = left + rect.width();
  const qreal bottom = top + rect.height();

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

QColor parseThemeColor(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

QColor compositeOn(const QColor& foreground, const QColor& background) {
  if (!foreground.isValid()) {
    return background;
  }
  if (!background.isValid()) {
    QColor opaque = foreground;
    opaque.setAlpha(255);
    return opaque;
  }

  const float fgAlpha = std::clamp(foreground.alphaF(), 0.0F, 1.0F);
  QColor mixed;
  mixed.setRedF(foreground.redF() * fgAlpha + background.redF() * (1.0F - fgAlpha));
  mixed.setGreenF(foreground.greenF() * fgAlpha + background.greenF() * (1.0F - fgAlpha));
  mixed.setBlueF(foreground.blueF() * fgAlpha + background.blueF() * (1.0F - fgAlpha));
  mixed.setAlpha(255);
  return mixed;
}

QPixmap renderTintedIcon(const adqt::icons::IconRef& token, const QColor& color, int side,
                         qreal dpr) {
  if (!adqt::icons::isValid(token) || side <= 0) {
    return QPixmap();
  }
  adqt::icons::IconRef tinted = token;
  tinted = tinted.withColors(tinted.colors().withPrimary(color));
  return adqt::icons::renderIconPixmap(tinted, {QSize(side, side), dpr});
}

int boundedCursorPosition(const QString& text, int requested) {
  return std::clamp(requested, 0, static_cast<int>(text.size()));
}

int lineHeightForFont(const QFont& font) {
  QFontMetrics fm(font);
  return std::max(1, fm.lineSpacing());
}

QMargins textControlContentMargins(const adqt::widgets::detail::InputVisualStyle& style) {
  const int borderInset = std::max(0, style.metrics.borderWidth);
  const int horizontalPadding = std::max(0, style.metrics.horizontalPadding);
  const int verticalPadding = std::max(0, style.metrics.verticalPadding);

  // Keep a single source of truth for the frame-to-text inset. QLineEdit and
  // QTextEdit expose different geometry APIs, but they should resolve to the
  // same visual content box.
  return QMargins(borderInset + horizontalPadding, borderInset + verticalPadding,
                  borderInset + horizontalPadding, borderInset + verticalPadding);
}

int textAreaRowPaddingExtra(const adqt::widgets::detail::InputVisualStyle& style) {
  const int verticalPadding = std::max(0, style.metrics.verticalPadding);
  return std::max(0, verticalPadding * 2);
}

int textAreaScrollBarHoverThickness() {
  return kTextAreaScrollBarBaseThickness + std::max(1, kTextAreaScrollBarBaseThickness / 2);
}

QRectF joinedBorderRect(const QRect& bounds, qreal borderWidth, bool joinedLeft, bool joinedRight) {
  const qreal half = std::max<qreal>(0.0, borderWidth / 2.0);
  qreal leftInset = half + 0.5;
  qreal rightInset = half + 0.5;
  if (joinedLeft) {
    leftInset = half;
  }
  if (joinedRight) {
    rightInset = half;
  }
  return QRectF(bounds).adjusted(leftInset, half, -rightInset, -half);
}

qreal snapToDevicePixelCoord(qreal value, qreal dpr) {
  if (dpr <= 0.0) {
    return value;
  }
  return qRound(value * dpr) / dpr;
}

QRectF snapRectToDevicePixels(const QRectF& rect, qreal dpr) {
  if (dpr <= 0.0) {
    return rect;
  }

  const qreal left = snapToDevicePixelCoord(rect.left(), dpr);
  const qreal top = snapToDevicePixelCoord(rect.top(), dpr);
  const qreal right = snapToDevicePixelCoord(rect.left() + rect.width(), dpr);
  const qreal bottom = snapToDevicePixelCoord(rect.top() + rect.height(), dpr);
  const qreal minSize = 1.0 / dpr;

  return QRectF(left, top, std::max(minSize, right - left), std::max(minSize, bottom - top));
}

void paintInputFrame(QPainter* painter, const QRect& bounds, const InputFramePaintStyle& style) {
  if (!painter) {
    return;
  }

  const qreal borderWidth = std::max<qreal>(0.0, style.borderWidth);
  const bool hasVisibleBorder = borderWidth > 0.0 && style.border.alpha() > 0;
  const QRectF fillRect(bounds);
  const QRectF rawBorderRect =
      joinedBorderRect(bounds, borderWidth, style.joinedLeft, style.joinedRight);

  if (!fillRect.isValid() || fillRect.width() <= 0.0 || fillRect.height() <= 0.0) {
    return;
  }
  if (hasVisibleBorder &&
      (!rawBorderRect.isValid() || rawBorderRect.width() <= 0.0 || rawBorderRect.height() <= 0.0)) {
    return;
  }

  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, true);
  const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
  const QRectF borderRect = snapRectToDevicePixels(rawBorderRect, dpr);

  if (style.underlined) {
    if (style.background.alpha() > 0) {
      painter->fillRect(fillRect, style.background);
    }
    if (hasVisibleBorder) {
      QPen underlinePen(style.border, borderWidth, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
      painter->setPen(underlinePen);
      painter->setBrush(Qt::NoBrush);
      // Keep the underline inside the snapped border geometry so its endpoints and
      // stroke center align with the other input border treatments.
      const qreal underlineY = borderRect.bottom() - borderWidth / 2.0;
      painter->drawLine(QPointF(borderRect.left(), underlineY),
                        QPointF(borderRect.right(), underlineY));
    }
    painter->restore();
    return;
  }

  const QPainterPath fillPath = roundedRectPath(fillRect, style.topLeftRadius, style.topRightRadius,
                                                style.bottomRightRadius, style.bottomLeftRadius);

  if (style.background.alpha() > 0) {
    painter->fillPath(fillPath, style.background);
  }

  if (hasVisibleBorder) {
    const QPainterPath borderPath =
        roundedRectPath(borderRect, style.topLeftRadius, style.topRightRadius,
                        style.bottomRightRadius, style.bottomLeftRadius);
    QPen borderPen(style.border, borderWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
    painter->setPen(borderPen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(borderPath);
  }

  painter->restore();
}

QVector<PreservedScrollPosition> captureAncestorScrollPositions(QWidget* widget) {
  QVector<PreservedScrollPosition> positions;
  for (QWidget* current = widget ? widget->parentWidget() : nullptr; current != nullptr;
       current = current->parentWidget()) {
    auto* scrollArea = qobject_cast<QAbstractScrollArea*>(current);
    if (!scrollArea) {
      continue;
    }

    PreservedScrollPosition position;
    position.area = scrollArea;
    if (QScrollBar* bar = scrollArea->horizontalScrollBar()) {
      position.horizontalValue = bar->value();
    }
    if (QScrollBar* bar = scrollArea->verticalScrollBar()) {
      position.verticalValue = bar->value();
    }
    positions.push_back(position);
  }
  return positions;
}

void restoreAncestorScrollPositions(const QVector<PreservedScrollPosition>& positions) {
  for (const PreservedScrollPosition& position : positions) {
    if (!position.area) {
      continue;
    }
    if (QScrollBar* bar = position.area->horizontalScrollBar()) {
      bar->setValue(position.horizontalValue);
    }
    if (QScrollBar* bar = position.area->verticalScrollBar()) {
      bar->setValue(position.verticalValue);
    }
  }
}

void restoreAncestorScrollPositionsDeferred(QWidget* owner,
                                            const QVector<PreservedScrollPosition>& positions) {
  if (!owner || positions.isEmpty()) {
    return;
  }

  QPointer<QWidget> guardedOwner(owner);
  QTimer::singleShot(0, owner, [guardedOwner, positions]() {
    if (!guardedOwner) {
      return;
    }
    restoreAncestorScrollPositions(positions);
  });
}

void updateInputFocusOverlay(const QWidget* owner, const QRect& frameRect,
                             const adqt::widgets::detail::InputVisualStyle& style, bool joinedLeft,
                             bool joinedRight) {
  if (!owner || !owner->isEnabled() || !owner->isVisible() ||
      style.selectorFocusOutlineColor.alpha() <= 0 || style.metrics.focusOutlineWidth <= 0.0) {
    stopInteractionFocusForOwner(owner);
    return;
  }

  QWidget* hostWindow = owner->window();
  if (!hostWindow) {
    return;
  }

  const QPoint origin = owner->mapTo(hostWindow, frameRect.topLeft());
  QRectF baseRect = joinedBorderRect(QRect(origin, frameRect.size()), style.metrics.borderWidth,
                                     joinedLeft, joinedRight);

  InteractionFocusRequest request;
  request.owner = owner;
  request.baseRectInWindow = baseRect;
  const qreal focusRadius =
      style.underlined ? 0.0 : std::max<qreal>(0.0, style.metrics.borderRadius);
  request.topLeft = joinedLeft ? 0.0 : focusRadius;
  request.topRight = joinedRight ? 0.0 : focusRadius;
  request.bottomRight = joinedRight ? 0.0 : focusRadius;
  request.bottomLeft = joinedLeft ? 0.0 : focusRadius;
  request.color = style.selectorFocusOutlineColor;
  request.strokeWidth = std::max<qreal>(1.0, style.metrics.focusOutlineWidth);
  request.offset = std::max<qreal>(0.0, style.metrics.focusOutlineOffset);
  triggerInteractionFocus(request);
}

}  // namespace adqt::widgets::detail::input_internal

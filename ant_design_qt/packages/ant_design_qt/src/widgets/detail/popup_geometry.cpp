#include "popup_geometry.h"

#include "../popup_placement.h"

#include <QWidget>

namespace adqt::widgets::detail {

PopupWidgetRect PopupWidgetRect::whole(const QWidget* widget) {
  return {widget, widget ? widget->rect() : QRect()};
}

PopupWidgetRect PopupWidgetRect::visible() const {
  if (!widget || !widget->isVisible() || !rect.isValid()) {
    return {widget, {}};
  }
  QRect clipped = rect;
  const QWidget* ancestor = widget;
  while (ancestor) {
    if (!ancestor->isVisible()) {
      return {widget, {}};
    }
    const QPoint offset = widget->mapTo(ancestor, QPoint());
    clipped &= ancestor->rect().translated(-offset);
    if (!clipped.isValid() || ancestor->isWindow()) {
      break;
    }
    // A window's QObject parent owns its lifetime; it does not clip its pixels.
    ancestor = ancestor->parentWidget();
  }
  return {widget, clipped};
}

bool PopupWidgetRect::containsGlobalPos(const QPoint& point) const {
  return widget && rect.isValid() && rect.contains(widget->mapFromGlobal(point));
}

QRect PopupWidgetRect::mappedTo(const QWidget* target) const {
  if (!widget || !target || !rect.isValid()) {
    return {};
  }
  if (widget->window() == target->window()) {
    return QRect(widget->mapTo(target, rect.topLeft()), rect.size());
  }
  // Map one reference point, then scale offsets in the destination window's
  // coordinates. Mapping corners independently can choose different screens.
  const QPointF center = QRectF(rect).center();
  const QPointF mappedCenter = target->mapFromGlobal(widget->mapToGlobal(center));
  const qreal scale = widget->devicePixelRatioF() / target->devicePixelRatioF();
  const QSizeF size = QSizeF(rect.size()) * scale;
  return QRectF(mappedCenter - QPointF(size.width(), size.height()) / 2, size).toAlignedRect();
}

PopupScreenRect PopupWidgetRect::onScreen() const {
  if (!widget || !rect.isValid()) {
    return {};
  }
  const QPointF center = QRectF(rect).center();
  const QPointF globalCenter = widget->mapToGlobal(center);
  QScreen* screen = popupScreenForGlobalPos(widget, globalCenter.toPoint());
  if (!screen) {
    return {};
  }
  const qreal scale = widget->devicePixelRatioF() / screen->devicePixelRatio();
  const QSizeF size = QSizeF(rect.size()) * scale;
  return {screen,
          QRectF(globalCenter - QPointF(size.width(), size.height()) / 2, size).toAlignedRect()};
}

bool widgetContainsGlobalPos(const QWidget* widget, const QPoint& point) {
  return PopupWidgetRect::whole(widget).visible().containsGlobalPos(point);
}

}  // namespace adqt::widgets::detail

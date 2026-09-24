#pragma once

#include <QApplication>
#include <QEnterEvent>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QWidget>

#include <optional>

namespace adqt::widgets::detail {

// A visual subtree ends at a window boundary. A tool window's QObject parent
// owns its lifetime, but does not own its input or clip its pixels.
inline bool pointerInWidgetTree(const QWidget* target, const QWidget* root) {
  return target && root && target->window() == root->window() &&
         (target == root || root->isAncestorOf(target));
}

// Geometry-only presence is intentionally separate from pointer targeting.
// Click-through hints use presence; interactive controls must also check target.
inline bool pointerRegionContains(const QWidget* widget, const QRect& region,
                                  const QPoint& globalPosition) {
  if (!widget || !widget->isVisible() || !region.isValid()) return false;
  const QPoint local = widget->mapFromGlobal(globalPosition);
  if (!region.contains(local)) return false;
  for (const QWidget* ancestor = widget; ancestor; ancestor = ancestor->parentWidget()) {
    const QPoint point = widget->mapTo(ancestor, local);
    if (!ancestor->isVisible() || !ancestor->rect().contains(point) ||
        (!ancestor->mask().isEmpty() && !ancestor->mask().contains(point)))
      return false;
    if (ancestor->isWindow()) break;
  }
  return true;
}

inline bool pointerTargetEligible(const QWidget* target, const QWidget* root) {
  if (!pointerInWidgetTree(target, root) || !root->isVisible()) return false;
  for (const QWidget* widget = target; widget; widget = widget->parentWidget()) {
    if (widget->testAttribute(Qt::WA_TransparentForMouseEvents)) return false;
    if (widget->isWindow()) break;
  }
  const QWidget* modal = QApplication::activeModalWidget();
  const QWidget* popup = QApplication::activePopupWidget();
  return (!modal || modal == root->window() || modal->isAncestorOf(root)) &&
         (!popup || popup == root->window() || popup->isAncestorOf(root));
}

// Mouse/hover events can propagate to ancestors. Recover the input child within
// the event receiver, without replacing the event's coordinates with live input.
inline QWidget* pointerTargetWithin(QWidget* receiver, const QPoint& globalPosition) {
  if (!receiver) return nullptr;
  QWidget* child = receiver->childAt(receiver->mapFromGlobal(globalPosition));
  return child ? child : receiver;
}

inline std::optional<QPoint> pointerEventGlobalPosition(const QWidget* receiver,
                                                        const QEvent* event) {
  switch (event->type()) {
    case QEvent::Enter:
      return static_cast<const QEnterEvent*>(event)->globalPosition().toPoint();
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
      return static_cast<const QMouseEvent*>(event)->globalPosition().toPoint();
    case QEvent::HoverEnter:
    case QEvent::HoverMove:
      if (receiver)
        return receiver->mapToGlobal(static_cast<const QHoverEvent*>(event)->position().toPoint());
      break;
    default:
      break;
  }
  return std::nullopt;
}

// Qt owns ordinary control hover. Enabled/visible state gates presentation; it
// must not be remembered in a second enter/leave flag.
inline void resetWidgetHoverOnLifecycle(QWidget* widget, const QEvent* event) {
  // Retained popup children can hide without a matching Leave. Qt otherwise
  // carries WA_UnderMouse into the next visible session.
  if (event->type() == QEvent::Hide || event->type() == QEvent::ParentAboutToChange)
    widget->setAttribute(Qt::WA_UnderMouse, false);
}

inline bool widgetHovered(const QWidget* widget) {
  return widget && widget->isVisible() && widget->isEnabled() && widget->underMouse() &&
         !widget->testAttribute(Qt::WA_TransparentForMouseEvents);
}

}  // namespace adqt::widgets::detail

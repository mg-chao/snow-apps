#pragma once

#include <QPointer>
#include <QRect>
#include <QScreen>

class QWidget;

namespace adqt::widgets::detail {

// A placement rectangle expressed entirely in one screen's logical coordinates.
// This is a snapshot, not a desktop hit-test region. Qt's logical desktop mapping
// is piecewise scaled; a QWidget rectangle cannot be represented by one global QRect.
struct PopupScreenRect {
  QPointer<QScreen> screen;
  QRect rect;
  bool operator==(const PopupScreenRect&) const = default;
};

// Retain the coordinate owner until clipping and hit testing are complete.
// These values are short-lived views; callers retaining a snapshot use PopupScreenRect.
struct PopupWidgetRect {
  const QWidget* widget = nullptr;
  QRect rect;

  static PopupWidgetRect whole(const QWidget* widget);
  PopupWidgetRect visible() const;
  bool containsGlobalPos(const QPoint& point) const;
  QRect mappedTo(const QWidget* target) const;
  PopupScreenRect onScreen() const;
};

bool widgetContainsGlobalPos(const QWidget* widget, const QPoint& point);

}  // namespace adqt::widgets::detail

#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>

#include <QtGlobal>

class QWidget;
class QScreen;

namespace adqt::widgets::detail {

enum class PopupPlacement {
  BottomLeft,
  BottomRight,
  TopLeft,
  TopRight,
  RightTop,
  LeftTop,
  BottomCenter,
  TopCenter,
};

// All lengths and rectangles in an input must use the same coordinate space:
// the popup parent for child surfaces, or the selected screen for tool windows.
struct PopupPlacementInput {
  QPoint anchorTopLeft;
  QSize anchorSize;
  QSize popupSize;
  QRect bounds;
  PopupPlacement preferredPlacement = PopupPlacement::BottomLeft;
  QPoint offset;
  bool allowFallback = true;
};

struct PopupPlacementOutput {
  QPoint topLeft;
  PopupPlacement placement = PopupPlacement::BottomLeft;
};

enum class OverlayPopupPlacement {
  Top,
  TopLeft,
  TopRight,
  Bottom,
  BottomLeft,
  BottomRight,
  Left,
  LeftTop,
  LeftBottom,
  Right,
  RightTop,
  RightBottom,
};

struct OverlayPopupPlacementInput {
  QRect anchorRect;
  QSize popupSize;
  QRect bounds;
  OverlayPopupPlacement preferredPlacement = OverlayPopupPlacement::Top;
  int popupOffset = 0;
  bool allowFallback = true;
  bool pointAtCenter = false;
  int arrowOffsetHorizontal = 0;
  int arrowOffsetVertical = 0;
};

struct OverlayPopupPlacementOutput {
  QPoint topLeft;
  OverlayPopupPlacement placement = OverlayPopupPlacement::Top;
  qreal arrowCenterCoord = 0.0;
};

QWidget* resolvePopupScopeWindow(const QWidget* owner);
QScreen* popupScreenForGlobalRect(const QWidget* owner, const QRect& globalRect);
QScreen* popupScreenForGlobalPos(const QWidget* owner, const QPoint& globalPos);
QRect popupScreenBoundsInGlobal(const QWidget* owner, const QRect& globalRect);
QRect popupScreenBoundsInGlobal(const QWidget* owner, const QPoint& globalPos);
QRect popupScreenBoundsInGlobal(const QRect& globalRect);
QRect popupScreenBoundsInGlobal(const QPoint& globalPos);
QPoint clampPopupTopLeft(const QPoint& topLeft, const QSize& popupSize, const QRect& bounds);
PopupPlacement oppositePopupPlacement(PopupPlacement placement);
PopupPlacementOutput resolvePopupPlacement(const PopupPlacementInput& input);
OverlayPopupPlacement oppositeOverlayPopupPlacement(OverlayPopupPlacement placement);
OverlayPopupPlacementOutput resolveOverlayPopupPlacement(const OverlayPopupPlacementInput& input);

}  // namespace adqt::widgets::detail

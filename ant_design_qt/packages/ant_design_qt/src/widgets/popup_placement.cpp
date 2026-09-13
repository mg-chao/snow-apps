#include "popup_placement.h"

#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QWidget>

#include <algorithm>

namespace adqt::widgets::detail {

namespace {

QPoint placementTopLeft(PopupPlacement placement, const QPoint& anchorTopLeft,
                        const QSize& anchorSize, const QSize& popupSize) {
  const int anchorWidth = std::max(0, anchorSize.width());
  const int anchorHeight = std::max(0, anchorSize.height());
  const int popupWidth = std::max(1, popupSize.width());
  const int popupHeight = std::max(1, popupSize.height());

  switch (placement) {
    case PopupPlacement::BottomLeft:
      return QPoint(anchorTopLeft.x(), anchorTopLeft.y() + anchorHeight);
    case PopupPlacement::BottomRight:
      return QPoint(anchorTopLeft.x() + anchorWidth - popupWidth, anchorTopLeft.y() + anchorHeight);
    case PopupPlacement::BottomCenter:
      return QPoint(anchorTopLeft.x() + (anchorWidth - popupWidth) / 2,
                    anchorTopLeft.y() + anchorHeight);
    case PopupPlacement::TopLeft:
      return QPoint(anchorTopLeft.x(), anchorTopLeft.y() - popupHeight);
    case PopupPlacement::TopRight:
      return QPoint(anchorTopLeft.x() + anchorWidth - popupWidth, anchorTopLeft.y() - popupHeight);
    case PopupPlacement::TopCenter:
      return QPoint(anchorTopLeft.x() + (anchorWidth - popupWidth) / 2,
                    anchorTopLeft.y() - popupHeight);
    case PopupPlacement::RightTop:
      return QPoint(anchorTopLeft.x() + anchorWidth, anchorTopLeft.y());
    case PopupPlacement::LeftTop:
      return QPoint(anchorTopLeft.x() - popupWidth, anchorTopLeft.y());
  }
  return anchorTopLeft;
}

int overflowCost(const QPoint& topLeft, const QSize& popupSize, const QRect& bounds) {
  if (!bounds.isValid()) {
    return 0;
  }

  const int popupWidth = std::max(1, popupSize.width());
  const int popupHeight = std::max(1, popupSize.height());

  const int leftOverflow = std::max(0, bounds.left() - topLeft.x());
  const int topOverflow = std::max(0, bounds.top() - topLeft.y());
  const int rightOverflow = std::max(0, topLeft.x() + popupWidth - (bounds.right() + 1));
  const int bottomOverflow = std::max(0, topLeft.y() + popupHeight - (bounds.bottom() + 1));

  return leftOverflow + topOverflow + rightOverflow + bottomOverflow;
}

bool isVerticalOverlayPlacement(OverlayPopupPlacement placement) {
  switch (placement) {
    case OverlayPopupPlacement::Top:
    case OverlayPopupPlacement::TopLeft:
    case OverlayPopupPlacement::TopRight:
    case OverlayPopupPlacement::Bottom:
    case OverlayPopupPlacement::BottomLeft:
    case OverlayPopupPlacement::BottomRight:
      return true;
    default:
      return false;
  }
}

bool supportsOverlayCrossAxisAutoShift(OverlayPopupPlacement placement) {
  return placement == OverlayPopupPlacement::Top || placement == OverlayPopupPlacement::Bottom ||
         placement == OverlayPopupPlacement::Left || placement == OverlayPopupPlacement::Right;
}

QPoint overlayPlacementTopLeft(OverlayPopupPlacement placement, const QRect& anchorRect,
                               const QSize& popupSize) {
  const int popupWidth = std::max(1, popupSize.width());
  const int popupHeight = std::max(1, popupSize.height());
  switch (placement) {
    case OverlayPopupPlacement::Top:
      return QPoint(anchorRect.center().x() - popupWidth / 2, anchorRect.top() - popupHeight);
    case OverlayPopupPlacement::TopLeft:
      return QPoint(anchorRect.left(), anchorRect.top() - popupHeight);
    case OverlayPopupPlacement::TopRight:
      return QPoint(anchorRect.right() - popupWidth + 1, anchorRect.top() - popupHeight);
    case OverlayPopupPlacement::Bottom:
      return QPoint(anchorRect.center().x() - popupWidth / 2, anchorRect.bottom() + 1);
    case OverlayPopupPlacement::BottomLeft:
      return QPoint(anchorRect.left(), anchorRect.bottom() + 1);
    case OverlayPopupPlacement::BottomRight:
      return QPoint(anchorRect.right() - popupWidth + 1, anchorRect.bottom() + 1);
    case OverlayPopupPlacement::Left:
      return QPoint(anchorRect.left() - popupWidth, anchorRect.center().y() - popupHeight / 2);
    case OverlayPopupPlacement::LeftTop:
      return QPoint(anchorRect.left() - popupWidth, anchorRect.top());
    case OverlayPopupPlacement::LeftBottom:
      return QPoint(anchorRect.left() - popupWidth, anchorRect.bottom() - popupHeight + 1);
    case OverlayPopupPlacement::Right:
      return QPoint(anchorRect.right() + 1, anchorRect.center().y() - popupHeight / 2);
    case OverlayPopupPlacement::RightTop:
      return QPoint(anchorRect.right() + 1, anchorRect.top());
    case OverlayPopupPlacement::RightBottom:
      return QPoint(anchorRect.right() + 1, anchorRect.bottom() - popupHeight + 1);
  }
  return anchorRect.topLeft();
}

QPoint applyOverlayPopupOffset(OverlayPopupPlacement placement, QPoint point, int offset) {
  if (offset <= 0) {
    return point;
  }

  switch (placement) {
    case OverlayPopupPlacement::Top:
    case OverlayPopupPlacement::TopLeft:
    case OverlayPopupPlacement::TopRight:
      point.ry() -= offset;
      break;
    case OverlayPopupPlacement::Bottom:
    case OverlayPopupPlacement::BottomLeft:
    case OverlayPopupPlacement::BottomRight:
      point.ry() += offset;
      break;
    case OverlayPopupPlacement::Left:
    case OverlayPopupPlacement::LeftTop:
    case OverlayPopupPlacement::LeftBottom:
      point.rx() -= offset;
      break;
    case OverlayPopupPlacement::Right:
    case OverlayPopupPlacement::RightTop:
    case OverlayPopupPlacement::RightBottom:
      point.rx() += offset;
      break;
  }
  return point;
}

QPoint applyOverlayCrossAxisShift(OverlayPopupPlacement placement, QPoint point,
                                  const QSize& popupSize, const QRect& bounds) {
  if (!bounds.isValid()) {
    return point;
  }

  const int popupWidth = std::max(1, popupSize.width());
  const int popupHeight = std::max(1, popupSize.height());
  if (placement == OverlayPopupPlacement::Top || placement == OverlayPopupPlacement::Bottom) {
    const int minX = bounds.left();
    const int maxX = std::max(minX, bounds.right() - popupWidth + 1);
    point.setX(std::clamp(point.x(), minX, maxX));
  } else if (placement == OverlayPopupPlacement::Left ||
             placement == OverlayPopupPlacement::Right) {
    const int minY = bounds.top();
    const int maxY = std::max(minY, bounds.bottom() - popupHeight + 1);
    point.setY(std::clamp(point.y(), minY, maxY));
  }
  return point;
}

qreal overlayAnchorCoordForArrow(OverlayPopupPlacement placement, const QRect& anchorRect,
                                 bool pointAtCenter, int edgeInsetHorizontal,
                                 int edgeInsetVertical) {
  const int horizontalInset = std::max(0, edgeInsetHorizontal);
  const int verticalInset = std::max(0, edgeInsetVertical);
  switch (placement) {
    case OverlayPopupPlacement::Top:
    case OverlayPopupPlacement::Bottom:
      return anchorRect.center().x();
    case OverlayPopupPlacement::TopLeft:
    case OverlayPopupPlacement::BottomLeft:
      return pointAtCenter ? anchorRect.center().x()
                           : (anchorRect.left() +
                              std::min(horizontalInset, std::max(0, anchorRect.width() / 2)));
    case OverlayPopupPlacement::TopRight:
    case OverlayPopupPlacement::BottomRight:
      return pointAtCenter ? anchorRect.center().x()
                           : (anchorRect.right() -
                              std::min(horizontalInset, std::max(0, anchorRect.width() / 2)));
    case OverlayPopupPlacement::Left:
    case OverlayPopupPlacement::Right:
      return anchorRect.center().y();
    case OverlayPopupPlacement::LeftTop:
    case OverlayPopupPlacement::RightTop:
      return pointAtCenter ? anchorRect.center().y()
                           : (anchorRect.top() +
                              std::min(verticalInset, std::max(0, anchorRect.height() / 2)));
    case OverlayPopupPlacement::LeftBottom:
    case OverlayPopupPlacement::RightBottom:
      return pointAtCenter ? anchorRect.center().y()
                           : (anchorRect.bottom() -
                              std::min(verticalInset, std::max(0, anchorRect.height() / 2)));
  }
  return 0.0;
}

QScreen* fallbackScreen() {
  QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
  if (!screen) {
    screen = QGuiApplication::primaryScreen();
  }
  return screen;
}

QRect fallbackBounds() {
  QScreen* screen = fallbackScreen();
  return screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
}

}  // namespace

QWidget* resolvePopupScopeWindow(const QWidget* owner) {
  if (!owner) {
    return nullptr;
  }
  QWidget* scopeWindow = owner->window();
  return scopeWindow ? scopeWindow : const_cast<QWidget*>(owner);
}

QScreen* popupScreenForGlobalRect(const QWidget* owner, const QRect& globalRect) {
  QScreen* ownerScreen = owner ? owner->screen() : nullptr;
  const QList<QScreen*> candidateScreens =
      ownerScreen ? ownerScreen->virtualSiblings() : QGuiApplication::screens();

  if (globalRect.isValid()) {
    const QPoint center = globalRect.center();
    QScreen* centerScreen =
        ownerScreen ? ownerScreen->virtualSiblingAt(center) : QGuiApplication::screenAt(center);
    if (centerScreen) {
      return centerScreen;
    }

    // Match Qt's geometry-based window screen selection when the center is outside every screen.
    QScreen* intersectingScreen = nullptr;
    qint64 largestIntersectionArea = 0;
    for (QScreen* screen : candidateScreens) {
      if (!screen) {
        continue;
      }
      const QRect intersection = screen->geometry().intersected(globalRect);
      const qint64 intersectionArea =
          static_cast<qint64>(intersection.width()) * intersection.height();
      if (intersectionArea > largestIntersectionArea) {
        largestIntersectionArea = intersectionArea;
        intersectingScreen = screen;
      }
    }
    if (intersectingScreen) {
      return intersectingScreen;
    }
  }

  return ownerScreen ? ownerScreen : fallbackScreen();
}

QScreen* popupScreenForGlobalPos(const QWidget* owner, const QPoint& globalPos) {
  return popupScreenForGlobalRect(owner, QRect(globalPos, QSize(1, 1)));
}

QRect popupScreenBoundsInGlobal(const QWidget* owner, const QRect& globalRect) {
  QScreen* screen = popupScreenForGlobalRect(owner, globalRect);
  return screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
}

QRect popupScreenBoundsInGlobal(const QWidget* owner, const QPoint& globalPos) {
  return popupScreenBoundsInGlobal(owner, QRect(globalPos, QSize(1, 1)));
}

QRect popupScreenBoundsInGlobal(const QRect& globalRect) {
  return popupScreenBoundsInGlobal(nullptr, globalRect);
}

QRect popupScreenBoundsInGlobal(const QPoint& globalPos) {
  return popupScreenBoundsInGlobal(nullptr, globalPos);
}

QPoint clampPopupTopLeft(const QPoint& topLeft, const QSize& popupSize, const QRect& bounds) {
  if (!bounds.isValid()) {
    return topLeft;
  }

  const int popupWidth = std::max(1, popupSize.width());
  const int popupHeight = std::max(1, popupSize.height());

  const int minX = bounds.left();
  const int minY = bounds.top();
  const int maxX = std::max(minX, bounds.right() - popupWidth + 1);
  const int maxY = std::max(minY, bounds.bottom() - popupHeight + 1);
  return QPoint(std::clamp(topLeft.x(), minX, maxX), std::clamp(topLeft.y(), minY, maxY));
}

PopupPlacement oppositePopupPlacement(PopupPlacement placement) {
  switch (placement) {
    case PopupPlacement::BottomLeft:
      return PopupPlacement::TopLeft;
    case PopupPlacement::BottomRight:
      return PopupPlacement::TopRight;
    case PopupPlacement::BottomCenter:
      return PopupPlacement::TopCenter;
    case PopupPlacement::TopLeft:
      return PopupPlacement::BottomLeft;
    case PopupPlacement::TopRight:
      return PopupPlacement::BottomRight;
    case PopupPlacement::TopCenter:
      return PopupPlacement::BottomCenter;
    case PopupPlacement::RightTop:
      return PopupPlacement::LeftTop;
    case PopupPlacement::LeftTop:
      return PopupPlacement::RightTop;
  }
  return placement;
}

PopupPlacementOutput resolvePopupPlacement(const PopupPlacementInput& input) {
  PopupPlacementOutput out;
  out.placement = input.preferredPlacement;

  const QSize popupSize(std::max(1, input.popupSize.width()),
                        std::max(1, input.popupSize.height()));
  const QRect bounds = input.bounds.isValid() ? input.bounds : fallbackBounds();

  QPoint preferredTopLeft =
      placementTopLeft(input.preferredPlacement, input.anchorTopLeft, input.anchorSize, popupSize);
  preferredTopLeft += input.offset;

  QPoint selectedTopLeft = preferredTopLeft;

  if (input.allowFallback) {
    const PopupPlacement fallbackPlacement = oppositePopupPlacement(input.preferredPlacement);
    if (fallbackPlacement != input.preferredPlacement) {
      QPoint fallbackTopLeft =
          placementTopLeft(fallbackPlacement, input.anchorTopLeft, input.anchorSize, popupSize);
      fallbackTopLeft += input.offset;

      const int preferredCost = overflowCost(preferredTopLeft, popupSize, bounds);
      const int fallbackCost = overflowCost(fallbackTopLeft, popupSize, bounds);
      if (fallbackCost < preferredCost) {
        out.placement = fallbackPlacement;
        selectedTopLeft = fallbackTopLeft;
      }
    }
  }

  out.topLeft = clampPopupTopLeft(selectedTopLeft, popupSize, bounds);
  return out;
}

OverlayPopupPlacement oppositeOverlayPopupPlacement(OverlayPopupPlacement placement) {
  switch (placement) {
    case OverlayPopupPlacement::Top:
      return OverlayPopupPlacement::Bottom;
    case OverlayPopupPlacement::TopLeft:
      return OverlayPopupPlacement::BottomLeft;
    case OverlayPopupPlacement::TopRight:
      return OverlayPopupPlacement::BottomRight;
    case OverlayPopupPlacement::Bottom:
      return OverlayPopupPlacement::Top;
    case OverlayPopupPlacement::BottomLeft:
      return OverlayPopupPlacement::TopLeft;
    case OverlayPopupPlacement::BottomRight:
      return OverlayPopupPlacement::TopRight;
    case OverlayPopupPlacement::Left:
      return OverlayPopupPlacement::Right;
    case OverlayPopupPlacement::LeftTop:
      return OverlayPopupPlacement::RightTop;
    case OverlayPopupPlacement::LeftBottom:
      return OverlayPopupPlacement::RightBottom;
    case OverlayPopupPlacement::Right:
      return OverlayPopupPlacement::Left;
    case OverlayPopupPlacement::RightTop:
      return OverlayPopupPlacement::LeftTop;
    case OverlayPopupPlacement::RightBottom:
      return OverlayPopupPlacement::LeftBottom;
  }
  return placement;
}

OverlayPopupPlacementOutput resolveOverlayPopupPlacement(const OverlayPopupPlacementInput& input) {
  OverlayPopupPlacementOutput out;
  out.placement = input.preferredPlacement;

  const QSize popupSize(std::max(1, input.popupSize.width()),
                        std::max(1, input.popupSize.height()));
  const QRect bounds = input.bounds.isValid() ? input.bounds : fallbackBounds();

  auto computeForPlacement = [&](OverlayPopupPlacement placement) {
    QPoint pos = overlayPlacementTopLeft(placement, input.anchorRect, popupSize);
    pos = applyOverlayPopupOffset(placement, pos, input.popupOffset);
    if (supportsOverlayCrossAxisAutoShift(placement)) {
      pos = applyOverlayCrossAxisShift(placement, pos, popupSize, bounds);
    }
    return pos;
  };

  const QPoint preferredTopLeft = computeForPlacement(input.preferredPlacement);
  QPoint selectedTopLeft = preferredTopLeft;
  OverlayPopupPlacement selectedPlacement = input.preferredPlacement;

  if (input.allowFallback) {
    const OverlayPopupPlacement fallbackPlacement =
        oppositeOverlayPopupPlacement(input.preferredPlacement);
    const QPoint fallbackTopLeft = computeForPlacement(fallbackPlacement);
    if (overflowCost(fallbackTopLeft, popupSize, bounds) <
        overflowCost(preferredTopLeft, popupSize, bounds)) {
      selectedTopLeft = fallbackTopLeft;
      selectedPlacement = fallbackPlacement;
    }
  }

  out.topLeft = clampPopupTopLeft(selectedTopLeft, popupSize, bounds);
  out.placement = selectedPlacement;
  const QRect popupRect(out.topLeft, popupSize);
  out.arrowCenterCoord =
      overlayAnchorCoordForArrow(selectedPlacement, input.anchorRect, input.pointAtCenter,
                                 input.arrowOffsetHorizontal, input.arrowOffsetVertical) -
      (isVerticalOverlayPlacement(selectedPlacement) ? popupRect.left() : popupRect.top());
  return out;
}

}  // namespace adqt::widgets::detail

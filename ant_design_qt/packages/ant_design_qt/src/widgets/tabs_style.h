#pragma once

#include "tabs.h"

#include <QColor>
#include <QFont>
namespace adqt::widgets::detail {

struct TabsMetrics {
  int itemHeight = 46;
  int horizontalPadding = 0;
  int verticalPadding = 12;
  int itemGutter = 32;
  int cardGutter = 2;
  int indicatorThickness = 2;
  int borderWidth = 1;
  int borderRadius = 6;
  int iconSize = 16;
  int iconGap = 8;
  int operationExtent = 40;
  int focusOutlineWidth = 2;
  int focusOutlineOffset = 2;
  QFont font;
};

struct TabsAppearance {
  QColor item;
  QColor selected;
  QColor hover;
  QColor active;
  QColor disabled;
  QColor inkBar;
  QColor cardBackground;
  QColor cardActiveBackground;
  QColor border;
  QColor focusOutline;
  QColor surface;
  QColor popupText;
  QColor popupDisabledText;
  QColor popupHoverBackground;
  QColor popupClose;
  QFont popupFont;
  int popupRowHeight = 30;
  int popupHorizontalPadding = 12;
  int popupPadding = 4;
  int popupRadius = 8;
  int popupCloseSize = 12;
  TabsMetrics metrics;
  int motionDuration = 200;
};

TabsAppearance resolveTabsAppearance(const AdTabs* tabs, const AdTabs::ComponentTokens& tokens);

}  // namespace adqt::widgets::detail

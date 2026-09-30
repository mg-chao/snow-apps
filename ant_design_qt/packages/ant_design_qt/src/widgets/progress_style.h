#pragma once

#include "progress.h"

namespace adqt::theme {
struct ResolvedTheme;
}

namespace adqt::widgets::detail {

struct ProgressVisualStyle {
  QColor fill;
  QColor normalFill;
  QColor rail;
  QColor success;
  QColor exception;
  QColor text;
  QColor circleText;
  QColor innerText;
  QColor shimmer;
  QColor rootBackground;
  QColor bodyBackground;
  QColor indicatorBackground;
  QFont font;
  qreal lineHeight = 8;
  qreal lineRadius = 100;
  qreal circleTextSize = 24;
  qreal circleIconSize = 28;
  int animationDurationMs = 300;
  bool motion = true;
};

ProgressVisualStyle resolveProgressVisualStyle(const AdProgress* widget,
                                               const adqt::theme::ResolvedTheme& theme);

}  // namespace adqt::widgets::detail

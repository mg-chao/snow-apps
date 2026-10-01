#pragma once

#include "descriptions.h"

#include <QColor>
#include <QFont>

namespace adqt::widgets::detail {

struct DescriptionsMetrics {
  int titleMarginBottom = 20;
  int itemPaddingBottom = 16;
  int itemPaddingEnd = 16;
  int colonMarginLeft = 2;
  int colonMarginRight = 8;
  int borderedPaddingBlock = 16;
  int borderedPaddingInline = 24;
  int borderWidth = 1;
  int borderRadius = 8;
  int lineHeight = 22;
  QFont textFont;
  QFont titleFont;
};

struct DescriptionsAppearance {
  qreal backgroundOpacity = 1.0;
  QColor rootBackground;
  QColor labelBackground;
  QColor labelColor;
  QColor titleColor;
  QColor contentColor;
  QColor extraColor;
  QColor borderColor;
  DescriptionsMetrics metrics;
};

DescriptionsAppearance resolveDescriptionsAppearance(
    const AdDescriptions* descriptions, const AdDescriptions::ComponentTokens& componentTokens,
    const AdDescriptions::SemanticStyles& semanticStyles);

}  // namespace adqt::widgets::detail

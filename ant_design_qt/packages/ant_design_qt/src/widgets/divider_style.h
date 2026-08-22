#pragma once

#include "divider.h"

#include <QColor>
#include <QFont>
#include <QPalette>

namespace adqt::theme {
struct ResolvedTheme;
}

namespace adqt::widgets::detail {

struct DividerMetrics {
  qreal lineWidth = 1.0;
  int textPaddingInline = 16;
  qreal orientationMargin = 0.05;
  int verticalMarginInline = 8;
  int horizontalMarginSmall = 8;
  int horizontalMarginMiddle = 16;
  int horizontalMarginLarge = 24;
  int horizontalMarginWithText = 16;
  qreal verticalHeightFactor = 0.9;
  int textLineHeight = 24;
  int verticalHeight = 13;
  int contentMarginStart = 0;
  int contentMarginEnd = 0;
  QFont contentFont;
};

struct DividerAppearance {
  QColor rootBackground;
  QColor contentBackground;
  QColor lineColor;
  QColor textColor;
  DividerMetrics metrics;
};

struct DividerStyleInput {
  AdDivider::Orientation orientation = AdDivider::Orientation::Horizontal;
  AdDivider::Size size = AdDivider::Size::Large;
  AdDivider::TitlePlacement titlePlacement = AdDivider::TitlePlacement::Center;
  AdDivider::Variant variant = AdDivider::Variant::Solid;
  bool plain = false;
  bool hasContent = false;
  bool enabled = true;
  bool hasPaletteOverride = false;
  QPalette::ColorGroup paletteGroup = QPalette::Active;
  QPalette palette;
  QFont baseFont;
  AdDivider::ComponentTokens componentTokens;
  AdDivider::SemanticStyles semanticStyles;
};

DividerAppearance resolveDividerAppearance(const DividerStyleInput& input,
                                           const adqt::theme::ResolvedTheme& resolvedTheme);
DividerAppearance resolveDividerAppearance(const DividerStyleInput& input);

}  // namespace adqt::widgets::detail
